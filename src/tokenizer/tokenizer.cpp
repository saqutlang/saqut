// ============================================================================
// saQut Compiler — Sözcüksel Analiz (Tokenizer) Gerçeklemesi
// ============================================================================
//
// DİZİN:   src/tokenizer/tokenizer.cpp
// KATMAN:  Tokenizer — Lexer yardımıyla kaynak kodu token'lara ayırır
// BAĞIMLI: tokenizer/tokenizer.hpp, tokenizer/token_kind.hpp
//
// scope() tek token okur (karakter switch'i → operatör/delimiter; aksi halde
// sayı, string, ad). scan() döngüde scope() çağırır ve her token'ın kesin
// türünü (kind) token_kind.hpp tablolarından yazar.
//
// ============================================================================

#include "tokenizer/tokenizer.hpp"
#include "diagnostic/diagnostic_engine.hpp"

void Tokenizer::report(const SourceLocation& loc, const char* code, const std::string& message) {
    if (diag_) diag_->report(code, loc, message);
}

// Token'ın kesin türü. Keyword ve operatör türleri tablodan; tabloda olmayan
// operatör metni (scope()'a eklenip OPERATOR_MAP'e eklenmemiş) SVR_VOID olur
// ve parser onu beklenmeyen token olarak raporlar.
static TokenType kindOf(const Token& t) {
    switch (t.category) {
        case TokenCategory::Identifier: return TokenType::IDENTIFIER;
        case TokenCategory::Number:     return TokenType::NUMBER;
        case TokenCategory::String:     return TokenType::STRING;
        case TokenCategory::Keyword: {
            auto it = KEYWORD_MAP.find(t.token);
            return it != KEYWORD_MAP.end() ? it->second : TokenType::SVR_VOID;
        }
        case TokenCategory::Operator:
        case TokenCategory::Delimiter: {
            auto it = OPERATOR_MAP.find(t.token);
            return it != OPERATOR_MAP.end() ? it->second : TokenType::SVR_VOID;
        }
        case TokenCategory::End:        return TokenType::SVR_VOID;
    }
    return TokenType::SVR_VOID;
}

// ─────────────────────────────────────────────────────────────────────────────
// Yardımcı makrolar — OperatorToken ve DelimiterToken üretimi
// ─────────────────────────────────────────────────────────────────────────────
#define MAKE_OP(str, len)                      \
    do {                                        \
        OperatorToken* _t = new OperatorToken();\
        _t->start = lexer.getOffset();            \
        _t->loc   = lexer.getLocation();          \
        lexer.toChar(len);                        \
        _t->end   = lexer.getOffset();            \
        _t->token = (str);                      \
        return _t;                              \
    } while(0)

#define MAKE_DEL(str, len)                       \
    do {                                          \
        DelimiterToken* _t = new DelimiterToken();\
        _t->start = lexer.getOffset();              \
        _t->loc   = lexer.getLocation();            \
        lexer.toChar(len);                          \
        _t->end   = lexer.getOffset();              \
        _t->token = (str);                        \
        return _t;                                \
    } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// scan
// ─────────────────────────────────────────────────────────────────────────────
TokenList Tokenizer::scan(std::string input, std::string filePath) {
    TokenList tokens;
    lexer.setSourceText(filePath, input);
    while (true) {
        Token* token = scope();
        // Dosya sonu işareti kategoriyle tanınır, metinle değil: eskiden
        // `token == "EOL"` karşılaştırması `EOL` adlı bir değişkende
        // tokenizasyonu sessizce bitiriyordu (#296).
        if (token->category == TokenCategory::End) {
            delete token;
            break;
        }
        token->kind = kindOf(*token);
        tokens.push_back(token);
        if (lexer.isEnd()) break;
    }
    return tokens;
}

