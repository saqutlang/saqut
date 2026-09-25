// ============================================================================
// saQut LSP — DocumentStore Gerçeklemesi
// ============================================================================

#include "lsp/document_store.hpp"
#include "lsp/uri.hpp"
#include "lsp/position.hpp"
#include "module/module_loader.hpp"
#include "symbol/symbol_collector.hpp"
#include "semantic/type_checker.hpp"
#include "semantic/structural_validator.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

DocumentState& DocumentStore::update(const std::string& uri,
                                      const std::string& content, int version) {
    auto it = store_.find(uri);
    if (it == store_.end()) {
        store_[uri] = std::make_unique<DocumentState>();
        it = store_.find(uri);
        it->second->uri = uri;
    }
    DocumentState& state = *it->second;

    // Bölüm 1 — son geçerli analiz: biten tur sözdizimi hatasızsa yeni tura
    // geçmeden sahipliğini snapshot'a devret (AST/token/tablo taşınır, kopya
    // yok). Bozuk turlar lastGood'u değiştirmez.
    if (state.syntaxOk && state.ast) {
        auto snap = std::make_unique<AnalysisSnapshot>();
        snap->content        = std::move(state.content);
        snap->lineStarts     = std::move(state.lineStarts);
        snap->filePath       = state.filePath;
        snap->ast            = state.ast;
        snap->tokens         = std::move(state.tokens);
        snap->symbolTable    = std::move(state.symbolTable);
        snap->symbolByOffset = std::move(state.symbolByOffset);
        state.ast = nullptr;
        state.tokens.clear();
        state.symbolTable = SymbolTable{};
        state.symbolByOffset.clear();
        state.lastGood = std::move(snap);
    }

    state.content    = content;
    state.lineStarts = buildLineStarts(content);
    state.version    = version;
    delete state.ast;
    state.ast = nullptr;
    for (auto* t : state.tokens) delete t;
    state.tokens.clear();
    runPipeline(state);
    return state;
}

void DocumentStore::reanalyze(DocumentState& state) {
    // İçerik aynı; yalnız bağımlılıklar değişti. Sözdizimi temiz tur
    // snapshot'a devredilmez (içerik değişmediği için yeni tur da aynı
    // sözdizimi sonucunu verir).
    delete state.ast;
    state.ast = nullptr;
    for (auto* t : state.tokens) delete t;
    state.tokens.clear();
    runPipeline(state);
}

std::vector<DocumentState*> DocumentStore::dependentsOf(const std::string& path,
                                                        const DocumentState* except) {
    std::vector<DocumentState*> out;
    for (auto& [uri, doc] : store_) {
        if (doc.get() == except) continue;
        if (std::find(doc->deps.begin(), doc->deps.end(), path) != doc->deps.end())
            out.push_back(doc.get());
    }
    // unordered_map sırası çalıştırmalar arasında kararsız — bildirim sırası
    // (golden testler) deterministik olsun.
    std::sort(out.begin(), out.end(),
              [](DocumentState* a, DocumentState* b) { return a->uri < b->uri; });
    return out;
}

std::vector<DocumentState*> DocumentStore::all() {
    std::vector<DocumentState*> out;
    for (auto& [uri, doc] : store_) out.push_back(doc.get());
    std::sort(out.begin(), out.end(),
              [](DocumentState* a, DocumentState* b) { return a->uri < b->uri; });
    return out;
}

DocumentState* DocumentStore::get(const std::string& uri) {
    auto it = store_.find(uri);
    return (it != store_.end()) ? it->second.get() : nullptr;
}

void DocumentStore::close(const std::string& uri) {
    store_.erase(uri);
}

bool DocumentStore::openContent(const std::string& path, std::string& out) const {
    for (auto& [uri, docState] : store_) {
        std::string docPath = fs::weakly_canonical(uriToPath(uri)).string();
        if (docPath == path) {
            out = docState->content;
            return true;
        }
    }
    return false;
}

std::string DocumentStore::contentForPath(const std::string& path) const {
    std::string out;
    if (openContent(path, out)) return out;

    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) return "";
    std::stringstream buf;
    buf << file.rdbuf();
    return buf.str();
}

std::string DocumentStore::uriForPath(const std::string& path) const {
    for (auto& [uri, docState] : store_) {
        std::string docPath = fs::weakly_canonical(uriToPath(uri)).string();
        if (docPath == path) return uri;
    }
    return pathToUri(path);
}

