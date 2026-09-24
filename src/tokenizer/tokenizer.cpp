// ============================================================================
// saQut Compiler — Sözcüksel Analiz (Tokenizer) Gerçeklemesi
// ============================================================================
//
// DİZİN:   src/tokenizer/tokenizer.cpp
// KATMAN:  Katman 2 — Lexer yardımıyla kaynak kodu token'lara ayırır
// BAĞIMLI: tokenizer/tokenizer.hpp
//
// AMAÇ:
//   Lexer aracılığıyla karakterleri okuyarak anlamlı token'lar üretir:
//   sayı, string literal, operatör, delimiter, keyword, identifier.
//   scope() ana dispatch metodudur; scan() döngüde scope() çağırarak
//   token listesini oluşturur.
//
// ============================================================================

#include "tokenizer/tokenizer.hpp"
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
// Keyword hash map — O(1) lookup yerine O(n) for döngüsü
// ─────────────────────────────────────────────────────────────────────────────
static const std::unordered_map<std::string_view, std::string_view> KW_MAP = {
    {"if","if"},{"else","else"},{"for","for"},{"while","while"},{"do","do"},
    {"as","as"},
    {"switch","switch"},{"case","case"},{"default","default"},
    {"break","break"},{"continue","continue"},{"return","return"},
    {"try","try"},{"catch","catch"},{"finally","finally"},
    {"throw","throw"},{"throws","throws"},{"assert","assert"},
    {"void","void"},{"int","int"},{"float","float"},{"double","double"},
    {"char","char"},{"string","string"},{"bool","bool"},{"decimal","decimal"},
    {"byte","byte"},
    {"true","true"},{"false","false"},{"null","null"},
    {"class","class"},{"struct","struct"},{"interface","interface"},
    {"enum","enum"},{"extends","extends"},{"implements","implements"},
    {"new","new"},{"public","public"},{"private","private"},
    {"protected","protected"},{"static","static"},{"final","final"},
    {"abstract","abstract"},{"import","import"},{"export","export"},{"package","package"},
    {"const","const"},{"extern","extern"},{"ffi","ffi"},{"typedef","typedef"},
    {"sizeof","sizeof"},{"auto","auto"},{"constexpr","constexpr"},
    {"noexcept","noexcept"},{"native","native"},
    {"synchronized","synchronized"},{"volatile","volatile"},
    {"transient","transient"},
    // ADR-045 Faz 3: izole thread modeli
    {"shared","shared"},{"lock","lock"},{"unlock","unlock"},{"wait","wait"},
    {"thread","thread"},{"Pool","Pool"},{"List","List"},{"Thread","Thread"}
};

// ─────────────────────────────────────────────────────────────────────────────
// Yardımcı makrolar — OperatorToken ve DelimiterToken üretimi
// ─────────────────────────────────────────────────────────────────────────────
#define MAKE_OP(str, len)                      \
    do {                                        \
        OperatorToken* _t = new OperatorToken();\
        _t->start = hmx.getOffset();            \
        _t->loc   = hmx.getLocation();          \
        hmx.toChar(len);                        \
        _t->end   = hmx.getOffset();            \
        _t->token = (str);                      \
        return _t;                              \
    } while(0)

#define MAKE_DEL(str, len)                       \
    do {                                          \
        DelimiterToken* _t = new DelimiterToken();\
        _t->start = hmx.getOffset();              \
        _t->loc   = hmx.getLocation();            \
        hmx.toChar(len);                          \
        _t->end   = hmx.getOffset();              \
        _t->token = (str);                        \
        return _t;                                \
    } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// scan
// ─────────────────────────────────────────────────────────────────────────────
std::vector<Token*> Tokenizer::scan(std::string input, std::string filePath) {
    std::vector<Token*> tokens;
    hmx.setSourceText(filePath, input);
    while (true) {
        Token* token = scope();
        if (token->token == "EOL") break;
        tokens.push_back(token);
        if (hmx.isEnd()) break;
    }
    return tokens;
}