// ─────────────────────────────────────────────────────────────────────────────
// scope — ana dispatch; her token için TEK geçiş
// ─────────────────────────────────────────────────────────────────────────────
Token* Tokenizer::scope() {
    lexer.skipWhiteSpace();

    // Yorumlar
    if (lexer.tryConsume("//"))  { skipOneLineComment();  return scope(); }
    if (lexer.tryConsume("/*"))  { skipMultiLineComment(); return scope(); }

    if (lexer.isEnd()) {
        Token* t = new Token(TokenCategory::End);   // dosya sonu işareti; scan() siler
        return t;
    }

    if (lexer.getchar() == '"') return readString();
    if (lexer.isNumeric())      {
        INumber lem = lexer.readNumeric();
        NumberToken* nt = new NumberToken();
        nt->loc        = lem.startLoc;
        nt->base       = lem.base;
        nt->start      = lem.start;
        nt->end        = lem.end;
        nt->hasEpsilon = lem.hasEpsilon;
        nt->isFloat    = lem.isFloat;
        nt->token      = lem.token;
        return nt;
    }

    char c0 = lexer.getchar();
    char c1 = lexer.getchar(1);  // bir sonraki karakter (tüketmeden bakış)

    // ── Operatörler & Delimiter'lar — switch ile O(1) dispatch ───────────
    switch (c0) {
        // + ++ +=
        case '+':
            if (c1 == '+') MAKE_OP("++", 2);
            if (c1 == '=') MAKE_OP("+=", 2);
            MAKE_OP("+", 1);

        // - -- -= ->
        case '-':
            if (c1 == '-') MAKE_OP("--", 2);
            if (c1 == '=') MAKE_OP("-=", 2);
            if (c1 == '>') MAKE_DEL("->", 2);
            MAKE_OP("-", 1);

        // * *= **
        case '*':
            if (c1 == '=') MAKE_OP("*=", 2);
            if (c1 == '*') MAKE_OP("**", 2);
            MAKE_OP("*", 1);

        // / /=
        case '/':
            if (c1 == '=') MAKE_OP("/=", 2);
            MAKE_OP("/", 1);

        // % %=
        case '%':
            if (c1 == '=') MAKE_OP("%=", 2);
            MAKE_OP("%", 1);

        // < <= << <<=
        case '<':
            if (c1 == '<') {
                if (lexer.getchar(2) == '=') MAKE_OP("<<=", 3);
                MAKE_OP("<<", 2);
            }
            if (c1 == '=') MAKE_OP("<=", 2);
            MAKE_OP("<", 1);

        // > >= >> >>=
        case '>':
            if (c1 == '>') {
                if (lexer.getchar(2) == '=') MAKE_OP(">>=", 3);
                MAKE_OP(">>", 2);
            }
            if (c1 == '=') MAKE_OP(">=", 2);
            MAKE_OP(">", 1);

        // = ==
        case '=':
            if (c1 == '=') MAKE_OP("==", 2);
            MAKE_OP("=", 1);

        // ! !=
        case '!':
            if (c1 == '=') MAKE_OP("!=", 2);
            MAKE_OP("!", 1);

        // & && &=
        case '&':
            if (c1 == '&') MAKE_OP("&&", 2);
            if (c1 == '=') MAKE_OP("&=", 2);
            MAKE_OP("&", 1);

        // | || |=
        case '|':
            if (c1 == '|') MAKE_OP("||", 2);
            if (c1 == '=') MAKE_OP("|=", 2);
            MAKE_OP("|", 1);

        // ^ ^=
        case '^':
            if (c1 == '=') MAKE_OP("^=", 2);
            MAKE_OP("^", 1);

        // ~ (tek karakter)
        case '~': MAKE_OP("~", 1);

        // : ::
        case ':':
            if (c1 == ':') MAKE_DEL("::", 2);
            MAKE_DEL(":", 1);

        // Tek karakterli delimiter'lar
        case '[': MAKE_DEL("[", 1);
        case ']': MAKE_DEL("]", 1);
        case '(': MAKE_DEL("(", 1);
        case ')': MAKE_DEL(")", 1);
        case '{': MAKE_DEL("{", 1);
        case '}': MAKE_DEL("}", 1);
        case ';': MAKE_DEL(";", 1);
        case ',': MAKE_DEL(",", 1);
        case '.': MAKE_DEL(".", 1);
        case '?': MAKE_OP("?",  1);

        default: break;
    }

    // ── Identifier veya Keyword — önce oku, sonra hash map'te ara ────────
    IdentifierToken* id = readIdentifier();

    if (KEYWORD_MAP.count(id->token)) {
        KeywordToken* kt = new KeywordToken();
        kt->start = id->start;
        kt->end   = id->end;
        kt->loc   = id->loc;
        kt->token = id->token;
        delete id;
        return kt;
    }

    return id;
}

