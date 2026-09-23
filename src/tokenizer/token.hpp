// ============================================================================
// saQut Compiler — Token Sınıfları
// ============================================================================
//
// DİZİN:   src/tokenizer/token.hpp
// KATMAN:  Katman 2 — Tokenizer ile Parser arasında veri yapısı
// BAĞIMLI: Yok (sadece <string>)
//
// AMAÇ:
//   Tüm token tiplerinin temel sınıfları. 6 adet polimorfik token tipi:
//   Token → NumberToken, StringToken, OperatorToken, DelimiterToken,
//           KeywordToken, IdentifierToken
//
// ============================================================================

#ifndef SAQUT_TOKENIZER_TOKEN
#define SAQUT_TOKENIZER_TOKEN

#include <string>
#include "core/location.hpp"

class Token {
protected:
    std::string type;
public:
    int start = 0;
    int end   = 0;
    SourceLocation loc;  // Token'ın kaynak koddaki konumu
    std::string token;
    std::string gettype() { return type; }
    virtual ~Token() = default;
};

class StringToken : public Token {
public:
    StringToken()             { type = "string"; }
    std::string context;
    int size = 0;
    // Tokenizer'ın tanı kanalı yok; sözcüksel hatalar burada işaretlenir ve
    // parser (diag sahibi) string literalini kurarken raporlar (#256).
    std::string badEscapes;      // tanınmayan kaçışların harfleri (`\x` → 'x')
    bool        unterminated = false; // kapanış '"' bulunamadan dosya bitti
};

class NumberToken : public Token {
public:
    NumberToken()             { type = "number"; }
    bool isFloat    = false;
    bool hasEpsilon = false;
    int base        = 10;
};

class OperatorToken : public Token {
public:
    OperatorToken()           { type = "operator"; }
};

class DelimiterToken : public Token {
public:
    DelimiterToken()          { type = "delimiter"; }
};

class KeywordToken : public Token {
public:
    KeywordToken()            { type = "keyword"; }
};

class IdentifierToken : public Token {
public:
    IdentifierToken()         { type = "identifier"; }
    std::string context;
    int size = 0;
};

#endif // SAQUT_TOKENIZER_TOKEN
