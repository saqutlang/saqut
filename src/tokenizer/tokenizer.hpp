// ============================================================================
// saQut Compiler — Sözcüksel Analiz Motoru (Tokenizer)
// ============================================================================
//
// DİZİN:   src/tokenizer/tokenizer.hpp
// KATMAN:  Tokenizer — Lexer'ın karakter akışını token dizisine dönüştürür
// BAĞIMLI: lexer/lexer.hpp, tokenizer/token.hpp, diagnostic/diagnostic_engine.hpp
//
// Her token'ın hem kaba sınıfını (category) hem kesin türünü (kind) yazar;
// keyword ve operatör türleri tokenizer/token_kind.hpp tablolarından gelir.
// Hangi karakter dizisinin tek operatör olduğuna tokenizer.cpp scope()'daki
// karakter switch'i karar verir.
//
// Sözcüksel hatalar (E906 bilinmeyen kaçış, E907 kapanmamış string) verilen
// DiagnosticEngine'e raporlanır. diag verilmezse (ör. `saqut tokens`) hatalar
// raporlanmaz, token'lar yine üretilir.
//
// YENİ SÖZCÜKSEL HATA: ilgili okuma fonksiyonunda report(...) çağrısı.
//
// ============================================================================

#ifndef SAQUT_TOKENIZER
#define SAQUT_TOKENIZER

#include <string>
#include <vector>
#include "lexer/lexer.hpp"
#include "tokenizer/token.hpp"

class DiagnosticEngine;

class Tokenizer {
public:
    explicit Tokenizer(DiagnosticEngine* diag = nullptr) : diag_(diag) {}

    TokenList scan(std::string input, std::string filePath = "");

private:
    Lexer             lexer;
    DiagnosticEngine* diag_ = nullptr;

    Token*           scope();
    IdentifierToken* readIdentifier();
    StringToken*     readString();
    void skipOneLineComment();
    void skipMultiLineComment();
    void report(const SourceLocation& loc, const char* code, const std::string& message);
};

#endif // SAQUT_TOKENIZER
