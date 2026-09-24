// ============================================================================
// saQut Compiler — AST Düğüm Tabanı (ASTNode, ASTKind, LiteralType)
// ============================================================================
//
// DİZİN:   src/parser/ast_node.hpp
// KATMAN:  Katman 3 — Parser (Ayrıştırıcı)
// AMAÇ:    Tüm AST düğümlerinin taban sınıfını ve temel enum'ları tanımlamak
//
// BAĞIMLILIKLAR:
//   - core/location.hpp:   Kaynak kod konum bilgisi (SourceLocation)
//   - parser/token.hpp:    Token tipleri (TokenType, ParserToken)
//   - tools.hpp:           Yardımcı fonksiyonlar (jsonIndent vb.)
//
// MİMARİ KARARLAR:
//   1. ASTNode, tüm düğümlerin ortak davranışını (log, toJson, children)
//      tek bir yerde tanımlar. NVI (Non-Virtual Interface) pattern'i.
//   2. virtual log() ve toJson() — her düğüm kendi çıktısını kendisi üretir.
//   3. parent pointer — AST'de yukarı doğru gezinme (ör: sembol çözümleme).
//   4. children vector — aşağı doğru gezinme (ör: tüm düğümleri ziyaret).
//   5. ASTKind enum — switch/case ile tip kontrolü (dynamic_cast yerine).
//      Performans: dynamic_cast < switch/case < virtual method çağrısı
//      Ama switch/case ile yeni tip eklemek derleyici uyarısı verir (eksik case).
//
// ============================================================================

#ifndef SAQUT_AST_NODE
#define SAQUT_AST_NODE

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "core/location.hpp"
#include "core/type.hpp"
#include "parser/token.hpp"
#include "tools.hpp"

// ============================================================================
// ASTKind — Düğüm Tipi Enum
// ============================================================================
//
// Tüm AST düğüm tiplerini tanımlar. Her düğüm sınıfı bu enum'dan bir değer
// alır. enum class olması sayesinde isim çakışması olmaz (ASTKind::Program).
//
// NEDEN enum class, neden inheritance'daki typeid kullanılmıyor?
//   - typeid().name() derleyiciye bağlıdır (g++: "4Program", MSVC: "class Program").
//   - enum class her derleyicide aynıdır, string dönüşümü kolaydır.
//   - static_cast<uint16_t> ile serileştirilebilir.
//
// ============================================================================

enum class ASTKind {
    /* ====== En üst seviye ====== */
    Program,              // Kök düğüm — tüm programı kapsar.
                          //   İçindeki children: FunctionDecl, StructDecl, VariableDecl.
                          //   Tüm .cpp/.sqt dosyası tek bir Program düğümüdür.

    /* ====== Tanımlar (Declarations) ====== */
    ImportDecl,           // import {name, ...} from "file.sqt";
                          //   importedNames: içe aktarılan isimler listesi
                          //   sourcePath: kaynak dosya yolu (ham string)
    FunctionDecl,         // Fonksiyon tanımı.
                          //   children: [returnType?], [name], [params...], [body: Block]
                          //   Örn: int main() { ... }
    FfiDecl,              // Gömülü host fonksiyon bildirimi (ADR-034, #107).
                          //   ffi <ret> <ad>(<params>) : <HOST_ID> from <mod> [requires <cap>];
                          //   Gövdesiz; yalnız gömülü root.sqt'te geçerli.
    StructDecl,           // struct tanımı.
                          //   children: [name], [members: VariableDecl...]
                          //   Örn: struct Point { int x; int y; };
    EnumDecl,             // enum tanımı.
                          //   members: [(isim, değer)] — int-tabanlı sabit küme
                          //   Örn: enum Color { Red, Green, Blue }
    VariableDecl,         // Değişken tanımı.
                          //   children: [type?], [name], [initializer?]
                          //   Örn: int x = 5; veya string name;

