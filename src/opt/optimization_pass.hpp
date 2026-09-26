// ============================================================================
// saQut — OptimizationPass (Soyut Temel Sınıf, ADR-009)
// ============================================================================
//
// DİZİN:   src/opt/optimization_pass.hpp
// KATMAN:  AST optimizasyonu — tüm optimizasyon geçişleri için soyut arayüz
//
// AMAÇ:
//   Her somut pasaj (ConstantFoldingPass, DeadCodeElimPass) bu sınıftan
//   türetilir ve run()/name() metodlarını implemente eder. OptimizationManager
//   tüm pass'leri bu arayüz üzerinden yönetir.
//
// ============================================================================

#ifndef SAQUT_OPT_PASS
#define SAQUT_OPT_PASS

#include <string>
#include "parser/ast_node.hpp"
#include "symbol/symbol_table.hpp"

class OptimizationPass {
public:
    virtual ~OptimizationPass() = default;

    // Pass'i çalıştırır. En az bir dönüşüm yaptıysa true döndürür.
    // root: optimize edilecek klonlanmış AST kökü (sahiplik değişmez).
    virtual bool run(ASTNode* root, SymbolTable* table) = 0;

    virtual const std::string& name() const = 0;
};

#endif // SAQUT_OPT_PASS
