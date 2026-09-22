// ============================================================================
// saQut — AST Derin Klonlama (ADR-007)
//
// deepClone(node): tüm ağacı kopyalar, parent pointer'ları yeniden bağlar.
// IdentifierNode::resolvedSymbol orijinal sembol tablosunu gösterir (read-only,
// optimizasyon için yeterli — ADR-007).
// ============================================================================

#ifndef SAQUT_OPT_AST_CLONE
#define SAQUT_OPT_AST_CLONE

#include "parser/ast_node.hpp"
#include "parser/nodes/program.hpp"
#include "parser/nodes/declarations.hpp"
#include "parser/nodes/statements.hpp"
#include "parser/nodes/expressions.hpp"
#include "parser/nodes/binary_expr.hpp"
#include "parser/nodes/literal.hpp"
#include "parser/nodes/identifier.hpp"

inline ASTNode* deepClone(ASTNode* node);

// ── Yardımcı: typed pointer klonla ve parent'ı bağla ────────────────────────
static inline ASTNode* cloneChild(ASTNode* child, ASTNode* newParent) {
    if (!child) return nullptr;
    ASTNode* c = deepClone(child);
    c->parent = newParent;
    return c;
}

inline ASTNode* deepClone(ASTNode* node) {
    if (!node) return nullptr;

    switch (node->kind) {

    // ── ProgramNode ──────────────────────────────────────────────────────────
    case ASTKind::Program: {
        auto* src = static_cast<ProgramNode*>(node);
        auto* dst = new ProgramNode();
        dst->loc = src->loc;
        for (auto* ch : src->getChildren()) dst->addChild(deepClone(ch));
        return dst;
    }

    // ── FunctionDeclNode ─────────────────────────────────────────────────────
    case ASTKind::FunctionDecl: {
        auto* src = static_cast<FunctionDeclNode*>(node);
        auto* dst = new FunctionDeclNode();
        dst->loc        = src->loc;
        dst->name       = src->name;
        dst->returnType = src->returnType;
        for (auto* p : src->params) {
            auto* cp = static_cast<VariableDeclNode*>(deepClone(p));
            cp->parent = dst;
            dst->params.push_back(cp);
        }
        for (auto* ch : src->getChildren()) dst->addChild(deepClone(ch));
        return dst;
    }

    // ── VariableDeclNode ─────────────────────────────────────────────────────
    case ASTKind::VariableDecl: {
        auto* src = static_cast<VariableDeclNode*>(node);
        auto* dst = new VariableDeclNode();
        dst->loc         = src->loc;
        dst->name        = src->name;
        dst->varType     = src->varType;
        dst->isReachable = src->isReachable;
        if (src->initExpr) dst->initExpr = cloneChild(src->initExpr, dst);
        for (auto* ch : src->getChildren()) dst->addChild(deepClone(ch));
        return dst;
    }

    // ── StructDeclNode ───────────────────────────────────────────────────────
    case ASTKind::StructDecl: {
        auto* src = static_cast<StructDeclNode*>(node);
        auto* dst = new StructDeclNode();
        dst->loc  = src->loc;
        dst->name = src->name;
        for (auto* ch : src->getChildren()) dst->addChild(deepClone(ch));
        return dst;
    }

    // ── BlockNode ────────────────────────────────────────────────────────────
    case ASTKind::Block: {
        auto* src = static_cast<BlockNode*>(node);
        auto* dst = new BlockNode();
        dst->loc         = src->loc;
        dst->isReachable = src->isReachable;
        for (auto* ch : src->getChildren()) dst->addChild(deepClone(ch));
        return dst;
    }

    // ── IfStatementNode ──────────────────────────────────────────────────────
    case ASTKind::IfStatement: {
        auto* src = static_cast<IfStatementNode*>(node);
        auto* dst = new IfStatementNode();
        dst->loc         = src->loc;
        dst->isReachable = src->isReachable;
        dst->condition   = cloneChild(src->condition,  dst);
        dst->thenBranch  = cloneChild(src->thenBranch, dst);
        dst->elseBranch  = cloneChild(src->elseBranch, dst);
        return dst;
    }

    // ── WhileStatementNode ───────────────────────────────────────────────────
    case ASTKind::WhileStatement: {
        auto* src = static_cast<WhileStatementNode*>(node);
        auto* dst = new WhileStatementNode();
        dst->loc         = src->loc;
        dst->isReachable = src->isReachable;
        dst->condition   = cloneChild(src->condition, dst);
        dst->body        = cloneChild(src->body,      dst);
        return dst;
    }

    // ── ForStatementNode ─────────────────────────────────────────────────────
    case ASTKind::ForStatement: {
        auto* src = static_cast<ForStatementNode*>(node);
        auto* dst = new ForStatementNode();
        dst->loc         = src->loc;
        dst->isReachable = src->isReachable;
        dst->init        = cloneChild(src->init,      dst);
        dst->condition   = cloneChild(src->condition, dst);
        dst->update      = cloneChild(src->update,    dst);
        dst->body        = cloneChild(src->body,      dst);
        return dst;
    }

    // ── DoWhileStatementNode ─────────────────────────────────────────────────
    case ASTKind::DoWhileStatement: {
        auto* src = static_cast<DoWhileStatementNode*>(node);
        auto* dst = new DoWhileStatementNode();
        dst->loc         = src->loc;
        dst->isReachable = src->isReachable;
        dst->body        = cloneChild(src->body,      dst);
        dst->condition   = cloneChild(src->condition, dst);
        return dst;
    }

    // ── ReturnStatementNode ──────────────────────────────────────────────────
    case ASTKind::ReturnStatement: {
        auto* src = static_cast<ReturnStatementNode*>(node);
        auto* dst = new ReturnStatementNode();
        dst->loc         = src->loc;
        dst->isReachable = src->isReachable;
        dst->value       = cloneChild(src->value, dst);
        return dst;
    }

    // ── BreakStatementNode ───────────────────────────────────────────────────
    case ASTKind::BreakStatement: {
        auto* src = static_cast<BreakStatementNode*>(node);
        auto* dst = new BreakStatementNode();
        dst->loc         = src->loc;
        dst->isReachable = src->isReachable;
        return dst;
    }

    // ── ContinueStatementNode ────────────────────────────────────────────────
    case ASTKind::ContinueStatement: {
        auto* src = static_cast<ContinueStatementNode*>(node);
        auto* dst = new ContinueStatementNode();
        dst->loc         = src->loc;
        dst->isReachable = src->isReachable;
        return dst;
    }

    // ── ExpressionStatementNode ──────────────────────────────────────────────
    case ASTKind::ExpressionStatement: {
        auto* src = static_cast<ExpressionStatementNode*>(node);
        auto* dst = new ExpressionStatementNode();
        dst->loc         = src->loc;
        dst->isReachable = src->isReachable;
        dst->expression  = cloneChild(src->expression, dst);
        return dst;
    }

    // ── BinaryExpressionNode ─────────────────────────────────────────────────
    case ASTKind::BinaryExpression: {
        auto* src = static_cast<BinaryExpressionNode*>(node);
        auto* dst = new BinaryExpressionNode();
        dst->loc          = src->loc;
        dst->Operator     = src->Operator;
        dst->resolvedType = src->resolvedType;
        dst->isConstant   = src->isConstant;
        dst->Left         = cloneChild(src->Left,  dst);
        dst->Right        = cloneChild(src->Right, dst);
        return dst;
    }

    // ── LiteralNode ──────────────────────────────────────────────────────────
    case ASTKind::Literal: {
        auto* src = static_cast<LiteralNode*>(node);
        auto* dst = new LiteralNode();
        dst->loc            = src->loc;
        dst->literalType    = src->literalType;
        dst->literalBase    = src->literalBase;
        dst->isFloatValue   = src->isFloatValue;
        dst->lexerToken     = src->lexerToken;   // orijinal token, salt-okunur
        dst->parserToken    = src->parserToken;  // aynı token pointer, salt-okunur
        dst->resolvedType   = src->resolvedType;
        dst->isConstant     = src->isConstant;
        dst->hasDirectValue = src->hasDirectValue;
        dst->directIntValue = src->directIntValue;
        return dst;
    }

    // ── IdentifierNode ───────────────────────────────────────────────────────
    case ASTKind::Identifier: {
        auto* src = static_cast<IdentifierNode*>(node);
        auto* dst = new IdentifierNode();
        dst->loc            = src->loc;
        dst->lexerToken     = src->lexerToken;
        dst->parserToken    = src->parserToken;
        dst->resolvedSymbol = src->resolvedSymbol; // orijinal tablo, salt-okunur
        dst->resolvedType   = src->resolvedType;
        dst->isConstant     = src->isConstant;
        return dst;
    }

    // ── PostfixNode ──────────────────────────────────────────────────────────
    case ASTKind::Postfix: {
        auto* src = static_cast<PostfixNode*>(node);
        auto* dst = new PostfixNode();
        dst->loc          = src->loc;
        dst->Operator     = src->Operator;
        dst->isPrefix     = src->isPrefix;   // #237: kopyalanmazsa ++x sonek gibi davranır
        dst->resolvedType = src->resolvedType;
        dst->isConstant   = src->isConstant;
        dst->operand      = cloneChild(src->operand, dst);
        return dst;
    }

    // ── CallExpressionNode ───────────────────────────────────────────────────
    case ASTKind::Call: {
        auto* src = static_cast<CallExpressionNode*>(node);
        auto* dst = new CallExpressionNode();
        dst->loc          = src->loc;
        dst->resolvedType = src->resolvedType;
        dst->isConstant   = src->isConstant;
        dst->callee       = cloneChild(src->callee, dst);
        for (auto* arg : src->arguments) {
            ASTNode* ca = deepClone(arg);
            ca->parent = dst;
            dst->arguments.push_back(ca);
        }
        return dst;
    }

    // ── MemberAccessNode ─────────────────────────────────────────────────────
    case ASTKind::MemberAccess: {
        auto* src = static_cast<MemberAccessNode*>(node);
        auto* dst = new MemberAccessNode();
        dst->loc          = src->loc;
        dst->resolvedType = src->resolvedType;
        dst->isConstant   = src->isConstant;
        dst->member       = src->member;
        dst->arrow        = src->arrow;
        dst->object       = cloneChild(src->object, dst);
        return dst;
    }

    // ── IndexExpressionNode ──────────────────────────────────────────────────
    case ASTKind::IndexExpression: {
        auto* src = static_cast<IndexExpressionNode*>(node);
        auto* dst = new IndexExpressionNode();
        dst->loc          = src->loc;
        dst->resolvedType = src->resolvedType;
        dst->isConstant   = src->isConstant;
        dst->object       = cloneChild(src->object, dst);
        dst->index        = cloneChild(src->index,  dst);
        return dst;
    }

    // ── ScopeCallNode ────────────────────────────────────────────────────────
    case ASTKind::ScopeCall: {
        auto* src = static_cast<ScopeCallNode*>(node);
        auto* dst = new ScopeCallNode();
        dst->loc           = src->loc;
        dst->resolvedType  = src->resolvedType;
        dst->isConstant    = src->isConstant;
        dst->leftTypeName  = src->leftTypeName;
        dst->methodName    = src->methodName;
        dst->builtinId     = src->builtinId;
        dst->dotCall       = src->dotCall;
        for (auto* arg : src->arguments) {
            ASTNode* ca = deepClone(arg);
            ca->parent = dst;
            dst->arguments.push_back(ca);
        }
        return dst;
    }

    // ── UnaryExpression ──────────────────────────────────────────────────────
    // UnaryExpression şu anda ayrı bir sınıf değil; parser tarafından
    // BinaryExpression veya PostfixNode olarak temsil ediliyor.
    // Gelecekte eklenmesi gerekirse buraya eklenecek.
    default:
        // Bilinmeyen node tipi — klonlanamaz; orijinal döndür (güvenli değil ama
        // derleme aşamasında tüm tipler yukarıda kaplanmalı).
        return node;
    }
}

#endif // SAQUT_OPT_AST_CLONE
