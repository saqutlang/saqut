// ============================================================================
// saQut Compiler — Yapısal Doğrulayıcı (StructuralValidator)
// ============================================================================
//
// DİZİN:   src/semantic/structural_validator.hpp
// KATMAN:  Faz 3 — break/continue/return bağlamı, iç-içe bildirim yasağı
//
// AMAÇ:
//   AST'yi gezerek yapısal kuralları denetler: break/continue'in doğru
//   bağlamda kullanımı (E004), fonksiyon dışı return (E005), fonksiyon
//   içinde struct/enum/fonksiyon bildirimi yasağı (E011).
//
// ============================================================================

#ifndef SAQUT_SEMANTIC_STRUCTURAL_VALIDATOR
#define SAQUT_SEMANTIC_STRUCTURAL_VALIDATOR

#include "diagnostic/diagnostic_engine.hpp"
#include "parser/ast_node.hpp"

class StructuralValidator {
public:
    explicit StructuralValidator(DiagnosticEngine& diag) : diag_(diag) {}

    void validate(ASTNode* program);

private:
    void walkDecl(ASTNode* node);
    void walkStmt(ASTNode* node);
    void walkThreadBodies(ASTNode* expr);   // ADR-045

    DiagnosticEngine& diag_;
    int  loopDepth_   = 0; // döngü + switch derinliği (break için)
    int  pureLoopDepth_ = 0; // yalnızca döngü derinliği (continue için)
    bool inFunction_  = false;
};

#endif // SAQUT_SEMANTIC_STRUCTURAL_VALIDATOR