    /* ====== Kontrol Akışı (Statements) ====== */
    Block,                // { } bloğu — birleşik ifade.
                          //   children: [statements...]
                          //   Kapsam (scope) oluşturur. Yerel değişkenler burada tanımlanır.
    IfStatement,          // if (koşul) gövde [else gövde].
                          //   children: [condition], [thenBranch], [elseBranch?]
    ForStatement,         // for (init; koşul; artım) gövde.
                          //   children: [init?], [condition?], [increment?], [body]
    WhileStatement,       // while (koşul) gövde.
                          //   children: [condition], [body]
    DoWhileStatement,     // do gövde while (koşul);
                          //   children: [body], [condition]
    ReturnStatement,      // return [ifade?];
                          //   children: [value?]
    BreakStatement,       // break;
                          //   children: yok. Sadece döngü/switch içinde geçerli.
    ContinueStatement,    // continue;
                          //   children: yok. Sadece döngü içinde geçerli.
    ExpressionStatement,  // ifade + noktalı virgül (;)
                          //   children: [expression]
                          //   Örn: x = 5; veya foo();
    TryStatement,         // try { body } catch (Error e) { handler }  (ADR-025)
    ThrowStatement,       // throw <ifade>;                             (ADR-025)
    SwitchStatement,      // switch (expr) { case v: ... default: ... } (ADR-027)
    LockStatement,        // lock a, b;  /  unlock a;                    (ADR-045)
    WaitStatement,        // wait(koşul);                                (ADR-045)

    /* ====== İfadeler (Expressions) ====== */
    BinaryExpression,     // İkili işlem: sol OP sağ.
                          //   children: [left], [right]
                          //   OP bilgisi: exprType alanında saklanır.
    UnaryExpression,      // Tekli işlem: OP operand.
                          //   children: [operand]
                          //   prefix (++x) veya postfix (x++) olabilir.
    Literal,              // Sabit değer: 42, 3.14, "hello", true, null.
                          //   children: yok. Değer düğümün kendi alanında.
    Identifier,           // İsim referansı: x, PI, main.
                          //   children: yok. İsim string olarak saklanır.
    Postfix,              // Postfix işlem: operand++.
                          //   children: [operand]
    Call,                 // Fonksiyon/metot çağrısı: f(args).
                          //   children: [callee], [args...]
    MemberAccess,         // Üye erişimi: a.b veya a->b.
                          //   children: [object], [member]
    IndexExpression,      // Dizi/indeks erişimi: a[i].
                          //   children: [object], [index]
    ArrayLiteral,         // Dizi literali: [1, 2, 3].
                          //   children: [element0, element1, ...]
    CastExpression,       // Tip dönüşümü: expr as TargetType[?]  (ADR-026)
                          //   operand: kaynak ifade; targetTypeName: hedef tip adı
    ScopeCall,            // Built-in metod çağrısı: E::method(args)
                          //   leftTypeName: "int", "string", "Person" ...
                          //   methodName: "push", "upper", "toJson" ...
                          //   arguments: argümanlar (receiver dahil)
    ThreadExpr,           // thread { gövde } — ifade, tipi Thread       (ADR-045)
    CollectionNew,        // Pool(T) / List(T) — tip argümanlı intrinsic (ADR-045)

    /* ====== Hata Kurtarma (Faz 2 — LSP/DAP kurtarma planı) ====== */
    Error,                // Panic-mode kurtarma yer tutucusu: parser bu noktada
                          //   sözdizimsel olarak beklenmedik bir token gördü,
                          //   bir tanı (E9xx) üretti ve bilinen bir sınıra
                          //   (';', '}', statement-başlangıcı) kadar atlayıp
                          //   ağacı bu düğümle doldurdu. children: yok.
                          //   SymbolCollector/TypeChecker/StructuralValidator
                          //   switch/default ile bunu sessizce atlar — AST'nin
                          //   geri kalanı analiz edilmeye devam eder.
};

// ============================================================================
// LiteralType — Sabit Değer Alt Tipleri
// ============================================================================
//
// Literal düğümünün hangi türde bir sabit değer taşıdığını belirler.
// uint8_t tabanlı — 256 farklı literal tipi yeterli.
//
// KULLANIM:
//   Literal düğümü oluşturulurken tip belirtilir:
//     Literal lit(LiteralType::INTEGER, "42");
//
// ============================================================================

enum class LiteralType : uint8_t {
    INTEGER,   // Tamsayı sabiti: 42, 0xFF, 0b1010, 0777
               //   Decimal, hexadecimal (0x), octal (0), binary (0b) desteklenir.
               //   Tokenizer NumberToken ile iletilir.
    FLOAT,     // Ondalıklı sayı: 3.14, 1e-5, 2.0f
               //   Nokta veya üs (e/E) içeren sayılar.
    STRING,    // Metin sabiti: "merhaba dünya"
               //   Çift tırnak içinde. Kaçış dizileri (\n, \t, \") desteklenir.
    BOOLEAN,   // Mantıksal değer: true / false
               //   KW_TRUE veya KW_FALSE token'ından gelir.
    BOŞ        // null sabiti (Türkçe "boş").
               //   KW_NULL token'ından gelir. Nesne/referans türleri için kullanılır.
};