void DocumentStore::runPipeline(DocumentState& state) {
    state.diagnostics = DiagnosticEngine{};
    // NOT: state.symbolTable BURADA sıfırlanmaz. Faz 2 — parser artık panic-mode
    // recovery ile sözdizimi hatasında bile her zaman bir AST döndürür, bu yüzden
    // SymbolCollector normal şartlarda her turda çalışıp tabloyu yeniden kurar
    // (aşağıda). Tablo yalnızca modül hiç yüklenemediğinde (dosya bulunamadı vb.)
    // dokunulmadan kalır — LSP sorguları böylece bir önceki başarılı turun
    // ("son iyi") tablosuna düşmüş olur.

    std::string filePath = uriToPath(state.uri);

    // Overlay: derleme diski değil, açık olan editör buffer'larını görür.
    // Böylece A.sqt import ettiği B.sqt editörde açıksa, B'nin kaydedilmemiş
    // hali kullanılır (Faz 1, ADR: kaynak overlay). openContent aynı arama
    // mantığını contentForPath ile de paylaşır (Faz 3).
    ModuleLoader::SourceOverlay overlay =
        [this](const std::string& path, std::string& out) -> bool {
            return openContent(path, out);
        };

    ModuleRegistry registry;
    ModuleGraph    graph = ModuleLoader(registry, state.diagnostics, overlay)
                               .load(filePath);

    // Faz 2: modül hiç yüklenemediyse (örn. dosya bulunamadı — overlay ve disk
    // ikisi de başarısız) toplanacak bir AST yok; sembol tablosu bir önceki
    // başarılı turdan kalan haliyle bırakılır ("son iyi tablo").
    state.syntaxOk = false;
    if (graph.units.empty()) return;

    // ModuleLoader entryFilePath'i canonical hale getirir (fs::weakly_canonical);
    // SourceLocation.filePath'lerin hepsi bu biçimde. state.filePath'i ORADAN al
    // ki symbolByOffset filtrelemesi ve çok-dosya URI karşılaştırmaları eşleşsin.
    state.filePath = graph.units[0].filePath;
    state.deps.clear();
    for (auto& unit : graph.units) state.deps.push_back(unit.filePath);

    // Sözdizimi temizliği yalnız parse (E9xx) tanılarına bakar — tip hatası
    // olan kod da tamamlama için geçerli bir analizdir.
    state.syntaxOk = true;
    for (const auto& d : state.diagnostics.all())
        if (d.code.size() >= 2 && d.code[0] == 'E' && d.code[1] == '9' &&
            (d.loc.filePath().empty() || d.loc.filePath() == state.filePath))
            state.syntaxOk = false;

    state.symbolTable = SymbolTable{};
    SymbolCollector(state.symbolTable, state.diagnostics)
        .collectModuleGraph(graph);

    // Faz 3: (offset → Symbol*) indeksi — yalnızca BU belgenin dosyasına ait
    // tanım/referans konumları (kök neden #3: iki ayrı fonksiyondaki aynı adlı
    // değişken artık karışmaz, çünkü findSymbolAt artık isim-uzunluğu aralık
    // eşleştirmesi değil tam offset eşleşmesi kullanıyor).
    state.symbolByOffset.clear();
    for (Symbol* sym : state.symbolTable.allSymbols()) {
        if (sym->definitionLoc.isValid() && sym->definitionLoc.filePath() == state.filePath) {
            state.symbolByOffset[sym->definitionLoc.offset] = sym;
            // Faz 5 (#84): definitionLoc bildirim başını gösterir ("int deger"de
            // `int`) — bildirimdeki TANIMLAYICI token'ı da indeksle ki tanım
            // noktasında hover/rename/definition çalışsın. emplace: bir usage
            // aynı offset'e daha önce yazıldıysa ezme.
            int identOff = identOffsetFromDecl(state.content,
                                               sym->definitionLoc.offset, sym->name);
            if (identOff >= 0) state.symbolByOffset.emplace(identOff, sym);
        }
        for (const auto& ref : sym->references)
            if (ref.filePath() == state.filePath)
                state.symbolByOffset[ref.offset] = sym;
    }

    // Faz 2: erken dönüş YOK. Parser artık sözdizimi hatalarında bile
    // (panic-mode recovery ile) tam bir AST döndürdüğü için, bir hata olsa
    // dahi hatanın DIŞINDAKİ fonksiyonlar için hover/definition/documentSymbol
    // çalışmaya devam etsin diye TypeChecker/StructuralValidator'a kadar iniyoruz.
    // Bu katmanlar ErrorNode'u (default: dalı) sessizce atlar.
    for (auto& unit : graph.units)
        TypeChecker(state.symbolTable, state.diagnostics).check(unit.ast);
    for (auto& unit : graph.units)
        StructuralValidator(state.diagnostics).validate(unit.ast);

    // AST + token sahipliğini DocumentState'e aktar (Faz 3: findSymbolAt token
    // binary search'ü için tokenlere ihtiyaç duyar; IdentifierNode::lexerToken
    // bunlara işaret ettiği için ast ile aynı ömürde tutulmalılar).
    state.ast             = graph.units[0].ast;
    graph.units[0].ast    = nullptr;
    state.tokens          = std::move(graph.units[0].tokens);
}
