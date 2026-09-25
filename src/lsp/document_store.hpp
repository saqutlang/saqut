// ============================================================================
// saQut LSP — DocumentStore (Açık Belgelerin Yöneticisi)
// ============================================================================
//
// DİZİN:   src/lsp/document_store.hpp
// KATMAN:  LSP — Açık belgelerin buffer'larını yönetir, overlay ile derler
//
// AMAÇ:
//   didOpen/didChange/didClose ile belge durumunu takip eder.
//   runPipeline() tüm açık belgeleri ModuleLoader overlay'i ile derler.
//
// ============================================================================

#ifndef SAQUT_LSP_DOCUMENT_STORE
#define SAQUT_LSP_DOCUMENT_STORE

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <memory>
#include "parser/ast_node.hpp"
#include "symbol/symbol_table.hpp"
#include "diagnostic/diagnostic_engine.hpp"
#include "core/module_registry.hpp"
#include "tokenizer/token.hpp"

// "Son geçerli analiz" (Bölüm 1): sözdizimi hatası içermeyen en son turun
// sahiplenilmiş kopyası. Kullanıcı yazarken dosya geçici olarak bozulduğunda
// (yarım ifade, eşleşmemiş parantez) tamamlama bir önceki sağlam analizin
// tiplerine düşebilsin diye tutulur.
struct AnalysisSnapshot {
    std::string                       content;
    std::vector<int>                  lineStarts;
    std::string                       filePath;
    ASTNode*                          ast = nullptr;
    std::vector<Token*>               tokens;
    SymbolTable                       symbolTable;
    std::unordered_map<int, Symbol*>  symbolByOffset;

    AnalysisSnapshot() = default;
    AnalysisSnapshot(const AnalysisSnapshot&) = delete;
    AnalysisSnapshot& operator=(const AnalysisSnapshot&) = delete;
    ~AnalysisSnapshot() {
        delete ast;
        for (auto* t : tokens) delete t;
    }
};

struct DocumentState {
    std::string      uri;
    std::string      content;
    // content'in satır-başlangıç byte offset indeksi (position.hpp
    // buildLineStarts). Konum dönüşümü yapan döngüler (documentSymbol,
    // diagnostics, references...) satır metnine dosya başından taramadan
    // O(1) erişsin diye content ile birlikte güncellenir.
    std::vector<int> lineStarts;
    int              version = 0;
    ASTNode*         ast     = nullptr;
    // Faz 3: bu belgenin canonical dosya yolu (uriToPath + weakly_canonical).
    // symbolByOffset'i doldururken ve çok-dosyalı sorgularda "bu sembol BENİM
    // dosyamda mı" testinde kullanılır — ModuleLoader'ın SourceLocation.filePath'e
    // yazdığı yolla aynı biçimde (canonical) olmalı.
    std::string      filePath;
    // Faz 3: bu turun tokenleri — ast ile birlikte sahiplenilir (IdentifierNode
    // ::lexerToken bunlara işaret eder). findSymbolAt konum→token binary search'ü
    // için kullanır (kök neden #3).
    std::vector<Token*> tokens;
    // Faz 3: bu dosyaya ait (offset → Symbol*) indeksi — runPipeline'da bir kez
    // kurulur (SymbolTable.allSymbols() + her sembolün definitionLoc/references'ı,
    // yalnızca filePath == bu belgenin filePath'i olanlar). findSymbolAt burada
    // O(1) arar; isim-uzunluğu aralık eşleştirmesi ve allSymbols lineer taraması
    // artık yok.
    std::unordered_map<int, Symbol*> symbolByOffset;
    // Faz 2 ("son iyi tablo"): SymbolTable unique_ptr tabanlı sahiplik kullandığı
    // için kopyalanamaz, yalnızca taşınabilir — bu yüzden ayrı bir
    // lastGoodSymbolTable alanı yerine symbolTable'ın KENDİSİ bu rolü üstlenir:
    // runPipeline yalnızca yeni bir tablo üretebildiğinde üzerine yazar (bkz.
    // document_store.cpp), modül hiç yüklenemediğinde dokunmadan bırakır.
    // TODO(faz-ileri): SymbolCollector bir gün gerçekten yarıda kesilebilir hale
    // gelirse (bugün mümkün değil — hep tamamlanır), gerçek bir "son iyi" anlık
    // görüntüsü için SymbolTable derin kopyalanabilir hale getirilmeli.
    SymbolTable      symbolTable;
    DiagnosticEngine diagnostics;