// ─────────────────────────────────────────────────────────────────────────────
// readIdentifier — ASCII harf, rakam, '_' ve '$' dizisini okur. Hiç karakter
// okunamazsa (tanınmayan karakter) bir karakter atlayıp boş ad döndürür.
// ─────────────────────────────────────────────────────────────────────────────
IdentifierToken* Tokenizer::readIdentifier() {
    lexer.beginPosition();
    IdentifierToken* it = new IdentifierToken();
    it->start = lexer.getOffset();

    while (!lexer.isEnd()) {
        char c = lexer.getchar();
        bool read = false;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            read = true;
            it->token.push_back(c);
        } else if (c == '_' || c == '$') {
            read = true;
            it->token.push_back(c);
        }

        if (read) {
            lexer.nextChar();
        } else {
            if (it->token.empty()) { lexer.nextChar(); } break;
        }
    }

    it->end  = lexer.getOffset();
    it->size = static_cast<int>(it->context.size());
    it->loc  = lexer.sourceFile.offsetToLocation(it->start);
    lexer.acceptPosition();
    return it;
}

// ─────────────────────────────────────────────────────────────────────────────
// readString — "..." literalini okur; kaçışları çözülmüş metni context'e yazar.
// Sözcüksel hatalar: E907 (kapanış '"' yok), E906 (bilinmeyen kaçış, #256).
// ─────────────────────────────────────────────────────────────────────────────
StringToken* Tokenizer::readString() {
    lexer.beginPosition();
    StringToken* st = new StringToken();
    bool started = false;
    bool ended   = false;
    std::string badEscapes;   // tanınmayan kaçışların harfleri (`\q` → 'q')
    st->start = lexer.getOffset();

    while (!lexer.isEnd()) {
        char c = lexer.getchar();
        st->token.push_back(c);
        switch (c) {
            case '"':
                if (!started) { started = true; }
                else          { ended   = true; }
                break;
            case '\\': {
                lexer.nextChar();
                c = lexer.getchar();
                st->token.push_back(c);
                // Kaçış dizisini gerçek kontrol/karakter değerine çevir
                // (wiki/literals.md sözleşmesi: \n \t \r \b \\ \").
                char actual = c;
                switch (c) {
                    case 'n': actual = '\n'; break;
                    case 't': actual = '\t'; break;
                    case 'r': actual = '\r'; break;
                    case 'b': actual = '\b'; break;
                    case '\\':
                    case '"': actual = c;    break;
                    default:
                        // #256: tanınmayan kaçış eskiden sessizce harfe
                        // dönüşüyordu (`"\x41"` → `x41`); E906 raporlanır.
                        badEscapes.push_back(c);
                        actual = c;
                        break;
                }
                st->context.push_back(actual);
                break;
            }
            default:
                st->context.push_back(c);
                break;
        }
        lexer.nextChar();
        if (ended) break;
    }

    st->end  = lexer.getOffset();
    st->size = static_cast<int>(st->context.size());
    st->loc  = lexer.sourceFile.offsetToLocation(st->start);
    lexer.acceptPosition();

    if (!ended)
        report(st->loc, "E907", "unterminated string literal (missing closing '\"')");
    for (char e : badEscapes)
        report(st->loc, "E906",
               std::string("unknown escape sequence '\\") + e +
                   "' in string literal (supported: \\n \\t \\r \\b \\\\ \\\")");
    return st;
}

// ─────────────────────────────────────────────────────────────────────────────
// skipOneLineComment / skipMultiLineComment — değişmedi
// ─────────────────────────────────────────────────────────────────────────────
void Tokenizer::skipOneLineComment() {
    while (!lexer.isEnd()) {
        if (lexer.getchar() == '\n') {
            lexer.nextChar();
            lexer.skipWhiteSpace();
            return;
        }
        lexer.nextChar();
    }
}

void Tokenizer::skipMultiLineComment() {
    while (!lexer.isEnd()) {
        if (lexer.tryConsume("*/")) {
            lexer.skipWhiteSpace();
            return;
        }
        lexer.nextChar();
    }
}