// ─────────────────────────────────────────────────────────────────────────────
// scope — ana dispatch; her token için TEK geçiş
// ─────────────────────────────────────────────────────────────────────────────
Token* Tokenizer::scope() {
    hmx.skipWhiteSpace();

    // Yorum satırları — include() burada hâlâ gerekli (2 karakter kontrol)
    if (hmx.include("//", true))  { skipOneLineComment();  return scope(); }
    if (hmx.include("/*", true))  { skipMultiLineComment(); return scope(); }

    if (hmx.isEnd()) {
        Token* t = new Token();
        t->token = "EOL";
        return t;
    }

    if (hmx.getchar() == '"') return readString();
    if (hmx.isNumeric())      {
        INumber lem = hmx.readNumeric();
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

    char c0 = hmx.getchar();
    char c1 = hmx.getchar(1);  // sadece 1 ek okuma, include() değil

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
                if (hmx.getchar(2) == '=') MAKE_OP("<<=", 3);
                MAKE_OP("<<", 2);
            }
            if (c1 == '=') MAKE_OP("<=", 2);
            MAKE_OP("<", 1);

        // > >= >> >>=
        case '>':
            if (c1 == '>') {
                if (hmx.getchar(2) == '=') MAKE_OP(">>=", 3);
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

    auto it = KW_MAP.find(id->token);
    if (it != KW_MAP.end()) {
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
// readIdentifier — değişmedi
// ─────────────────────────────────────────────────────────────────────────────
IdentifierToken* Tokenizer::readIdentifier() {
    hmx.beginPosition();
    IdentifierToken* it = new IdentifierToken();
    it->start = hmx.getOffset();

    while (!hmx.isEnd()) {
        char c = hmx.getchar();
        bool read = false;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            read = true;
            it->token.push_back(c);
        } else if (c == '_' || c == '$') {
            read = true;
            it->token.push_back(c);
        }

        if (read) {
            hmx.nextChar();
        } else {
            if (it->token.empty()) { hmx.nextChar(); } break;
        }
    }

    it->end  = hmx.getOffset();
    it->size = static_cast<int>(it->context.size());
    it->loc  = hmx.sourceFile.offsetToLocation(it->start);
    hmx.acceptPosition();
    return it;
}

// ─────────────────────────────────────────────────────────────────────────────
// readString — değişmedi
// ─────────────────────────────────────────────────────────────────────────────
StringToken* Tokenizer::readString() {
    hmx.beginPosition();
    StringToken* st = new StringToken();
    bool started = false;
    bool ended   = false;
    st->start = hmx.getOffset();

    while (!hmx.isEnd()) {
        char c = hmx.getchar();
        st->token.push_back(c);
        switch (c) {
            case '"':
                if (!started) { started = true; }
                else          { ended   = true; }
                break;
            case '\\': {
                hmx.nextChar();
                c = hmx.getchar();
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
                        // dönüşüyordu (`"\x41"` → `x41`). Artık işaretlenir;
                        // parser E906 raporlar.
                        st->badEscapes.push_back(c);
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
        hmx.nextChar();
        if (ended) break;
    }

    st->unterminated = !ended;
    st->end  = hmx.getOffset();
    st->size = static_cast<int>(st->context.size());
    st->loc  = hmx.sourceFile.offsetToLocation(st->start);
    hmx.acceptPosition();
    return st;
}

// ─────────────────────────────────────────────────────────────────────────────
// skipOneLineComment / skipMultiLineComment — değişmedi
// ─────────────────────────────────────────────────────────────────────────────
void Tokenizer::skipOneLineComment() {
    while (!hmx.isEnd()) {
        if (hmx.getchar() == '\n') {
            hmx.nextChar();
            hmx.skipWhiteSpace();
            return;
        }
        hmx.nextChar();
    }
}

void Tokenizer::skipMultiLineComment() {
    while (!hmx.isEnd()) {
        if (hmx.include("*/", true)) {
            hmx.skipWhiteSpace();
            return;
        }
        hmx.nextChar();
    }
}
