// ============================================================================
// saQut Compiler — ParserToken ve Operatör Öncelik Tablosu
// ============================================================================
//
// DİZİN:   src/parser/parser_token.hpp
// KATMAN:  Parser — Pratt ifade ayrıştırıcısının tabloları
// BAĞIMLI: tokenizer/token.hpp (Token, TokenType)
//
// Bu dosya parser'a özgü iki şeyi tanımlar:
//   1. TokenPrecedence() / RightAssociative(): Pratt parser'ın öncelik ve
//      birleşme tablosu (ADR-002). Yeni bir ikili operatör = TokenType'a değer
//      (tokenizer/token_kind.hpp) + burada öncelik + parser'da denotation.
//   2. ParserToken: parser'ın okuduğu token (Token* + TokenType).
//
// Token'ın TÜRÜ (TokenType) tokenizer katmanında belirlenir (token_kind.hpp);
// parser onu yeniden sınıflandırmaz.
//
// ============================================================================

#ifndef SAQUT_PARSER_PARSER_TOKEN
#define SAQUT_PARSER_PARSER_TOKEN

#include <cstdint>
#include <initializer_list>
#include "tokenizer/token.hpp"

// ============================================================================
// TokenPrecedence — Operatör Öncelik Tablosu (Pratt Parser'ın Kalbi)
// ============================================================================
//
// Yüksek sayı = daha sıkı bağlanma (önce işlenir).
//
// ÖNCELİK SEVİYELERİ (yüksekten düşüğe):
//   18: Üye erişimi       . -> [ ] ( )     — En yüksek
//   17: Postfix           ++ --
//   16: Unary prefix      ! ~ + -
//   15: Üs alma           **               — Sağ birleşmeli
//   14: Çarpma/Bölme      * / %
//   13: Toplama/Çıkarma   + -
//   12: Bitsel kaydırma   << >>  ve  as (ADR-026)
//   11: İlişkisel         < <= > >=
//   10: Eşitlik           == !=
//    9: Bitsel VE         &
//    8: Bitsel XOR        ^ (#230)
//    7: Bitsel VEYA       |
//    6: Mantıksal VE      &&
//    5: Mantıksal VEYA    ||
//    4: `?`               ? (ternary YOK — nullable tip işareti)
//    3: `:`               : (ternary else YOK — etiket)
//    2: Atama             = += -= vb.      — Sağ birleşmeli
//    1: Virgül            ,
//    0: Önceliksiz        (değerler, EOF, bilinmeyen)
//
// KARAR (#230, ürün sahibi): ^ (CARET) bitsel XOR'tur ve Level 8'dedir;
// üs alma yalnız ** (STAR_STAR, Level 15) ile yapılır. C/C++/Python ile
// hizalıdır: & (9) ile | (7) arasına düşer, unary '-'den (13) gevşektir;
// `-a ^ b` doğru şekilde `(-a) ^ b` olur.
//
inline uint16_t TokenPrecedence(TokenType type) {
    switch (type) {
        // Level 18: Member access / call
        case TokenType::DOT:
        case TokenType::ARROW:
        case TokenType::LBRACKET:
        case TokenType::LPAREN:
            return 18;

        // Level 17: Postfix
        case TokenType::PLUS_PLUS:
        case TokenType::MINUS_MINUS:
            return 17;

        // Level 16: Unary prefix — sadece her zaman prefix olanlar
        case TokenType::BANG:      // !
        case TokenType::TILDE:     // ~
            return 16;

        // Level 15: Exponentiation
        case TokenType::STAR_STAR: // ** (tek üs operatörü; ^ XOR'dur, #230)
            return 15;

        // Level 14: Multiplicative
        case TokenType::STAR:      // *
        case TokenType::SLASH:     // /
        case TokenType::PERCENT:   // %
            return 14;

        // Level 13: Additive — PLUS ve MINUS hem unary hem binary
        case TokenType::PLUS:      // +
        case TokenType::MINUS:     // -
            return 13;

        // Level 12: Bit shift + as cast (sola-bağlı, aritmetikten gevşek)
        case TokenType::LSHIFT:    // <<
        case TokenType::RSHIFT:    // >>
        case TokenType::KW_AS:     // as (ADR-026)
            return 12;

        // Level 11: Relational
        case TokenType::LESS:      // <
        case TokenType::LESS_EQUAL:// <=
        case TokenType::GREATER:   // >
        case TokenType::GREATER_EQUAL: // >=
            return 11;

        // Level 10: Equality
        case TokenType::EQUAL_EQUAL:   // ==
        case TokenType::BANG_EQUAL:    // !=
            return 10;

        // Level 9: Bitwise AND
        case TokenType::AMPERSAND: // &
            return 9;

        // Level 8: Bitwise XOR
        case TokenType::CARET:     // ^ bitsel XOR (#230; C/Python ile hizalı)
            return 8;
        // Level 7: Bitwise OR
        case TokenType::PIPE:      // |
            return 7;

        // Level 6: Logical AND
        case TokenType::AMPERSAND_AMPERSAND: // &&
            return 6;

        // Level 5: Logical OR
        case TokenType::PIPE_PIPE: // ||
            return 5;

        // Level 4: Ternary
        case TokenType::TERNARY:  // ?
            return 4;
        case TokenType::COLON:     // : (ternary için)
            return 3;             // ternary'den düşük, atamadan yüksek

        // Level 2: Assignment
        case TokenType::EQUAL:     // =
        case TokenType::PLUS_EQUAL:// +=
        case TokenType::MINUS_EQUAL:// -=
        case TokenType::STAR_EQUAL:// *=
        case TokenType::SLASH_EQUAL:// /=
        case TokenType::PERCENT_EQUAL:// %=
        case TokenType::AMPERSAND_EQUAL:// &=
        case TokenType::PIPE_EQUAL:// |=
        case TokenType::CARET_EQUAL:// ^=
        case TokenType::LSHIFT_EQUAL:// <<=
        case TokenType::RSHIFT_EQUAL:// >>=
            return 2;

        // Level 1: Comma
        case TokenType::COMMA:     // ,
            return 1;

        default:
            return 0;  // Önceliksiz: değerler, EOF, bilinmeyen
    }
}

