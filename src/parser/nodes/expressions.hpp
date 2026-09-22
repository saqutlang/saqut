// ============================================================================
// saQut Compiler — Genişletilmiş İfade Düğümleri
// ============================================================================
//
// DİZİN:   src/parser/nodes/expressions.hpp
// KATMAN:  Katman 3 — CallExpression, Postfix, MemberAccess, Index, ScopeCall,
//           ArrayLiteral, Cast, Ternary
//
// AMAÇ:
//   BinaryExpression ve Literal dışında kalan tüm ifade türlerini tanımlar.
//   Fonksiyon çağrısı, postfix ++/--, üye erişimi, dizi indeksi, scope call,
//   array literal, tip dönüşümü (as) ve ternary ifade.
//
// ============================================================================

#ifndef SAQUT_AST_EXPR_EXT
#define SAQUT_AST_EXPR_EXT

#include "parser/ast_node.hpp"

class PostfixNode : public ExpressionNode {
public:
    ASTNode*  operand  = nullptr;
    TokenType Operator;
    // #237: önek biçimi (++x) de bu düğümle temsil edilir. Yan etki iki
    // biçimde aynıdır; fark yalnız döndürülen değerdedir — önek YENİ,
    // sonek ESKİ değeri verir.
    bool      isPrefix = false;
    PostfixNode();
    ~PostfixNode() override { delete operand; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

class CallExpressionNode : public ExpressionNode {
public:
    ASTNode* callee = nullptr;
    std::vector<ASTNode*> arguments;
    CallExpressionNode();
    ~CallExpressionNode() override { delete callee; for (auto* a : arguments) delete a; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

class MemberAccessNode : public ExpressionNode {
public:
    ASTNode*   object = nullptr;
    std::string member;
    bool       arrow = false;
    MemberAccessNode();
    ~MemberAccessNode() override { delete object; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

class IndexExpressionNode : public ExpressionNode {
public:
    ASTNode* object = nullptr;
    ASTNode* index  = nullptr;
    IndexExpressionNode();
    ~IndexExpressionNode() override { delete object; delete index; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

class ArrayLiteralNode : public ExpressionNode {
public:
    std::vector<ASTNode*> elements;
    ArrayLiteralNode();
    ~ArrayLiteralNode() override { for (auto* e : elements) delete e; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

// Built-in metod çağrısı: E::method(args)  (ör. int::push(arr, 12))
class ScopeCallNode : public ExpressionNode {
public:
    std::string leftTypeName;   // "int", "array", "string", "struct", "Person", ...
    std::string methodName;     // "push", "pop", "upper", "toJson", ...
    std::vector<ASTNode*> arguments;
    int         builtinId = -1; // TypeChecker çözer; IR codegen kullanır
    // ADR-033 (#85): UFCS nokta çağrısı — expr.method(args) şekeri.
    // true ise leftTypeName boştur, receiver arguments[0]'dadır; kategori
    // TypeChecker'da receiver TİPİNDEN çözülür. IR aynı CALLHOST'a düşer.
    bool        dotCall = false;

    ScopeCallNode();
    ~ScopeCallNode() override { for (auto* a : arguments) delete a; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

// ADR-026: expr as TargetType[?]
class CastExpressionNode : public ExpressionNode {
public:
    ASTNode*    operand        = nullptr;
    std::string targetTypeName;     // "int", "float", "bool", "string"
    bool        targetNullable = false; // as int? → true
    CastExpressionNode();
    ~CastExpressionNode() override { delete operand; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

#endif
