// ============================================================================
// saQut Compiler — ErrorNode (Hata Düğümü)
// ============================================================================
//
// DİZİN:   src/parser/nodes/error_node.hpp
// KATMAN:  Parser — sözdizimi hatası yer tutucusu
//
// AMAÇ:
//   Parser sözdizimsel bir hatayla karşılaşınca konumlu bir tanı üretir,
//   bilinen bir sınıra kadar token atlar (panic-mode recovery) ve bu
//   düğümü ağaca bırakıp devam eder. Analiz katmanları (SymbolCollector,
//   TypeChecker, StructuralValidator) ErrorNode'u switch/default ile
//   sessizce atlar — AST'nin geri kalanı analiz edilmeye devam eder.
//
// ============================================================================

#ifndef SAQUT_AST_ERROR_NODE
#define SAQUT_AST_ERROR_NODE

#include "parser/ast_node.hpp"

class ErrorNode : public ASTNode {
public:
    std::string message; // hangi tanının üretildiği (debug/log amaçlı)
    std::string code;     // ör. "E901"

    ErrorNode();
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

#endif // SAQUT_AST_ERROR_NODE
