// ============================================================================
// saQut Compiler — Tip Denetleyici (TypeChecker)
// ============================================================================
//
// DİZİN:   src/semantic/type_checker.hpp
// KATMAN:  Faz 3 — AST üzerinde tip denetimi ve tip çıkarımı
//
// AMAÇ:
//   Her ifade düğümüne resolvedType atar, atama/parametre/dönüş uyumunu
//   kontrol eder (ADR-010/021/025/026/027/028). Nullable daraltma ve
//   non-void return kontrolü yapar.
//
// ============================================================================

#ifndef SAQUT_SEMANTIC_TYPE_CHECKER
#define SAQUT_SEMANTIC_TYPE_CHECKER

#include <unordered_set>
#include <string>
#include <vector>
#include "symbol/symbol_table.hpp"
#include "diagnostic/diagnostic_engine.hpp"
#include "parser/ast_node.hpp"
#include "core/type.hpp"

class TypeChecker {
public:
    TypeChecker(SymbolTable& table, DiagnosticEngine& diag)
        : table_(table), diag_(diag) {}

    void check(ASTNode* program);

private:
    // İfadeyi gez, resolvedType ata, tipi döndür.
    // expected: bağlam tipi — literal genişletme kararı için.
    Type checkExpr(ASTNode* node, const Type& expected = Type::error());

    void checkStmt(ASTNode* node);
    void checkFunction(ASTNode* fnNode);

    // Atama / parametre uyumu: true = geçerli (uyarı dahil).
    // srcIsLiteral: RHS doğrudan bir Literal node'u mu?
    bool checkAssign(const Type& target, const Type& src,
                     bool srcIsLiteral,
                     const SourceLocation& loc,
                     const std::string& context,
                     const std::string& hintExpr = "");

    // İki sayısal tipin genişlik sırası: int=0, float=1, double=2; -1 = sayısal değil.
    static int numericRank(const Type& t);

    // ADR-021: if-narrowing — null kontrolü kalıbını ayrıştır
    // Dönüş: {varName, isNotNull} — "a != null" → {a, true}; "a == null" → {a, false}; {"", _} = kalıp yok
    static std::pair<std::string, bool> extractNullCheck(ASTNode* cond);
    // Bir statement her zaman çıkış yapıyor mu? (return/throw/break/continue)
    static bool alwaysExits(ASTNode* stmt);
    // Non-void fonksiyon kontrolü: tüm akış yolları return/throw ile bitiyor mu?
    static bool pathAlwaysReturns(ASTNode* stmt);

    SymbolTable&      table_;
    DiagnosticEngine& diag_;

    Type currentReturnType_;   // aktif fonksiyonun beklenen dönüş tipi
    bool inFunction_ = false;

    // ADR-021: akış-duyarlı null daraltma — bu kapsamda non-null olduğu bilinen değişkenler
    std::unordered_set<std::string> narrowedNonNull_;

    // ── ADR-045 (Faz 3-b) ─────────────────────────────────────────────────
    // Modül kapsamındaki bildirim denetleniyor mu (shared / Pool / List).
    bool inGlobalDecl_ = false;
    // Pool(T)/List(T) yalnız shared global başlatıcısında geçerli.
    bool allowCollectionNew_ = false;
    // Sözcüksel kilit kapsamları: her Block bir seviye; tutulan shared adları.
    std::vector<std::vector<std::string>> heldLocks_;
    bool anyLockHeld() const {
        for (auto& lvl : heldLocks_) if (!lvl.empty()) return true;
        return false;
    }
    // Gönderilebilir tip (mesaj deep copy / thread yakalaması).
    bool isSendable(const Type& t) const;
    bool isSendableImpl(const Type& t, std::unordered_set<std::string>& seenStructs) const;
    // Pool/List/Thread alıcılı metot çağrısı (ScopeCall dotCall).
    Type checkThreadIntrinsic(class ScopeCallNode* sc, const Type& recvType,
                              const std::vector<Type>& argTypes);
    // İfade ağacında shared bir sembole başvuru var mı (wait kuralı, W008).
    static bool referencesShared(ASTNode* node);
    static bool referencesSymbol(ASTNode* node, const Symbol* sym);

    // #219 A1: unary '-' altındaki tam sayı literal'i denetlenirken >0 olur.
    // Aralık denetimi üst sınırı bir kaydırır, böylece her tipin EN KÜÇÜK
    // değeri (-2147483648, -9223372036854775808) yazılabilir kalır.
    int negatedLiteralDepth_ = 0;
};

#endif // SAQUT_SEMANTIC_TYPE_CHECKER
