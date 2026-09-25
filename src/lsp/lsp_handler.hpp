// ============================================================================
// saQut LSP — LspHandler (İstek Dispatch ve İşleme)
// ============================================================================
//
// DİZİN:   src/lsp/lsp_handler.hpp
// KATMAN:  LSP — Tüm LSP isteklerini dispatch eder
//
// AMAÇ:
//   initialize, hover, definition, references, documentSymbol,
//   completion, highlight isteklerini işler. Position encoding
//   anlaşması ve gruplanmış publishDiagnostics yapar.
//
// ============================================================================

#ifndef SAQUT_LSP_HANDLER
#define SAQUT_LSP_HANDLER

#include "vendor/nlohmann/json.hpp"
#include "lsp/document_store.hpp"
#include "lsp/json_rpc.hpp"
#include "lsp/project_index.hpp"
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

class LspHandler {
public:
    explicit LspHandler(std::ostream& out) : out_(out) {}

    nlohmann::json dispatch(const nlohmann::json& msg);

    // Sunucu döngüsü kuyruk boşken çağırır (lsp_server.cpp): proje indeksini
    // bir dosya ilerletir; ilk tarama bitince istemciden lens yenilemesi ister.
    bool hasIdleWork() const { return index_.hasPending(); }
    void idleStep();

    // LSP özellik düzeyi — eklenti bununla eski ikiliyi tanır (Bölüm 0).
    static constexpr int kLspLevel = 2;

private:
    std::ostream& out_;
    DocumentStore store_;
    ProjectIndex  index_;
    bool          shutdownRequested_ = false;
    // initializationOptions / workspace/didChangeConfiguration
    bool          syncIndex_          = false;   // testler: indeksleme istek içinde
    bool          inlayParamNames_    = true;
    bool          codeLensRefresh_    = false;   // istemci workspace/codeLens/refresh destekler
    int           serverRequestSeq_   = 0;
    // Son yayımlanan tanılar (uri → dizi): bağımlı belgeler yeniden analiz
    // edildiğinde yalnız değişen tanılar yeniden yayımlanır.
    std::map<std::string, nlohmann::json> lastPublished_;
    // Faz 3: initialize'da istemciyle anlaşılan pozisyon birimi. İstemci
    // general.positionEncodings'te "utf-8" bildiriyorsa "utf-8" (dönüşüm
    // gerekmez — SourceLocation.column zaten byte/UTF-8 code unit sayıyor);
    // yoksa LSP varsayılanı "utf-16" (src/lsp/position.hpp dönüştürücüleri
    // devreye girer).
    std::string   positionEncoding_ = "utf-16";

    nlohmann::json handleInitialize(const nlohmann::json& id,
                                    const nlohmann::json& params);
    void handleDidOpen(const nlohmann::json& params);
    void handleDidChange(const nlohmann::json& params);
    void handleDidClose(const nlohmann::json& params);
    nlohmann::json handleDefinition(const nlohmann::json& id,
                                    const nlohmann::json& params);
    nlohmann::json handleHover(const nlohmann::json& id,
                               const nlohmann::json& params);
    nlohmann::json handleReferences(const nlohmann::json& id,
                                    const nlohmann::json& params);
    nlohmann::json handleDocumentSymbol(const nlohmann::json& id,
                                        const nlohmann::json& params);
    nlohmann::json handleDocumentHighlight(const nlohmann::json& id,
                                           const nlohmann::json& params);
    nlohmann::json handleCompletion(const nlohmann::json& id,
                                    const nlohmann::json& params);
    // Faz 5 (#84): tanım + tüm referansları kapsayan çok dosyalı WorkspaceEdit.
    nlohmann::json handleRename(const nlohmann::json& id,
                                const nlohmann::json& params);
    // Faz 5 (#84): imleci saran çağrının imzası (kullanıcı fonksiyonu + builtin).
    nlohmann::json handleSignatureHelp(const nlohmann::json& id,
                                       const nlohmann::json& params);
    // Faz 6: semantic tokens (textDocument/semanticTokens/full). Sınıflandırma
    // grammar regex'i yerine derleyicinin sembol tablosundan gelir — builtin
    // fonksiyonlar (Symbol::isBuiltin), kullanıcı tipleri, değişkenler.
    nlohmann::json handleSemanticTokens(const nlohmann::json& id,
                                        const nlohmann::json& params);

