// ============================================================================
// saQut LSP — Proje indeksi (Bölüm 3, docs/lsp-decisions.md)
// ============================================================================
//
// DİZİN:   src/lsp/project_index.hpp
// KATMAN:  LSP — çalışma alanı genelinde üst düzey semboller ve referanslar
//
// Her .sqt dosyası için: modül yükleyici + sembol toplayıcı (type check YOK).
// Kayıt: dosyada tanımlı üst düzey semboller (fonksiyon, struct, enum,
// global) + imza + konum + export bilgisi, dosyanın import'ları ve dosyadan
// herhangi bir üst düzey sembole yapılan referanslar (referans sayısı lens'i,
// dosyalar arası references/rename).
//
// İş parçacığı: indeks yalnız LSP analiz thread'inde değişir (ön ucun
// FileRegistry gibi tekilleri kilitsizdir — bkz. karar günlüğü). Tarama
// boşta kalınan anlarda dosya dosya ilerler (step()).
//
// Önbellek: içerik hash'i aynıysa dosya yeniden indekslenmez.
// ============================================================================

#ifndef SAQUT_LSP_PROJECT_INDEX
#define SAQUT_LSP_PROJECT_INDEX

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "module/module_loader.hpp"
#include "symbol/symbol.hpp"

class SymbolTable;
class ASTNode;

struct IndexSymbol {
    std::string name;        // görünen ad (ad@N soyulmuş)
    SymbolKind  kind = SymbolKind::Function;
    bool        exported = false;
    bool        shared   = false;
    std::string signature;   // "int topla(int a, int b)"
    std::string doc;         // tanımın hemen üstündeki yorum bloğu
    int         line = 0;    // 1-tabanlı, tanımlayıcının kendisi
    int         col  = 0;    // 1-tabanlı bayt sütunu
    int         offset = -1;
    int         declOffset = -1;   // bildirimin başı (Symbol::definitionLoc)
};

struct IndexRef {
    std::string targetFile;  // hedef sembolün tanımlandığı dosya
    std::string targetName;  // hedef sembolün görünen adı
    int         line = 0, col = 0, offset = -1, length = 0;
};

struct IndexImport {
    std::string resolvedPath;   // dosya importu: canonical yol; modül importu: boş
    std::string module;         // tırnaksız modül adı (FFI)
    std::vector<std::pair<std::string, std::string>> names;   // kaynak → yerel
    int         declOffset = -1;   // `import` token'ı
    int         endOffset  = -1;   // bildirimi bitiren ';' (dahil)
    int         closeBrace = -1;   // `}` offset'i (ad eklemek için)
};

struct IndexEntry {
    std::string              path;
    size_t                   hash = 0;
    bool                     fromOpenDocument = false;
    std::vector<IndexSymbol> symbols;
    std::vector<IndexRef>    refs;
    std::vector<IndexImport> imports;
    std::vector<std::string> deps;
};

class ProjectIndex {
public:
    using Overlay = ModuleLoader::SourceOverlay;

    // Çalışma alanı kökleri ve hariç tutma desenleri (glob: *, **, ?).
    void configure(std::vector<std::string> roots, std::vector<std::string> excludes);
    bool configured() const { return !roots_.empty(); }

    // Köklerdeki tüm .sqt dosyalarını kuyruğa al (build*, node_modules, .git
    // ve hariç desenler dışında).
    void scheduleFullScan();
    bool hasPending() const { return !pending_.empty(); }
    // Kuyruktaki bir dosyayı indeksle (içerik overlay'den ya da diskten).
    void step(const Overlay& overlay);
    void runAll(const Overlay& overlay) { while (hasPending()) step(overlay); }
    // Tarama tamamlandı mı (ilk tur).
    bool initialScanDone() const { return scanned_ && pending_.empty(); }

    void markDirty(const std::string& path);   // didChangeWatchedFiles / didClose
    void remove(const std::string& path);

    // Açık belgenin analizinden (güncel içerik) kaydı yenile.
    void updateFromAnalysis(const std::string& path, const std::string& content,
                            ASTNode* ast, SymbolTable& table,
                            const std::vector<std::string>& deps);
    // Tek bir dosyayı hemen indeksle (ör. `import {|} from "x.sqt"`).
    const IndexEntry* ensure(const std::string& path, const Overlay& overlay);

    const IndexEntry* get(const std::string& path) const;
    const std::map<std::string, IndexEntry>& entries() const { return entries_; }

    // (dosya, ad) sembolüne tüm dosyalardan yapılan referanslar.
    std::vector<std::pair<std::string, const IndexRef*>>
    refsTo(const std::string& file, const std::string& name) const;

    bool excluded(const std::string& path) const;

private:
    void indexFile(const std::string& path, const Overlay& overlay);

    std::vector<std::string>          roots_;
    std::vector<std::string>          excludes_;
    std::deque<std::string>           pending_;
    std::set<std::string>             pendingSet_;
    std::map<std::string, IndexEntry> entries_;
    bool                              scanned_ = false;

    // refsTo için ters indeks (hedef → referanslar); entries_ değiştikçe
    // version_ artar, ters indeks tembel yeniden kurulur.
    unsigned long version_ = 0;
    mutable unsigned long reverseVersion_ = ~0ul;
    mutable std::map<std::string, std::vector<std::pair<std::string, const IndexRef*>>> reverse_;
};

// Ortak: bir analiz sonucundan (AST + tablo) indeks kaydı üret.
IndexEntry buildIndexEntry(const std::string& path, const std::string& content,
                           ASTNode* ast, SymbolTable& table,
                           const std::vector<std::string>& deps);

// Tanımın (1-tabanlı satır) hemen üstündeki // ya da /* */ yorum bloğu.
std::string docCommentAbove(const std::string& content, int line);

// Basit glob: `*` (dizin ayırıcı hariç), `**` (her şey), `?`.
bool globMatch(const std::string& pattern, const std::string& path);

#endif // SAQUT_LSP_PROJECT_INDEX
