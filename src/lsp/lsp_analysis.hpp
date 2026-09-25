// ============================================================================
// saQut LSP — Kapsam ve alıcı analizi (Bölüm 1, docs/lsp-decisions.md)
// ============================================================================
//
// DİZİN:   src/lsp/lsp_analysis.hpp
// KATMAN:  LSP — yalnız ön uç verisi (token, AST, sembol tablosu)
//
// AMAÇ:
//   - Kapsam bilinçli ad çözümü: imleçteki `t` hangi `t`? SymbolCollector'ın
//     Scope nesneleri kaynak aralığı taşımaz; kapsam aralıkları token
//     dizisindeki { } / ( ) eşleşmelerinden çıkarılır. Token dizisi sözdizimi
//     hatalarında da tamdır (tokenizer hata toleranslı), bu yüzden bu analiz
//     yarım yazılmış kodda da çalışır.
//   - Alıcı tipi çözümü: `ifade.` / `ifade.önek` için imleçten geriye doğru
//     alıcı ifadesi (tanımlayıcı, a.b.c zinciri, dizi indeksi, fonksiyon ya
//     da metot çağrısı) yürünür ve tipi çıkarılır.
//   - Üye listeleri: struct alanları, UFCS yerleşik metotları, Pool/List/
//     Thread metotları, enum üyeleri — imza ve kısa açıklamayla.
//
// ============================================================================

#ifndef SAQUT_LSP_ANALYSIS
#define SAQUT_LSP_ANALYSIS

#include <climits>
#include <functional>
#include <string>
#include <vector>
#include "core/type.hpp"
#include "symbol/symbol.hpp"
#include "symbol/symbol_table.hpp"
#include "tokenizer/token.hpp"
#include "parser/ast_node.hpp"
#include "vendor/nlohmann/json.hpp"

// Bir analizin (güncel ya da "son geçerli") salt okunur görünümü. Çözüm
// fonksiyonları DocumentState'e değil buna bakar ki son geçerli analizle de
// aynı kod çalışsın.
struct AnalysisView {
    const std::string*         content  = nullptr;
    const std::vector<Token*>* tokens   = nullptr;
    SymbolTable*               table    = nullptr;
    const std::string*         filePath = nullptr;
    ASTNode*                   ast      = nullptr;
    bool valid() const { return content && tokens && table && filePath; }
};

// Token dizisinden { } ve ( ) eşleşmeleri. Kapanmamış açılış INT_MAX'a
// kadar sürer (yarım yazılmış blok dosya sonuna kadar kapsar).
class ScopeIndex {
public:
    struct Pair { int open; int close; };

    static ScopeIndex build(const std::vector<Token*>& toks);

    // offset'i kesin içeren (open < off < close) en içteki çift; yoksa -1.
    int innermostBrace(int off) const { return innermost(braces_, off); }
    int innermostParen(int off) const { return innermost(parens_, off); }
    const Pair& brace(int i) const { return braces_[static_cast<size_t>(i)]; }
    const Pair& paren(int i) const { return parens_[static_cast<size_t>(i)]; }
    // Açılışı tam `openOff`ta olan küme çifti; yoksa -1.
    int braceOpeningAt(int openOff) const;
    const std::vector<Pair>& braces() const { return braces_; }

private:
    static int innermost(const std::vector<Pair>& v, int off);
    std::vector<Pair> braces_;   // open'a göre sıralı
    std::vector<Pair> parens_;
};

// Bir yerel sembolün (değişken/parametre) görünürlük aralığı [open, close]
// (küme parantezlerinin offsetleri). Global/import/yerleşik semboller için
// {-1, INT_MAX}.
ScopeIndex::Pair localScopeRange(const AnalysisView& v, const ScopeIndex& si,
                                 const Symbol* s);

// Kapsam bilinçli ad çözümü: `offset`te görünür olan `name` sembolü.
// Yereller (en içteki kapsam, aynı kapsamda en son tanım) globallerden
// önce gelir. Field sembolleri hariç. Bulunamazsa nullptr.
Symbol* resolveNameAt(const AnalysisView& v, const ScopeIndex& si,
                      const std::string& name, int offset);

// Sembol imlecin bulunduğu yerde görünür mü (tamamlama listesi filtresi).
bool symbolVisibleAt(const AnalysisView& v, const ScopeIndex& si,
                     const Symbol* s, int offset);

// Alıcı ifadesinin tipi. `enumName` doluysa alıcı bir enum ADIDIR
// (`Color.` → üyeler); `type` o durumda anlamsızdır.
struct ReceiverType {
    bool        ok = false;
    Type        type;
    std::string enumName;
};

// tokens[endIdx] ile biten ifadenin tipi (endIdx: '.'nın hemen solundaki
// token). `rootResolver` kök tanımlayıcıyı çözer — varsayılan çağrı güncel
// analizi, düşme yolu son geçerli analizi kullanır.
using RootResolver = std::function<Symbol*(const std::string& name, int offset)>;
ReceiverType receiverTypeEndingAt(const AnalysisView& v, int endIdx,
                                  const RootResolver& resolveRoot);

// Bir tipin üye tamamlama öğeleri (struct alanları + yerleşik metotlar,
// Pool/List/Thread metotları, dizi/string UFCS metotları). `prefix` boş
// değilse yalnız o önekle başlayanlar.
nlohmann::json memberCompletionItems(const ReceiverType& r, SymbolTable& table,
                                     const std::string& prefix);

// Yerleşik metot çağrısının dönüş tipi (ör. `s.split(",")` → string[];
// `jobs.pop()` → eleman tipi). Bilinmiyorsa Type::error().
Type methodReturnType(const Type& recv, const std::string& method);

// Yerleşik (BuiltinMethodRegistry) metot öğeleri ve Pool/List/Thread
// metotları — handler'daki :: tamamlaması da bunları kullanır.
struct DataMethod;
nlohmann::json builtinMethodItem(const DataMethod* m);
nlohmann::json builtinMethodsForType(const Type& receiverType, const std::string& typeName);
nlohmann::json threadMethodsForType(const Type& t);

// Dil anahtar kelimeleri (tamamlama + rename hedef ad doğrulaması).
const std::vector<std::string>& lspKeywords();
// Yerleşik tip adları (tip beklenen konumda tamamlama).
const std::vector<std::string>& lspBuiltinTypeNames();

// `ad@N` iç adlarının (module_namespacer, #246) kullanıcıya görünen hali.
std::string displayName(const std::string& internalName);

// Sembolün kısa imzası (hover/tamamlama detail): "int topla(int a, int b)",
// "shared int total", "struct Point", ...
std::string symbolSignature(const Symbol* s);

#endif // SAQUT_LSP_ANALYSIS
