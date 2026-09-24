// ============================================================================
// saQut — ModuleLoader Gerçeklemesi
// ============================================================================

#include "module/module_loader.hpp"
#include "parser/nodes/declarations.hpp"
#include "tokenizer/tokenizer.hpp"
#include "parser/parser.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// load — giriş dosyasından başlayarak tüm bağımlılıkları yükle
// ─────────────────────────────────────────────────────────────────────────────

ModuleGraph ModuleLoader::load(const std::string& entryFilePath) {
    ModuleGraph graph;
    std::string canonical = fs::weakly_canonical(entryFilePath).string();
    loadUnit(canonical, graph);
    return graph;
}

// ─────────────────────────────────────────────────────────────────────────────
// loadUnit — tek bir dosyayı yükle, ImportDeclNode'larını takip et
// ─────────────────────────────────────────────────────────────────────────────

void ModuleLoader::loadUnit(const std::string& filePath, ModuleGraph& graph,
                            const SourceLocation& importLoc) {
    // Döngü tespiti (ADR-031): dosya kendi yükleme zincirinde tekrar
    // görünüyorsa döngüsel bağımlılık var — açık derleme hatası üret.
    // seen_ kontrolünden ÖNCE yapılmalı; aksi halde sessiz kısa devre olur.
    auto cycleStart = std::find(loadChain_.begin(), loadChain_.end(), filePath);
    if (cycleStart != loadChain_.end()) {
        std::string chain;
        for (auto it = cycleStart; it != loadChain_.end(); ++it)
            chain += fs::path(*it).filename().string() + " -> ";
        chain += fs::path(filePath).filename().string();
        diag_.report("E_MODULE_CYCLE", importLoc,
            "circular module dependency detected: " + chain);
        return;
    }

    if (seen_.count(filePath)) return;
    seen_.insert(filePath);

    // Kaynağı önce overlay'den dene (editör buffer'ı), yoksa diske düş.
    std::string source;
    bool haveSource = overlay_ && overlay_(filePath, source);
    if (!haveSource) {
        std::ifstream file(filePath, std::ios::in | std::ios::binary);
        if (!file.is_open()) {
            diag_.report("E_MODULE_NOT_FOUND", SourceLocation{},
                "cannot open module '" + filePath + "': file not found");
            return;
        }
        std::stringstream buf;
        buf << file.rdbuf();
        source = buf.str();
    }

    // Tokenize + parse
    Tokenizer tokenizer;
    std::vector<Token*> tokens;
    {
        Profiling::StageTimer::ScopedStage _prof(profiler_, "token");
        tokens = tokenizer.scan(source, filePath);
    }
    // --profile: bu dosyanın token sayısı "token" aşamasına eklenir
    // (çok-modüllü derlemede count() toplar). profiler_ nullptr ise no-op.
    if (profiler_) profiler_->count("token", static_cast<long long>(tokens.size()), "token");

    // Faz 2: diag_ enjekte edilir — sözdizimi hataları artık konumlu tanı
    // (E9xx) olarak DiagnosticEngine'e gider, parse yine de devam eder
    // (panic-mode recovery, bkz. Parser::synchronizeAndMakeError).
    Parser parser(&diag_);
    ASTNode* ast = nullptr;
    {
        Profiling::StageTimer::ScopedStage _prof(profiler_, "parser");
        ast = parser.parse(tokens);
    }
    if (!ast) {
        diag_.report("E_MODULE_PARSE", SourceLocation{},
            "failed to parse module '" + filePath + "'");
        for (auto* t : tokens) delete t;
        return;
    }

    // ModuleUnit oluştur ve graph'a ekle
    ModuleUnit unit;
    unit.filePath = filePath;
    unit.moduleId = registry_.intern(filePath);
    unit.ast      = ast;
    unit.tokens   = std::move(tokens);
    graph.units.push_back(std::move(unit));

    // ImportDeclNode'ları tara ve bağımlıları yükle. Bu dosya, bağımlıları
    // yüklenirken aktif zincirde kalır — döngü tespitinin temeli.
    loadChain_.push_back(filePath);
    for (ASTNode* child : ast->getChildren()) {
        if (child->kind != ASTKind::ImportDecl) continue;
        auto* imp = static_cast<ImportDeclNode*>(child);
        if (imp->sourcePath.empty()) continue;
        // ADR-034 (#107): tırnaksız import = gömülü FFI modülü, dosya grafiğinde
        // izlenmez — SymbolCollector::resolveFfiImport bunu FfiCatalog'dan çözer.
        if (imp->isModuleName) continue;

        std::string depPath = resolvePath(filePath, imp->sourcePath);
        imp->resolvedPath = depPath;
        loadUnit(depPath, graph, imp->loc);
    }
    loadChain_.pop_back();
}

// ─────────────────────────────────────────────────────────────────────────────
// resolvePath — import eden dosyanın dizinine göre canonical yol üret
// ─────────────────────────────────────────────────────────────────────────────

std::string ModuleLoader::resolvePath(const std::string& importerPath,
                                      const std::string& rawPath) {
    fs::path base   = fs::path(importerPath).parent_path();
    fs::path target = base / rawPath;
    return fs::weakly_canonical(target).string();
}