    // ── Bölüm 2/3: çalışma alanı (lsp_workspace.cpp) ─────────────────────
    nlohmann::json handleWorkspaceSymbol(const nlohmann::json& id,
                                         const nlohmann::json& params);
    void handleDidChangeWatchedFiles(const nlohmann::json& params);
    void handleDidChangeConfiguration(const nlohmann::json& params);
    void applySettings(const nlohmann::json& settings);
    // Bir belge analiz edildikten sonra: indeks kaydını yenile, bu dosyaya
    // bağlı açık belgeleri yeniden analiz et (tanıları değiştiyse yayımla).
    void afterAnalysis(DocumentState& state);
    void reanalyzeDependents(const std::string& path, const DocumentState* except);
    ModuleLoader::SourceOverlay overlay();
    // Üst düzey sembolün (dosya, ad) tüm proje referansları — LSP Location.
    // Proje genelinde izlenen üst düzey kullanıcı sembolü mü (fonksiyon,
    // struct, enum, global; yerleşik/FFI hariç).
    bool isProjectLevelSymbol(const Symbol* s) const;
    // `skip`: zaten bilinen (dosya, offset) konumları (belgenin kendi analizi).
    nlohmann::json projectReferenceLocations(
        const std::string& file, const std::string& name,
        const std::set<std::pair<std::string, int>>& skip = {});
    // Üst düzey sembolün tüm proje referansları (dosya, offset, uzunluk) — rename.
    std::vector<std::tuple<std::string, int, int>> projectReferenceOffsets(
        const std::string& file, const std::string& name);
    // Bir dosyaya `import {name} from "…"` ekleyen TextEdit (zaten varsa null).
    nlohmann::json importEditFor(DocumentState& state, const std::string& targetFile,
                                 const std::string& name);

    // ── Bölüm 4: editör hızlandırıcıları (lsp_editor.cpp) ────────────────
    nlohmann::json handleFoldingRange(const nlohmann::json& id, const nlohmann::json& params);
    nlohmann::json handleCodeLens(const nlohmann::json& id, const nlohmann::json& params);
    nlohmann::json handleCodeAction(const nlohmann::json& id, const nlohmann::json& params);
    nlohmann::json handleInlayHint(const nlohmann::json& id, const nlohmann::json& params);
    // Gereksiz kod ipuçları (DiagnosticTag.Unnecessary, Hint) — yayımlanan
    // tanılara eklenir.
    nlohmann::json unnecessaryDiagnostics(DocumentState& state);

    // Bölüm 3 kancaları (lsp_workspace.cpp): `import { | } from "dosya.sqt"`
    // için o dosyanın export'ları; önekle eşleşen import edilmemiş proje
    // sembolleri (otomatik import düzenlemesiyle).
    nlohmann::json importableNamesFromFile(DocumentState& state, const std::string& rawPath);
    void appendProjectCompletions(DocumentState& state, const std::string& prefix,
                                  const std::set<std::string>& seen, nlohmann::json& items);

    // Faz 3: state.diagnostics'i loc.filePath'e göre gruplar, her dosya için
    // ayrı bir publishDiagnostics bildirimi gönderir (kök neden #4 — import
    // edilen modülün hatası artık ana dosyada görünmüyor).
    // onlyIfChanged: bağımlılık kaynaklı yeniden analizde yalnız değişen
    // dosyaların tanıları yayımlanır.
    void publishDiagnosticsGrouped(DocumentState& state, bool onlyIfChanged = false);

    // Verilen (0-tabanlı, istemci pozisyon birimindeki) satır/sütun
    // konumundaki sembolü bul. Faz 3: token binary search + (offset→Symbol*)
    // indeksi — isim-uzunluğu aralık eşleştirmesi ve allSymbols lineer
    // taraması yok (kök neden #3).
    Symbol* findSymbolAt(DocumentState& state, int line, int character);

    // Konumdaki identifier token'ını bul (findSymbolAt ve üye erişimi
    // hover'ının ortak araması). Bulunamazsa veya token identifier değilse
    // nullptr — dönen token'a sahip olunmaz, state.tokens içindeki pointer'tır.
    Token* identifierTokenAt(DocumentState& state, int line, int character) const;

    // Faz 3 pozisyon-dönüşüm yardımcıları (src/lsp/position.hpp'yi sarar) —
    // tüm handler'lar konum çevirisini buradan geçirir.
    int         toByteColumn(const std::string& content, int line, int character) const;
    LspPosition toLspPos(const std::string& content, const SourceLocation& loc) const;
    // Satır-indeksli varyant: sembol/tanı başına çağrılan DÖNGÜLERDE bunu
    // kullan — indeksisiz varyant satırı bulmak için dosya başından tarar,
    // döngüde kuadratik patlar (90K satırlık dosyada documentSymbol dakikalarca
    // %100 CPU yakıyordu).
    LspPosition toLspPos(const std::string& content,
                         const std::vector<int>& lineStarts,
                         const SourceLocation& loc) const;

    // loc'un ait olduğu dosyanın içeriğini döndürür: state'in kendi dosyasıysa
    // buffer'ı doğrudan, değilse store_.contentForPath ile (açık belge ya da disk).
    std::string contentForLoc(DocumentState& state, const SourceLocation& loc) const;
};

#endif // SAQUT_LSP_HANDLER