// ============================================================================
// literalTypeToString — LiteralType'ı string'e çevir (log için)
// ============================================================================
//
// PARAMETRE:  t — LiteralType enum değeri
// DÖNÜŞ:     const char* — insan tarafından okunabilir string
// KARMAŞIKLIK: O(1) — switch/case (derleyici jump table üretir)
//
inline const char* literalTypeToString(LiteralType t) {
    switch (t) {
        case LiteralType::INTEGER: return "integer";
        case LiteralType::FLOAT:   return "float";
        case LiteralType::STRING:  return "string";
        case LiteralType::BOOLEAN: return "boolean";
        case LiteralType::BOŞ:     return "null";
    }
    return "?";
}

// ============================================================================
// ASTNode — Soyut Taban Sınıf
// ============================================================================
//
// Tüm AST düğümleri bu sınıftan türetilir. Her düğüm:
//   - kind:       ASTKind enum — tipini bilir (switch/case için)
//   - parent:     Ebeveyn düğüme işaretçi (ağaçta yukarı gezinme)
//   - loc:        Kaynak koddaki satır/sütun konumu (hata mesajları için)
//   - children:   Alt düğümler (ağaçta aşağı gezinme)
//   - log():      Konsola hiyerarşik yazdırma
//   - toJson():   JSON formatında serileştirme
//
// KALITIM:
//   Program : ASTNode          — Kök düğüm
//   FunctionDecl : ASTNode     — Fonksiyon tanımı
//   BinaryExpression : ASTNode — İkili işlem
//   ... (her düğüm tipi ayrı sınıf)
//
// BELLEK YÖNETİMİ:
//   Düğümler new ile oluşturulur, delete ile yok edilir.
//   Sahiplik: Parser oluşturur, çağıran (main/CLI) yok eder.
//   TODO(Büyük yeniden düzenleme): std::unique_ptr ile RAII.
//
// ============================================================================

class ASTNode {
public:
    /* ====== Profiling sayacı (byproduct) ====== */
    // Süreç boyunca kurulan toplam AST düğümü sayısı. --profile "parser"
    // aşamasının node adedini, ModuleLoader::load() öncesi/sonrası bu sayacın
    // delta'sıyla okur (ayrı bir ağaç gezme YOK — her düğüm zaten burada tek
    // bir kez kuruluyor). Tek-iş-parçacıklı derleme; senkronizasyon gerekmez.
    static inline long long s_constructedCount = 0;

    ASTNode() { ++s_constructedCount; }

    /* ====== Her düğümün tipi ====== */
    ASTKind kind;                    // Düğüm tipi (Program, FunctionDecl, ...)
                                     //   switch(kind) ile tip kontrolü.
                                     //   Set edilir ve bir daha değişmez.

    /* ====== Ağaç bağlantıları ====== */
    ASTNode* parent = nullptr;       // Ebeveyn düğüm pointerı.
                                     //   addChild() tarafından otomatik set edilir.
                                     //   Kullanım: semantic analizde kapsam bulma.
                                     //   Örn: değişkenin tanımlandığı fonksiyonu bulmak
                                     //   için parent->parent->... şeklinde yukarı çıkılır.

    /* ====== Kaynak konumu ====== */
    SourceLocation loc;              // Tokenizer'dan gelen satır/sütun bilgisi.
                                     //   Hata mesajlarında: "satır 5, sütun 12"
                                     //   TODO: Şu anda tüm düğümlerde dolu değil.

    /* ====== Sanal Metotlar ====== */

    // log() — Düğümü ve alt düğümlerini konsola yazdırır.
    // PARAMETRE: indent — girinti seviyesi (her seviyede 2 boşluk artar)
    // KULLANIM:  ast->log(0); // tüm ağacı yazdır
    // KARMAŞIKLIK: O(n) — tüm alt ağacı dolaşır
    virtual void log(int indent = 0) {
        (void)indent;
        std::cout << "<Unknown>\n";
    }

    // toJson() — Düğümü ve alt düğümlerini JSON formatında döndürür.
    // PARAMETRE: indent — JSON girinti seviyesi
    // DÖNÜŞ:    JSON stringi
    // KULLANIM:  std::string json = ast->toJson(0);
    // KARMAŞIKLIK: O(n) — tüm alt ağacı dolaşır, string birleştirme maliyeti
    virtual std::string toJson(int indent = 0) {
        (void)indent;
        return "{\"kind\":\"Unknown\"}";
    }

