// ============================================================================
// saQut Compiler — StructuralValidator Gerçeklemesi
// ============================================================================
//
// DİZİN:   src/semantic/structural_validator.cpp
// KATMAN:  Faz 3 — Yapısal doğrulama
//
// AMAÇ:
//   AST'yi recursive olarak gezerek break/continue/return bağlam
//   kontrollerini ve fonksiyon içinde bildirim yasağını uygular.
//
// ============================================================================

#include "semantic/structural_validator.hpp"
#include "parser/nodes/declarations.hpp"
#include "parser/nodes/statements.hpp"
#include "parser/nodes/expressions.hpp"
#include "parser/nodes/binary_expr.hpp"

void StructuralValidator::validate(ASTNode* program) {
    if (!program) return;
    for (ASTNode* child : program->getChildren())
        walkDecl(child);
}

void StructuralValidator::walkDecl(ASTNode* node) {
    if (!node) return;
    if (node->kind == ASTKind::FunctionDecl) {
        auto* fn = (FunctionDeclNode*)node;
        inFunction_ = true;
        auto& ch = fn->getChildren();
        if (!ch.empty()) walkStmt(ch[0]);
        inFunction_ = false;
    }
}

// ADR-045: `thread { }` gövdesi ayrı bir fonksiyondur (lambda lifting): dıştaki
// döngüye break/continue edemez. Gövde sıfır döngü derinliğiyle denetlenir.
// Thread ifadesi bildirim başlatıcısında, ifade deyiminde ya da atamanın sağ
// tarafında bulunur.
void StructuralValidator::walkThreadBodies(ASTNode* expr) {
    if (!expr) return;
    if (expr->kind == ASTKind::ThreadExpr) {
        auto* te = static_cast<ThreadExprNode*>(expr);
        const int savedLoop = loopDepth_, savedPure = pureLoopDepth_;
        loopDepth_ = pureLoopDepth_ = 0;
        walkStmt(te->body);
        loopDepth_ = savedLoop;
        pureLoopDepth_ = savedPure;
        return;
    }
    if (expr->kind == ASTKind::BinaryExpression)
        walkThreadBodies(static_cast<BinaryExpressionNode*>(expr)->Right);
}

void StructuralValidator::walkStmt(ASTNode* node) {
    if (!node) return;

    switch (node->kind) {

    case ASTKind::Block:
        for (ASTNode* child : node->getChildren()) walkStmt(child);
        break;

    case ASTKind::IfStatement: {
        auto* ifn = (IfStatementNode*)node;
        if (ifn->thenBranch) walkStmt(ifn->thenBranch);
        if (ifn->elseBranch) walkStmt(ifn->elseBranch);
        break;
    }

    case ASTKind::WhileStatement: {
        auto* ws = (WhileStatementNode*)node;
        loopDepth_++; pureLoopDepth_++;
        if (ws->body) walkStmt(ws->body);
        loopDepth_--; pureLoopDepth_--;
        break;
    }

    case ASTKind::DoWhileStatement: {
        auto* dw = (DoWhileStatementNode*)node;
        loopDepth_++; pureLoopDepth_++;
        if (dw->body) walkStmt(dw->body);
        loopDepth_--; pureLoopDepth_--;
        break;
    }

    case ASTKind::ForStatement: {
        auto* fs = (ForStatementNode*)node;
        loopDepth_++; pureLoopDepth_++;
        if (fs->init) walkStmt(fs->init);
        if (fs->body) walkStmt(fs->body);
        loopDepth_--; pureLoopDepth_--;
        break;
    }

    case ASTKind::SwitchStatement: {
        auto* sw = (SwitchStatementNode*)node;
        loopDepth_++; // break switch içinde geçerli
        for (auto& c : sw->cases)
            for (auto* s : c.body) walkStmt(s);
        loopDepth_--;
        break;
    }

    case ASTKind::BreakStatement:
        if (loopDepth_ == 0)
            diag_.report("E004", node->loc,
                "'break' cannot be used outside a loop or switch",
                "'break' is only valid inside for, while, do-while or switch — move this statement into a loop body");
        break;

    case ASTKind::ContinueStatement:
        if (pureLoopDepth_ == 0)
            diag_.report("E004", node->loc,
                "'continue' cannot be used outside a loop",
                "'continue' is only valid inside for, while or do-while — move this statement into a loop body");
        break;

    case ASTKind::ReturnStatement:
        if (!inFunction_)
            diag_.report("E005", node->loc,
                "'return' cannot be used outside a function",
                "move the return statement into a function body: `func name() : type { return value; }`");
        break;

    case ASTKind::StructDecl:
        if (inFunction_)
            diag_.report("E011", node->loc,
                "struct declaration is not allowed inside a function",
                "move the struct definition to the top level, outside all functions");
        break;

    case ASTKind::EnumDecl:
        if (inFunction_)
            diag_.report("E011", node->loc,
                "enum declaration is not allowed inside a function",
                "move the enum definition to the top level, outside all functions");
        break;

    case ASTKind::ImportDecl:
        if (inFunction_)
            diag_.report("E011", node->loc,
                "import declaration is not allowed inside a function",
                "move the import statement to the top of the file, outside all functions");
        break;

    case ASTKind::FunctionDecl:
        if (inFunction_)
            diag_.report("E011", node->loc,
                "nested function declaration is not allowed",
                "saQut does not support nested functions — move this declaration to the top level");
        break;

    case ASTKind::VariableDecl: {
        walkThreadBodies(static_cast<VariableDeclNode*>(node)->initExpr);   // ADR-045
        for (ASTNode* sib : node->getChildren())
            if (sib->kind == ASTKind::VariableDecl) walkStmt(sib);
        break;
    }

    case ASTKind::ExpressionStatement:
        walkThreadBodies(static_cast<ExpressionStatementNode*>(node)->expression);   // ADR-045
        break;

    default:
        break;
    }
}
