// ============================================================================
// saQut Compiler — Token Sınıfları
// ============================================================================
//
// DİZİN:   src/tokenizer/token.hpp
// KATMAN:  Tokenizer — tokenizer'ın ürettiği, parser'ın okuduğu nesneler
// BAĞIMLI: core/location.hpp, tokenizer/token_kind.hpp
//
// Her token iki tür bilgisi taşır:
//   category — kaba sınıf (keyword, identifier, number, ...). `saqut tokens`
//              çıktısı ve LSP bunu okur.
//   kind     — kesin tür (KW_WHILE, PLUS, NUMBER, ...). Parser bunu okur.
// İkisini de tokenizer doldurur; sonraki katmanlar yeniden sınıflandırmaz.
//
// Alt sınıflar türe özgü ek alan taşır (NumberToken: base/isFloat,
// StringToken: kaçışları çözülmüş metin).
//
// ============================================================================

#ifndef SAQUT_TOKENIZER_TOKEN
#define SAQUT_TOKENIZER_TOKEN

#include <string>
#include <vector>
#include "core/location.hpp"
#include "tokenizer/token_kind.hpp"

enum class TokenCategory { Identifier, Keyword, Number, String, Operator, Delimiter, End };

// `saqut tokens` çıktısındaki etiket ("keyword", "identifier", ...).
inline const char* tokenCategoryName(TokenCategory c) {
    switch (c) {
        case TokenCategory::Identifier: return "identifier";
        case TokenCategory::Keyword:    return "keyword";
        case TokenCategory::Number:     return "number";
        case TokenCategory::String:     return "string";
        case TokenCategory::Operator:   return "operator";
        case TokenCategory::Delimiter:  return "delimiter";
        case TokenCategory::End:        return "";
    }
    return "";
}

class Token {
public:
    explicit Token(TokenCategory c = TokenCategory::End) : category(c) {}
    virtual ~Token() = default;

    TokenCategory  category;
    TokenType      kind = TokenType::SVR_VOID;  // Tokenizer::scan() doldurur
    int            start = 0;
    int            end   = 0;
    SourceLocation loc;    // Token'ın kaynak koddaki konumu
    std::string    token;  // ham metin (string'de tırnaklar ve kaçışlar dahil)
};

// Tokenizer::scan() üretir, Parser::parse() okur. Ham pointer'lar: sahiplik
// çağırandadır (ModuleGraph ya da komut, AST'den SONRA siler — AST token
// pointer'larını tutar).
using TokenList = std::vector<Token*>;

class StringToken : public Token {
public:
    StringToken() : Token(TokenCategory::String) {}
    std::string context;  // kaçışları çözülmüş metin
    int size = 0;
};

class NumberToken : public Token {
public:
    NumberToken() : Token(TokenCategory::Number) {}
    bool isFloat    = false;
    bool hasEpsilon = false;
    int base        = 10;
};

class OperatorToken : public Token {
public:
    OperatorToken() : Token(TokenCategory::Operator) {}
};

class DelimiterToken : public Token {
public:
    DelimiterToken() : Token(TokenCategory::Delimiter) {}
};

class KeywordToken : public Token {
public:
    KeywordToken() : Token(TokenCategory::Keyword) {}
};

class IdentifierToken : public Token {
public:
    IdentifierToken() : Token(TokenCategory::Identifier) {}
    std::string context;
    int size = 0;
};

#endif // SAQUT_TOKENIZER_TOKEN