    // Bu turun analizi kendi dosyasında sözdizimi hatası (E9xx) içermiyor mu.
    bool syntaxOk = false;
    // Son sözdizimi-temiz tur (bu tur temizse bir öncekidir; bkz. update()).
    std::unique_ptr<AnalysisSnapshot> lastGood;
    // Bu belgenin modül grafiğindeki tüm dosyalar (canonical; kendisi dahil).
    // Bağımlılık değişince yeniden analiz için (Bölüm 2).
    std::vector<std::string> deps;

    ~DocumentState() {
        delete ast;
        for (auto* t : tokens) delete t;
    }
    DocumentState() = default;
    DocumentState(const DocumentState&) = delete;
    DocumentState& operator=(const DocumentState&) = delete;
};

// Faz 5 (#84): bildirim konumundan tanımlayıcının gerçek byte offset'ini bulur.
// Symbol::definitionLoc bildirim BAŞINI gösterir ("int deger"de `int` token'ı) —
// rename gibi tanımlayıcının kendisini hedefleyen işlemler için içerikte
// declOffset'ten ileriye doğru `name`in tam-kelime ilk geçişi aranır.
// Bulunamazsa -1 (arama penceresi: bildirim başlığı için 256 bayt yeterli).
inline int identOffsetFromDecl(const std::string& content, int declOffset,
                               const std::string& name) {
    if (declOffset < 0 || name.empty()) return -1;
    auto isWord = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
    size_t limit = std::min(content.size(),
                            static_cast<size_t>(declOffset) + 256 + name.size());
    for (size_t i = declOffset; i + name.size() <= limit; ++i) {
        if (content.compare(i, name.size(), name) != 0) continue;
        bool startOk = (i == 0) || !isWord(static_cast<unsigned char>(content[i - 1]));
        size_t after = i + name.size();
        bool endOk = (after >= content.size()) ||
                     !isWord(static_cast<unsigned char>(content[after]));
        if (startOk && endOk) return static_cast<int>(i);
    }
    return -1;
}

class DocumentStore {
public:
    DocumentState& update(const std::string& uri,
                          const std::string& content, int version);
    DocumentState* get(const std::string& uri);
    void           close(const std::string& uri);

    // Verilen canonical dosya yolunun içeriğini döndürür: açık bir belgeyse
    // buffer'ı, değilse diski okur (bulunamazsa boş string). Faz 3 — çok-dosya
    // konum dönüşümü (definition/references başka dosyaya işaret ettiğinde o
    // dosyanın satır metnine ihtiyaç var, bkz. src/lsp/position.hpp).
    std::string contentForPath(const std::string& path) const;

    // Verilen canonical dosya yolu için URI döndürür: dosya açık bir belgeyse
    // İSTEMCİNİN o belgeyi açarken gönderdiği ORİJİNAL uri string'i (böylece
    // aynı dosya için sonuç her zaman istemcinin kendi URI biçimiyle —
    // ör. yüzde-kodlamasız/kodlamalı — geri döner); açık değilse pathToUri
    // ile sentezlenmiş bir URI (Faz 3 — çok-dosya URI, kök neden #4).
    std::string uriForPath(const std::string& path) const;

    // İçerik değişmeden yeniden analiz (bağımlı bir modül değişti — Bölüm 2).
    void reanalyze(DocumentState& state);
    // Modül grafiğinde `path`i içeren açık belgeler (`except` hariç), uri sıralı.
    std::vector<DocumentState*> dependentsOf(const std::string& path,
                                             const DocumentState* except);
    // Tüm açık belgeler, uri sıralı.
    std::vector<DocumentState*> all();

private:
    void runPipeline(DocumentState& state);

    // store_'daki açık belgeler arasında canonical yola göre arar; bulursa
    // buffer'ını `out`'a yazar. Diske DÜŞMEZ — çağıran karar verir (ModuleLoader
    // overlay'i disk fallback'i kendi yapar, bkz. document_store.cpp).
    bool openContent(const std::string& path, std::string& out) const;

    std::unordered_map<std::string, std::unique_ptr<DocumentState>> store_;
};

#endif // SAQUT_LSP_DOCUMENT_STORE
