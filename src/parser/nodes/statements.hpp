// ============================================================================
// saQut Compiler — İfade/Deyim Düğümleri
// ============================================================================
//
// DİZİN:   src/parser/nodes/statements.hpp
// KATMAN:  Katman 3 — Kontrol akışı ve eylem düğümleri
//
// AMAÇ:
//   Değer üretmeyen, kontrol akışı yürüten düğümlerin tanımları.
//   Block, if/else, for, while, do-while, return, break, continue,
//   try/catch/throw, switch/case, expression statement.
//
// ============================================================================

#ifndef SAQUT_AST_STMT
#define SAQUT_AST_STMT

#include "parser/ast_node.hpp"

class BlockNode : public StatementNode {
public:
    BlockNode();
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

class IfStatementNode : public StatementNode {
public:
    ASTNode* condition  = nullptr;
    ASTNode* thenBranch = nullptr;
    ASTNode* elseBranch = nullptr;
    IfStatementNode();
    ~IfStatementNode() override { delete condition; delete thenBranch; delete elseBranch; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

class WhileStatementNode : public StatementNode {
public:
    ASTNode* condition = nullptr;
    ASTNode* body      = nullptr;
    WhileStatementNode();
    ~WhileStatementNode() override { delete condition; delete body; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

class ForStatementNode : public StatementNode {
public:
    ASTNode* init      = nullptr;
    ASTNode* condition = nullptr;
    ASTNode* update    = nullptr;
    ASTNode* body      = nullptr;
    ForStatementNode();
    ~ForStatementNode() override { delete init; delete condition; delete update; delete body; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

class DoWhileStatementNode : public StatementNode {
public:
    ASTNode* condition = nullptr;
    ASTNode* body      = nullptr;
    DoWhileStatementNode();
    ~DoWhileStatementNode() override { delete body; delete condition; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

class ReturnStatementNode : public StatementNode {
public:
    ASTNode* value = nullptr;
    ReturnStatementNode();
    ~ReturnStatementNode() override { delete value; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

class BreakStatementNode : public StatementNode {
public:
    BreakStatementNode();
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

class ContinueStatementNode : public StatementNode {
public:
    ContinueStatementNode();
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

class ExpressionStatementNode : public StatementNode {
public:
    ASTNode* expression = nullptr;
    ExpressionStatementNode();
    ~ExpressionStatementNode() override { delete expression; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

// ADR-025: try { body } catch (Error catchVar) { handler }
class TryStatementNode : public StatementNode {
public:
    ASTNode*    body        = nullptr;
    std::string catchVar;              // catch değişken adı (ör. "e")
    ASTNode*    handler     = nullptr;
    TryStatementNode();
    ~TryStatementNode() override { delete body; delete handler; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

// ADR-025: throw <ifade>;
class ThrowStatementNode : public StatementNode {
public:
    ASTNode* value = nullptr;
    ThrowStatementNode();
    ~ThrowStatementNode() override { delete value; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

// ADR-027: switch (expr) { case v1, v2: ... default: ... }
// Fallthrough YOK; implicit break her case sonrasında.
// Çok-değerli case: case 1, 2, 3: (OR semantiği)
struct CaseClause {
    std::vector<ASTNode*> values; // boş ise default
    bool isDefault = false;
    std::vector<ASTNode*> body;  // bu case'in statement'ları
    ~CaseClause() {
        for (auto* v : values) delete v;
        for (auto* s : body)   delete s;
    }
    // Copy forbidden; use heap-allocated CaseClause* or move
    CaseClause() = default;
    CaseClause(const CaseClause&) = delete;
    CaseClause& operator=(const CaseClause&) = delete;
    CaseClause(CaseClause&&) = default;
    CaseClause& operator=(CaseClause&&) = default;
};

class SwitchStatementNode : public StatementNode {
public:
    ASTNode* subject = nullptr;
    std::vector<CaseClause> cases;
    SwitchStatementNode();
    ~SwitchStatementNode() override { delete subject; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

// ADR-045: lock a, b;  /  unlock a;
// targets: IdentifierNode'lar (SymbolCollector çözer). lock kapsam (blok)
// sonunda otomatik bırakılır; çoklu kilit slot indeksine göre sıralı alınır.
class LockStatementNode : public StatementNode {
public:
    std::vector<ASTNode*> targets;
    bool                  isUnlock = false;
    LockStatementNode();
    ~LockStatementNode() override { for (auto* t : targets) delete t; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

// ADR-045: wait(koşul); — koşul doğru olana kadar bekler (en az bir shared
// sembol içermeli).
class WaitStatementNode : public StatementNode {
public:
    ASTNode* condition = nullptr;
    WaitStatementNode();
    ~WaitStatementNode() override { delete condition; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

#endif