    /* ====== Yardımcı Metotlar ====== */

    // addChild() — Alt düğüm ekler ve parent pointer'ını set eder.
    // PARAMETRE: child — eklenecek alt düğüm (nullptr olmamalı)
    // YAN ETKİ:  child->parent = this (otomatik)
    // KARMAŞIKLIK: O(1) amortize — vector push_back
    void addChild(ASTNode* child) {
        children.push_back(child);
        child->parent = this;
    }

    // getChildren() — Alt düğüm vektörüne erişim.
    // DÖNÜŞ:     std::vector<ASTNode*>& — çocuk düğümler listesi
    // KARMAŞIKLIK: O(1) — referans döndürür
    std::vector<ASTNode*>& getChildren() { return children; }

    // ~ASTNode() — children vektörünü özyinelemeli siler.
    //   Typed pointer'lar (condition, thenBranch vb.) alt sınıf yıkıcılarına bırakılır;
    //   children vektörü ile typed pointer'lar örtüşmediği için double-delete olmaz.
    virtual ~ASTNode() {
        for (auto* ch : children) delete ch;
    }

protected:
    // children — Alt düğümlerin vektörü.
    //   protected: doğrudan erişim yerine addChild/getChildren kullanılır.
    //   Türetilmiş sınıflar erişebilir (ör: log() içinde çocukları gezme).
    std::vector<ASTNode*> children;
};

// ============================================================================
// ExpressionNode — Değer Üreten Düğümlerin Tabanı (Faz 1, ADR-012)
// ============================================================================
//
// Bir DEĞER üreten her düğüm (Literal, Identifier, BinaryExpression, Call,
// Postfix, MemberAccess, IndexExpression) buradan türer. Bir ifadenin bir
// TİPİ vardır; analiz/optimizasyon alanları burada toplanır.
//
class ExpressionNode : public ASTNode {
public:
    // TODO(faz-3): tip denetleyici doldurur. Şimdilik Error = "henüz çözülmedi".
    Type resolvedType;

    // TODO(faz-4): sabit katlama (constant folding) bayrağı.
    bool isConstant = false;
    // TODO(faz-4): foldedValue — katlanmış sabit değer (temsil Faz 4'te netleşir).

    // resolvedType'ın JSON karşılığı (henüz çözülmemişse null gösterilir).
    std::string resolvedTypeJson() const {
        return resolvedType.isError() ? std::string("null") : resolvedType.toJson();
    }
};

// ============================================================================
// StatementNode — Eylem/Kontrol Akışı Yürüten Düğümlerin Tabanı (Faz 1)
// ============================================================================
//
// Değer üretmeyen, bir iş/kontrol akışı yürüten her düğüm (Block, If, For,
// While, DoWhile, Return, Break, Continue, ExpressionStatement ve şimdilik
// VariableDecl) buradan türer. Tipi yoktur; akış-analizi alanları taşır.
//
// TODO(faz-1 gözden geçirme): VariableDecl/FunctionDecl/StructDecl'in tam
// sınıflandırması provizyonel — VariableDecl burada (blok içinde erişilebilirliğe
// tabi), Function/StructDecl doğrudan ASTNode altında kaldı.
//
class StatementNode : public ASTNode {
public:
    // TODO(faz-3/4): erişilebilirlik (dead-code) analizi günceller.
    bool isReachable = true;
};

// ============================================================================
// childrenToJson — Düğümün çocuklarını JSON array olarak yaz
// ============================================================================
//
// Bir düğümün tüm alt düğümlerini dolaşır ve her birinin toJson() çıktısını
// virgülle ayrılmış şekilde birleştirir.
//
// PARAMETRELER:
//   node  — çocukları yazdırılacak düğüm
//   depth — JSON girinti seviyesi
//
// DÖNÜŞ:  JSON array içeriği (köşeli parantezler HARİÇ)
//
// KULLANIM:
//   std::string json = childrenToJson(this, depth + 1);
//
// KARMAŞIKLIK: O(n) — n = çocuk sayısı
//
inline std::string childrenToJson(ASTNode* node, int depth) {
    std::ostringstream ss;
    std::string in = jsonIndent(depth);
    auto& ch = node->getChildren();
    for (size_t i = 0; i < ch.size(); i++) {
        ss << ch[i]->toJson(depth);
        if (i + 1 < ch.size()) ss << ",";  // son elemandan sonra virgül yok
        ss << "\n";
    }
    return ss.str();
}

#endif // SAQUT_AST_NODE