// ============================================================================
// RightAssociative — Sağdan Sola Birleşme Kontrolü
// ============================================================================
//
// Sağ birleşmeli (a OP b OP c = a OP (b OP c)): ** (üs), = ve birleşik
// atamalar (+=, -=, ...), ? (ternary). Diğer tüm operatörler sol birleşmelidir.
// NOT (#230): ^ bitsel XOR'dur, sol birleşimlidir.
//
inline bool RightAssociative(TokenType type) {
    switch (type) {
        case TokenType::STAR_STAR:  // ** (üs)
        case TokenType::EQUAL:      // =
        case TokenType::PLUS_EQUAL: // +=
        case TokenType::MINUS_EQUAL:// -=
        case TokenType::STAR_EQUAL: // *=
        case TokenType::SLASH_EQUAL:// /=
        case TokenType::PERCENT_EQUAL:// %=
        case TokenType::AMPERSAND_EQUAL:// &=
        case TokenType::PIPE_EQUAL: // |=
        case TokenType::CARET_EQUAL:// ^=
        case TokenType::LSHIFT_EQUAL:// <<=
        case TokenType::RSHIFT_EQUAL:// >>=
        case TokenType::TERNARY:   // ? (ternary)
            return true;
        default:
            return false;
    }
}

// ============================================================================
// IsBinaryOperator — BinaryExpression kurulabilecek infix operatörler
// ============================================================================
//
// Pratt döngüsü önceliği sıfırdan büyük her token'ı parseLeftDenotation'a
// verir. Özel dalı olmayanlardan (çağrı, indeks, üye, `as`, postfix) yalnız
// bu listedekiler BinaryExpression olur; tip denetleyici ve IR yalnız bunları
// tanır. `?` (nullable soneki), `:` (case/etiket), `!`/`~` (yalnız önek) ve
// `,` önceliği olduğu halde ikili operatör DEĞİLDİR (#299).
//
// YENİ İKİLİ OPERATÖR: TokenPrecedence'a seviye + buraya satır + TypeChecker
// ve IRGenerator'da işleyişi.
//
inline bool IsBinaryOperator(TokenType type) {
    switch (type) {
        case TokenType::PLUS:  case TokenType::MINUS: case TokenType::STAR:
        case TokenType::SLASH: case TokenType::PERCENT: case TokenType::STAR_STAR:
        case TokenType::AMPERSAND: case TokenType::PIPE: case TokenType::CARET:
        case TokenType::LSHIFT: case TokenType::RSHIFT:
        case TokenType::LESS: case TokenType::LESS_EQUAL:
        case TokenType::GREATER: case TokenType::GREATER_EQUAL:
        case TokenType::EQUAL_EQUAL: case TokenType::BANG_EQUAL:
        case TokenType::AMPERSAND_AMPERSAND: case TokenType::PIPE_PIPE:
        case TokenType::EQUAL:
        case TokenType::PLUS_EQUAL: case TokenType::MINUS_EQUAL:
        case TokenType::STAR_EQUAL: case TokenType::SLASH_EQUAL:
        case TokenType::PERCENT_EQUAL: case TokenType::AMPERSAND_EQUAL:
        case TokenType::PIPE_EQUAL: case TokenType::CARET_EQUAL:
        case TokenType::LSHIFT_EQUAL: case TokenType::RSHIFT_EQUAL:
            return true;
        default:
            return false;
    }
}

// ============================================================================
// ParserToken — Parser'ın okuduğu token
// ============================================================================
//
// token: tokenizer'ın ürettiği nesne (polimorfik: NumberToken base/isFloat,
//        StringToken çözülmüş metin taşır). Pointer tutulur; değer kopyası
//        alt sınıf alanlarını keserdi (object slicing, commit 40579ca).
// type:  token->kind'ın kopyası — Pratt döngüsü sık okur.
//
struct ParserToken {
    Token*    token = nullptr;               // EOF'ta nullptr (type == SVR_VOID)
    TokenType type  = TokenType::SVR_VOID;

    bool is(TokenType t) const {
        return type == t;
    }

    // Çoklu tip kontrolü: current.is({TokenType::SEMICOLON, TokenType::RPAREN})
    bool is(std::initializer_list<TokenType> types) const {
        for (TokenType t : types)
            if (type == t) return true;
        return false;
    }

    // Pratt döngüsü: while (minPrec < current.getPowerOperator()) { ... }
    uint16_t getPowerOperator() const {
        return TokenPrecedence(type);
    }

    bool isRightAssociative() const {
        return RightAssociative(type);
    }
};

#endif // SAQUT_PARSER_PARSER_TOKEN
