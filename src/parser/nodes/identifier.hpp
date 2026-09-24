// ============================================================================
// saQut Compiler — IdentifierNode (Tanımlayıcı Düğümü)
// ============================================================================
//
// DİZİN:   src/parser/nodes/identifier.hpp
// KATMAN:  Katman 3 — Değişken/fonksiyon ismi
//
// AMAÇ:
//   Kaynak koddaki tanımlayıcıları (değişken adı, fonksiyon adı, tip adı)
//   temsil eder. Sembol tablosuna bağlantı için Symbol* alanı içerir.
//
// ============================================================================

#ifndef SAQUT_AST_IDENTIFIER
#define SAQUT_AST_IDENTIFIER

#include "parser/ast_node.hpp"

struct Symbol; // TODO(faz-2): sembol tablosu (Symbol) tanımlandığında bağlanacak

class IdentifierNode : public ExpressionNode {
public:
    Token*       lexerToken  = nullptr;
    ParserToken  parserToken;

    // TODO(faz-2): isim çözümlemede sembol tablosundaki tanıma bağlanır.
    Symbol* resolvedSymbol = nullptr;

    // ADR-045: bu ad bir `thread { }` gövdesinin içinde, çevreleyen fonksiyonun
    // bir yerelini (yakalanan kopya) gösteriyor. SymbolCollector işaretler;
    // TypeChecker bu ada atamayı reddeder (E016 "yakalanan değişken bir kopyadır").
    bool capturedInThread = false;

    IdentifierNode();
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

#endif
