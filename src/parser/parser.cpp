// ============================================================================
// saQut Compiler — Pratt Parser Gerçeklemesi
// ============================================================================
//
// DİZİN:   src/parser/parser.cpp
// KATMAN:  Katman 3 — Token listesini AST'ye dönüştürür
// BAĞIMLI: parser/parser.hpp, parser/nodes/*.hpp
//
// AMAÇ:
//   Parser sınıfının tüm metodlarının gövdeleri. Pratt parsing (Top-Down
//   Operator Precedence) algoritması ile token'ları AST düğümlerine çevirir.
//   5 ana bölüm: token navigasyonu, bildirim ayrıştırma, ifade/deyim
//   ayrıştırma, Pratt ifade ayrıştırma, hata raporlama/kurtarma.
//
// ============================================================================

#include "parser/parser.hpp"

#include "parser/nodes/binary_expr.hpp"
#include "parser/nodes/declarations.hpp"
#include "parser/nodes/error_node.hpp"
#include "parser/nodes/expressions.hpp"
#include "parser/nodes/identifier.hpp"
#include "parser/nodes/literal.hpp"
#include "parser/nodes/program.hpp"
#include "parser/nodes/statements.hpp"

// --------------------------------------------------------------------------
// parseToken: Ham Token'ı ParserToken'a dönüştür.
// --------------------------------------------------------------------------
ParserToken Parser::parseToken(Token* token) {
    ParserToken pt;
    pt.token = token;

    std::string t = token->gettype();
    if (t == "string")
        pt.type = TokenType::STRING;
    else if (t == "number")
        pt.type = TokenType::NUMBER;
    else if (t == "operator")
        pt.type = OPERATOR_MAP.find(pt.token->token)->second;
    else if (t == "delimiter")
        pt.type = OPERATOR_MAP.find(pt.token->token)->second;
    else if (t == "keyword")
        pt.type = KEYWORD_MAP.find(pt.token->token)->second;
    else if (t == "identifier")
        pt.type = TokenType::IDENTIFIER;

    return pt;
}

ParserToken Parser::getToken(int offset) {
    const int idx = current + offset;
    if ((int) tokens.size() - 1 < idx) {
        ParserToken pt;
        pt.type = TokenType::SVR_VOID;
        return pt;
    }

    // parseToken() saf bir fonksiyondur: aynı Token* için hep aynı ParserToken'ı
    // üretir. Ama currentToken()/lookahead() aynı indeksi tekrar tekrar sorar —
    // 2.3 MB'lık girdide 1.06 milyon token için 6.17 milyon parseToken çağrısı
    // ölçüldü (token başına 5.8 kat). Her çağrı bir std::string kopyası, zincirleme
    // string karşılaştırması ve bir hash araması demekti. Sonuç indeks başına bir
    // kez hesaplanıp saklanır; tokens listesi parse() süresince sabittir.
    if (tokenCache_.size() != tokens.size())
        tokenCache_.assign(tokens.size(), ParserToken{});

    ParserToken& slot = tokenCache_[idx];
    if (slot.token == nullptr)
        slot = parseToken(tokens[idx]);
    return slot;
}

void Parser::nextToken() {
    if (currentToken().token)
        lastLoc_ = currentToken().token->loc;
    if ((int) tokens.size() >= current + 1)
        current++;
}

ParserToken Parser::lookahead(uint32_t forward) {
    return getToken(forward);
}

ParserToken Parser::currentToken() {
    return getToken(0);
}

ASTNode* Parser::parse(TokenList toks) {
    tokens = toks;
    current = 0;
    // Aynı Parser örneği ikinci kez parse() edilirse eski önbellek başka bir
    // token listesine ait olur; boyut kontrolü getToken() içinde de var ama
    // aynı boyutlu farklı liste durumuna karşı burada da temizlenir.
    tokenCache_.clear();
    return parseProgram();
}

// ─────────────────────────────────────────────────────────────────────────────
// Faz 2 — sözdizimi hata raporlama + panic-mode kurtarma
// ─────────────────────────────────────────────────────────────────────────────

void Parser::reportError(const SourceLocation& loc, const std::string& code,
                         const std::string& message) {
    if (diag_) {
        diag_->report(code, loc, message);
    } else {
        std::cerr << "parser error: " << message << "\n";
    }
}

// Bir statement'ın başlayabileceği token mı? panic-mode recovery bu token'lara
// kadar atlar ama onları TÜKETMEZ — bir sonraki parseStatement() çağrısı
// normal şekilde devam edebilsin diye.
static bool isStatementStartToken(TokenType t) {
    switch (t) {
    case TokenType::KW_IF:
    case TokenType::KW_WHILE:
    case TokenType::KW_FOR:
    case TokenType::KW_DO:
    case TokenType::KW_RETURN:
    case TokenType::KW_BREAK:
    case TokenType::KW_CONTINUE:
    case TokenType::KW_TRY:
    case TokenType::KW_THROW:
    case TokenType::KW_SWITCH:
    case TokenType::KW_STRUCT:
    case TokenType::KW_ENUM:
    case TokenType::KW_IMPORT:
    case TokenType::KW_EXPORT:
    case TokenType::KW_VOID:
    case TokenType::KW_INT:
    case TokenType::KW_FLOAT_TYPE:
    case TokenType::KW_DOUBLE:
    case TokenType::KW_DECIMAL:
    case TokenType::KW_BYTE:
    case TokenType::KW_DATE:
    case TokenType::KW_BOOL:
    case TokenType::KW_CHAR:
    case TokenType::KW_STRING_TYPE:
    case TokenType::KW_AUTO:
    // ADR-045
    case TokenType::KW_SHARED:
    case TokenType::KW_LOCK:
    case TokenType::KW_UNLOCK:
    case TokenType::KW_WAIT:
    case TokenType::KW_POOL:
    case TokenType::KW_LIST:
    case TokenType::KW_THREAD_TYPE:
        return true;
    default:
        return false;
    }
}

ASTNode* Parser::synchronizeAndMakeError(const SourceLocation& loc, const std::string& code,
                                         const std::string& message) {
    reportError(loc, code, message);

    // İlerleme garantisi: en az bir token tüket (aksi halde çağıran döngüde
    // sonsuz döngü riski olurdu — bkz. parseProgram'daki eski "prevPos" koruması).
    if (currentToken().type != TokenType::SVR_VOID)
        nextToken();

    while (currentToken().type != TokenType::SEMICOLON &&
           currentToken().type != TokenType::RBRACE && currentToken().type != TokenType::SVR_VOID &&
           !isStatementStartToken(currentToken().type)) {
        nextToken();
    }
    if (currentToken().type == TokenType::SEMICOLON)
        nextToken(); // sınırlayıcı ';' tüketilir; '}' ve statement-başlangıcı tüketilmez

    ErrorNode* err = new ErrorNode();
    err->loc = loc;
    err->code = code;
    err->message = message;
    return err;
}

void Parser::expectSemicolon(const char* after) {
    if (currentToken().type == TokenType::SEMICOLON) {
        nextToken();
        return;
    }
    reportError(lastLoc_, "E905", std::string("expected ';' after ") + after);
}

ASTNode* Parser::expectExpression(const std::string& context, uint16_t precedence) {
    ASTNode* expr = parseExpression(precedence);
    if (!expr) {
        auto ct = currentToken();
        std::string tokText = ct.token ? "'" + ct.token->token + "'" : "end of file";
        reportError(ct.token ? ct.token->loc : lastLoc_, "E901",
                    "expected an expression " + context + ", found " + tokText);
    }
    return expr;
}

ASTNode* Parser::parseProgram() {
    ProgramNode* program = new ProgramNode();

    while (currentToken().type != TokenType::SVR_VOID) {
        int prevPos = current;
        ASTNode* decl = parseDeclaration();
        if (decl)
            program->addChild(decl);
        // İlerleme olmadıysa token atla — syntax hatasında sonsuz döngüyü önler
        if (current == prevPos)
            nextToken();
    }

    return program;
}

ASTNode* Parser::parseImportDecl() {
    auto* node = new ImportDeclNode();
    node->loc = currentToken().token->loc;
    nextToken(); // 'import' tüket

    // { bekleniyor
    if (currentToken().type != TokenType::LBRACE)
        return node;
    nextToken();

    // virgülle ayrılmış isimler: { add, Vector as Vec, ... }
    // İsteğe bağlı `as <yerelAd>`: `import {exists as fileExists} from fs`.
    // İlerleme garantisi: IDENTIFIER/COMMA olmayan bir token sonsuz döngüye
    // yol açardı (bkz. #170 — bu, 25 kapanış-delimiter site'inden AYRI,
    // bağımsız bulunmuş bir hang/DoS hatasıydı) — beklenmeyen token'da hata
    // basıp döngüden çık.
    while (!currentToken().is({TokenType::RBRACE, TokenType::SVR_VOID})) {
        if (currentToken().type == TokenType::IDENTIFIER) {
            ImportDeclNode::ImportName en;
            en.source = currentToken().token->token;
            nextToken();
            // `as <local>` — bağlamca güvenli: bu noktada zaten IDENTIFIER
            // tükettik ve virgül/} bekliyoruz; `as` cast değil, import takma
            // adı (aşırı yüklü keyword, TokenType::KW_AS).
            if (currentToken().type == TokenType::KW_AS) {
                nextToken();
                if (currentToken().type == TokenType::IDENTIFIER) {
                    en.local = currentToken().token->token;
                    nextToken();
                } else {
                    reportError(currentToken().token ? currentToken().token->loc : node->loc,
                                "E905", "expected local name after 'as' in import");
                }
            }
            node->importedNames.push_back(std::move(en));
        } else {
            reportError(currentToken().token ? currentToken().token->loc : node->loc,
                        "E905", "expected identifier or '}' in import list");
            break;
        }
        if (currentToken().type == TokenType::COMMA)
            nextToken();
    }

    // } bekleniyor
    if (currentToken().type == TokenType::RBRACE)
        nextToken();

    // contextual keyword: from
    if (currentToken().type == TokenType::IDENTIFIER && currentToken().token->token == "from") {
        nextToken();
    }

    // ADR-034 (#107): tırnaklı = dosya yolu; tırnaksız identifier = gömülü/
    // çözümlenen modül adı.
    if (currentToken().type == TokenType::STRING) {
        auto* st = static_cast<StringToken*>(currentToken().token);
        node->sourcePath = st->context;
        node->isModuleName = false; // dosya
        nextToken();
    } else if (currentToken().type == TokenType::IDENTIFIER && currentToken().token) {
        node->sourcePath = currentToken().token->token;
        node->isModuleName = true; // gömülü/çözümlenen modül
        nextToken();
    }

    // ;
    if (currentToken().type == TokenType::SEMICOLON)
        nextToken();

    return node;
}

ASTNode* Parser::parseExportDecl() {
    nextToken(); // 'export' tüket

    auto ct = currentToken();

    if (ct.type == TokenType::KW_STRUCT) {
        auto* node = static_cast<StructDeclNode*>(parseStructDecl());
        if (node)
            node->isExported = true;
        return node;
    }

    if (ct.type == TokenType::KW_ENUM) {
        auto* node = static_cast<EnumDeclNode*>(parseEnumDecl());
        if (node)
            node->isExported = true;
        return node;
    }

    // Dönüş tipli fonksiyon: export void/int/... name( ...
    // veya struct dönüş tipli: export TypeName name(
    bool isFunctionReturnType =
        ct.is({TokenType::KW_VOID, TokenType::KW_INT, TokenType::KW_FLOAT_TYPE,
               TokenType::KW_DOUBLE, TokenType::KW_DECIMAL, TokenType::KW_BYTE, TokenType::KW_DATE,
               TokenType::KW_BOOL, TokenType::KW_CHAR, TokenType::KW_STRING_TYPE});

    bool isIdentifierReturnType = (ct.type == TokenType::IDENTIFIER);

    if (isFunctionReturnType || isIdentifierReturnType) {
        auto la1 = lookahead(1);
        auto la2 = lookahead(2);
        bool isNullable = (la1.type == TokenType::TERNARY);
        bool isFnDecl =
            isNullable ?
                (la2.type == TokenType::IDENTIFIER && lookahead(3).type == TokenType::LPAREN) :
                (la1.type == TokenType::IDENTIFIER && la2.type == TokenType::LPAREN);

        if (!isFnDecl && la1.type == TokenType::LBRACKET) {
            int off = 1;
            while (lookahead(off).type == TokenType::LBRACKET &&
                   lookahead(off + 1).type == TokenType::RBRACKET)
                off += 2;
            if (lookahead(off).type == TokenType::IDENTIFIER &&
                lookahead(off + 1).type == TokenType::LPAREN)
                isFnDecl = true;
            else if (lookahead(off).type == TokenType::TERNARY &&
                     lookahead(off + 1).type == TokenType::IDENTIFIER &&
                     lookahead(off + 2).type == TokenType::LPAREN)
                isFnDecl = true;
        }

        if (isFnDecl) {
            auto* node = static_cast<FunctionDeclNode*>(parseFunctionDecl());
            if (node)
                node->isExported = true;
            return node;
        }
    }

    // Buraya gelindiyse global değişken bildirimi: `export int X = ...;` (#3).
    // Fonksiyon/struct/enum ile aynı şekilde isExported bayrağını taşı —
    // `int a, b;` çoklu bildirimindeki tüm kardeşler de dahil.
    ASTNode* decl = parseDeclaration();
    if (auto* vd = dynamic_cast<VariableDeclNode*>(decl)) {
        vd->isExported = true;
        for (ASTNode* sibling : vd->getChildren()) {
            if (auto* svd = dynamic_cast<VariableDeclNode*>(sibling))
                svd->isExported = true;
        }
    }
    return decl;
}

ASTNode* Parser::parseDeclaration() {
    auto ct = currentToken();

    if (ct.type == TokenType::KW_IMPORT)
        return parseImportDecl();

    if (ct.type == TokenType::KW_EXPORT)
        return parseExportDecl();

    if (ct.type == TokenType::KW_FFI)
        return parseFfiDecl();

    // ADR-045: shared <tip> <ad> = ...;
    if (ct.type == TokenType::KW_SHARED)
        return parseSharedDecl();

    if (ct.is({TokenType::KW_VOID, TokenType::KW_INT, TokenType::KW_FLOAT_TYPE,
               TokenType::KW_DOUBLE, TokenType::KW_DECIMAL, TokenType::KW_BYTE, TokenType::KW_DATE,
               TokenType::KW_BOOL, TokenType::KW_CHAR, TokenType::KW_STRING_TYPE,
               TokenType::KW_AUTO,
               // ADR-045: Pool/List/Thread tip adları
               TokenType::KW_POOL, TokenType::KW_LIST, TokenType::KW_THREAD_TYPE})) {
        auto la1 = lookahead(1);
        auto la2 = lookahead(2);
        // int name(  → fonksiyon
        if (la1.type == TokenType::IDENTIFIER && la2.type == TokenType::LPAREN)
            return parseFunctionDecl();
        // int? name(  → nullable dönüş tipli fonksiyon (ADR-021)
        if (la1.type == TokenType::TERNARY) {
            auto la3 = lookahead(3);
            if (la2.type == TokenType::IDENTIFIER && la3.type == TokenType::LPAREN)
                return parseFunctionDecl();
        }
        if (la1.type == TokenType::LBRACKET) {
            int off = 1;
            while (lookahead(off).type == TokenType::LBRACKET &&
                   lookahead(off + 1).type == TokenType::RBRACKET)
                off += 2;
            if (lookahead(off).type == TokenType::IDENTIFIER &&
                lookahead(off + 1).type == TokenType::LPAREN)
                return parseFunctionDecl();
            if (lookahead(off).type == TokenType::TERNARY &&
                lookahead(off + 1).type == TokenType::IDENTIFIER &&
                lookahead(off + 2).type == TokenType::LPAREN)
                return parseFunctionDecl();
        }
        return parseVariableDecl();
    }

    if (ct.type == TokenType::KW_STRUCT)
        return parseStructDecl();

    // enum top-level bildirimdir; E013 son-dalına (statement) düşmemeli.
    // Not: enum daha önce yalnızca parseStatement'ten parse ediliyordu —
    // bu da declaration/statement ayrımını bozuyordu.
    if (ct.type == TokenType::KW_ENUM)
        return parseEnumDecl();

    // Kullanıcı tanımlı tip adı (struct tipi) ile değişken/fonksiyon bildirimi
    if (ct.type == TokenType::IDENTIFIER) {
        auto la1 = lookahead(1);
        auto la2 = lookahead(2);
        if (la1.type == TokenType::IDENTIFIER && la2.type == TokenType::LPAREN)
            return parseFunctionDecl();
        if (la1.type == TokenType::TERNARY) {
            auto la3 = lookahead(3);
            if (la2.type == TokenType::IDENTIFIER && la3.type == TokenType::LPAREN)
                return parseFunctionDecl();
            // `Inner? name;` — nullable struct-tipli değişken/alan (ADR-021).
            // la3 LPAREN değilse bu bir bildirimdir; son-dala (E013) düşmesin.
            if (la2.type == TokenType::IDENTIFIER)
                return parseVariableDecl();
        }
        if (la1.type == TokenType::IDENTIFIER)
            return parseVariableDecl();
        // "TypeName[]...[] varName" — çok boyutlu struct/enum array bildirimi
        if (la1.type == TokenType::LBRACKET) {
            int off = 1;
            while (lookahead(off).type == TokenType::LBRACKET &&
                   lookahead(off + 1).type == TokenType::RBRACKET)
                off += 2;
            if (lookahead(off).type == TokenType::IDENTIFIER &&
                lookahead(off + 1).type == TokenType::LPAREN)
                return parseFunctionDecl();
            if (lookahead(off).type == TokenType::TERNARY &&
                lookahead(off + 1).type == TokenType::IDENTIFIER &&
                lookahead(off + 2).type == TokenType::LPAREN)
                return parseFunctionDecl();
            if (lookahead(off).type == TokenType::IDENTIFIER)
                return parseVariableDecl();
        }
    }

    // Top-level'da statement yasak (E013, "globalde hesaplama yapılmaz" —
    // C++ ile aynı: modül kapsamında yalnız bildirimler olabilir). IR generator
    // bu statement'ları sessizce atlıyordu (a=5; a+=1; a.push(...); fn(); hepsi
    // no-op — kullanıcı "global değişken kullanılmaz" diye telafi ediyordu).
    // Şimdi açık hata: parse edip discard et (döngü ilerlesin), program'a EKLEME.
    {
        auto loc = currentToken().token ? currentToken().token->loc : SourceLocation{};
        reportError(loc, "E013", "statements are not allowed at module scope");
        ASTNode* discarded = parseStatement();
        (void)discarded;  // bilinçli: hata zaten raporlandı, node atılıyor
        delete discarded;
        return nullptr;
    }
}

ASTNode* Parser::parseExpression() {
    return parseExpression(0);
}

ASTNode* Parser::parseExpression(uint16_t precedence) {
    if (currentToken().type == TokenType::SVR_VOID)
        return nullptr;

    ASTNode* left = parseNullDenotation();
    if (!left)
        return nullptr;

    while (true) {
        auto next = currentToken();
        if (next.type == TokenType::RPAREN || next.type == TokenType::SEMICOLON ||
            next.type == TokenType::RBRACE || next.type == TokenType::COMMA)
            break;

        if (precedence < next.getPowerOperator()) {
            left = parseLeftDenotation(left);
        } else {
            break;
        }
    }
    return left;
}

// E::method(args) kalıbını ayrıştır: hem type-keyword hem IDENTIFIER başlangıçları için.
// Koşul: (type_kw | IDENTIFIER) COLON_COLON IDENTIFIER LPAREN
// ADR-033 (#85): `struct::toJson(p)` ad alanı çağrısı için KW_STRUCT da sol
// tarafta geçerli ("array" zaten IDENTIFIER olarak eşleşir).
static bool isScopeCallPattern(const ParserToken& ct, const ParserToken& la1,
                               const ParserToken& la2, const ParserToken& la3) {
    bool leftIsType =
        ct.is({TokenType::KW_INT, TokenType::KW_FLOAT_TYPE, TokenType::KW_DOUBLE,
               TokenType::KW_DECIMAL, TokenType::KW_BYTE, TokenType::KW_DATE, TokenType::KW_BOOL,
               TokenType::KW_CHAR, TokenType::KW_STRING_TYPE, TokenType::KW_STRUCT}) ||
        ct.type == TokenType::IDENTIFIER;
    return leftIsType && la1.type == TokenType::COLON_COLON && la2.type == TokenType::IDENTIFIER &&
           la3.type == TokenType::LPAREN;
}

ASTNode* Parser::parseNullDenotation() {
    auto ct = currentToken();

    // SVR_VOID (EOF) burada özel olarak ele alınmaz: aşağıdaki hiçbir kalıpla
    // eşleşmez ve fonksiyon sonundaki genel `return nullptr;`e düşer — tıpkı
    // tanınmayan herhangi bir token gibi. Buradan yükselen nullptr, çağıran
    // (genelde parseExpressionStatement) tarafından TEK bir konumlu tanıya
    // (E901) ve panic-mode kurtarmaya çevrilir; burada ikinci bir mesaj
    // basılırsa aynı hata için çift tanı üretilirdi (Faz 2).

    // ── ADR-045: Pool(T) / List(T) — argüman TİP adıdır ────────────────────
    if ((ct.type == TokenType::KW_POOL || ct.type == TokenType::KW_LIST) &&
        lookahead(1).type == TokenType::LPAREN) {
        auto* cn   = new CollectionNewNode();
        cn->loc    = ct.token ? ct.token->loc : SourceLocation{};
        cn->isPool = ct.type == TokenType::KW_POOL;
        nextToken(); // Pool / List
        nextToken(); // (
        cn->elemTypeName = parseTypeName();
        if (currentToken().type == TokenType::RPAREN)
            nextToken();
        else
            reportError(currentToken().token ? currentToken().token->loc : cn->loc, "E905",
                        std::string("expected ')' after ") + (cn->isPool ? "Pool" : "List") +
                            " element type");
        return cn;
    }

    // ── ADR-045: thread { gövde } ──────────────────────────────────────────
    if (ct.type == TokenType::KW_THREAD) {
        auto* te = new ThreadExprNode();
        te->loc  = ct.token ? ct.token->loc : SourceLocation{};
        nextToken(); // thread
        if (currentToken().type != TokenType::LBRACE) {
            reportError(currentToken().token ? currentToken().token->loc : te->loc, "E905",
                        "expected '{' after 'thread'");
            return te;
        }
        te->body = parseBlock();
        if (te->body) te->body->parent = te;
        return te;
    }

    // ── E::method(args) — built-in scope-call ────────────────────────────────
    if (isScopeCallPattern(ct, lookahead(1), lookahead(2), lookahead(3))) {
        ScopeCallNode* sc = new ScopeCallNode();
        sc->loc = ct.token ? ct.token->loc : SourceLocation{};

        // sol tip adını oku
        sc->leftTypeName = ct.token ? ct.token->token : "";
        nextToken(); // tüket: type-keyword veya identifier

        nextToken(); // tüket: ::

        // metod adını oku
        auto methodTok = currentToken();
        sc->methodName = methodTok.token ? methodTok.token->token : "";
        nextToken(); // tüket: method identifier

        // argüman listesini oku: ( expr, expr, ... )
        if (currentToken().type == TokenType::LPAREN)
            nextToken(); // tüket: (
        if (currentToken().type != TokenType::RPAREN) {
            if (currentToken().type == TokenType::COMMA) {
                reportError(currentToken().token ? currentToken().token->loc : sc->loc,
                            "E904", "expected expression before ','");
                nextToken();
            } else {
                sc->arguments.push_back(expectExpression("in argument list"));
            }
            while (currentToken().type == TokenType::COMMA) {
                auto comma = currentToken();
                nextToken();
                if (currentToken().type == TokenType::COMMA || currentToken().type == TokenType::RPAREN) {
                    reportError(comma.token ? comma.token->loc : sc->loc,
                                "E904", "expected expression after ','");
                    continue;
                }
                sc->arguments.push_back(expectExpression("in argument list"));
            }
        }
        if (currentToken().type == TokenType::RPAREN)
            nextToken(); // tüket: )
        else
            reportError(currentToken().token ? currentToken().token->loc : sc->loc,
                        "E905", "expected ')' after scope-call arguments");
        return sc;
    }

    if (ct.type == TokenType::LPAREN) {
        nextToken();
        ASTNode* expr = expectExpression("inside '( )'");
        if (currentToken().type == TokenType::RPAREN)
            nextToken();
        else
            reportError(currentToken().token ? currentToken().token->loc : lastLoc_,
                        "E905", "expected ')' to close parenthesized expression");
        return expr;
    }

    // Array literal: [expr, expr, ...]
    if (ct.type == TokenType::LBRACKET) {
        nextToken();
        ArrayLiteralNode* arr = new ArrayLiteralNode();
        arr->loc = ct.token ? ct.token->loc : SourceLocation{};
        if (currentToken().type != TokenType::RBRACKET) {
            arr->elements.push_back(expectExpression("in array literal"));
            while (currentToken().type == TokenType::COMMA) {
                nextToken();
                // Sondaki virgül serbest: `[1, 2, 3,]` (çok satırlı literal).
                if (currentToken().type == TokenType::RBRACKET)
                    break;
                arr->elements.push_back(expectExpression("in array literal"));
            }
        }
        if (currentToken().type == TokenType::RBRACKET)
            nextToken();
        else
            reportError(currentToken().token ? currentToken().token->loc : arr->loc,
                        "E905", "expected ']' to close array literal");
        return arr;
    }

    // #237: Önek ++/-- ayrı bir düğümdür. Eskiden unary +/-/!/~ ile aynı
    // listedeydi ve Left=nullptr'lı BinaryExpression kuruyordu; düşürme
    // tarafında o biçimin karşılığı olmadığı için `++x` sessizce "x'in
    // değeri" ifadesine indirgeniyor, hiçbir yan etki üretmiyordu.
    if (ct.is({TokenType::PLUS_PLUS, TokenType::MINUS_MINUS})) {
        nextToken();
        ASTNode* right = expectExpression(
            "after prefix '" + std::string(ct.token ? ct.token->token : "?") + "'",
            ct.getPowerOperator());
        PostfixNode* pf = new PostfixNode();
        pf->loc = ct.token ? ct.token->loc : SourceLocation{};
        pf->operand = right;
        pf->Operator = ct.type;
        pf->isPrefix = true;
        if (right)
            right->parent = pf;
        return pf;
    }

    if (ct.is({TokenType::PLUS, TokenType::MINUS,
               TokenType::BANG, TokenType::TILDE})) {
        nextToken();
        ASTNode* right = expectExpression(
            "after unary '" + std::string(ct.token ? ct.token->token : "?") + "'",
            ct.getPowerOperator());
        BinaryExpressionNode* bin = new BinaryExpressionNode();
        bin->loc = ct.token ? ct.token->loc : SourceLocation{};
        bin->Right = right;
        bin->Left = nullptr;
        bin->Operator = ct.type;
        if (right)
            right->parent = bin;
        return bin;
    }

    if (ct.type == TokenType::NUMBER) {
        nextToken();
        LiteralNode* lit = new LiteralNode();
        lit->loc = ct.token ? ct.token->loc : SourceLocation{};
        lit->lexerToken = ct.token;
        lit->parserToken = ct;
        if (auto* nt = dynamic_cast<NumberToken*>(ct.token)) {
            lit->literalBase = nt->base;
            lit->isFloatValue = nt->isFloat;
            lit->literalType = nt->isFloat ? LiteralType::FLOAT : LiteralType::INTEGER;
        }
        return lit;
    }

    if (ct.type == TokenType::STRING) {
        nextToken();
        if (auto* st = dynamic_cast<StringToken*>(ct.token)) {
            if (st->unterminated)
                reportError(st->loc, "E907", "unterminated string literal (missing closing '\"')");
            for (char e : st->badEscapes)
                reportError(st->loc, "E906",
                            std::string("unknown escape sequence '\\") + e +
                                "' in string literal (supported: \\n \\t \\r \\b \\\\ \\\")");
        }
        LiteralNode* lit = new LiteralNode();
        lit->literalType = LiteralType::STRING;
        lit->loc = ct.token ? ct.token->loc : SourceLocation{};
        lit->lexerToken = ct.token;
        lit->parserToken = ct;
        return lit;
    }

    if (ct.is({TokenType::KW_TRUE, TokenType::KW_FALSE, TokenType::KW_NULL})) {
        nextToken();
        LiteralNode* lit = new LiteralNode();
        if (ct.is({TokenType::KW_TRUE, TokenType::KW_FALSE}))
            lit->literalType = LiteralType::BOOLEAN;
        else
            lit->literalType = LiteralType::BOŞ;
        lit->loc = ct.token ? ct.token->loc : SourceLocation{};
        lit->lexerToken = ct.token;
        lit->parserToken = ct;
        return lit;
    }

    if (ct.type == TokenType::IDENTIFIER) {
        nextToken();
        IdentifierNode* id = new IdentifierNode();
        id->loc = ct.token ? ct.token->loc : SourceLocation{};
        id->lexerToken = ct.token;
        id->parserToken = ct;
        return id;
    }

    return nullptr;
}

ASTNode* Parser::parseLeftDenotation(ASTNode* left) {
    auto ct = currentToken();

    if (ct.is({TokenType::PLUS_PLUS, TokenType::MINUS_MINUS})) {
        nextToken();
        PostfixNode* pf = new PostfixNode();
        pf->loc = ct.token ? ct.token->loc : SourceLocation{};
        pf->operand = left;
        pf->Operator = ct.type;
        left->parent = pf;
        return pf;
    }

    if (ct.type == TokenType::LPAREN) {
        nextToken();
        CallExpressionNode* call = new CallExpressionNode();
        call->loc = ct.token ? ct.token->loc : SourceLocation{};
        call->callee = left;
        left->parent = call;

        if (currentToken().type != TokenType::RPAREN) {
            if (currentToken().type == TokenType::COMMA) {
                reportError(currentToken().token ? currentToken().token->loc : call->loc,
                            "E904", "expected expression before ','");
                nextToken();
            } else {
                call->arguments.push_back(expectExpression("in argument list"));
            }
            while (currentToken().type == TokenType::COMMA) {
                auto comma = currentToken();
                nextToken();
                if (currentToken().type == TokenType::COMMA || currentToken().type == TokenType::RPAREN) {
                    reportError(comma.token ? comma.token->loc : call->loc,
                                "E904", "expected expression after ','");
                    continue;
                }
                call->arguments.push_back(expectExpression("in argument list"));
            }
        }
        if (currentToken().type == TokenType::RPAREN)
            nextToken();
        else
            reportError(currentToken().token ? currentToken().token->loc : call->loc,
                        "E905", "expected ')' after call arguments");
        return call;
    }

    if (ct.type == TokenType::LBRACKET) {
        nextToken();
        IndexExpressionNode* idx = new IndexExpressionNode();
        idx->loc = ct.token ? ct.token->loc : SourceLocation{};
        idx->object = left;
        left->parent = idx;
        idx->index = expectExpression("inside '[ ]'");
        if (currentToken().type == TokenType::RBRACKET)
            nextToken();
        else
            reportError(currentToken().token ? currentToken().token->loc : idx->loc,
                        "E905", "expected ']' to close index expression");
        return idx;
    }

    // ADR-026: as tip dönüşümü — expr as TargetType[?]
    if (ct.type == TokenType::KW_AS) {
        nextToken(); // tüket: as
        CastExpressionNode* cast = new CastExpressionNode();
        cast->loc = ct.token ? ct.token->loc : SourceLocation{};
        cast->operand = left;
        if (left)
            left->parent = cast;

        // Hedef tip adını oku: int / float / bool / string / IDENTIFIER
        auto typeTok = currentToken();
        if (typeTok.is({TokenType::KW_INT, TokenType::KW_FLOAT_TYPE, TokenType::KW_DOUBLE,
                        TokenType::KW_DECIMAL, TokenType::KW_BYTE, TokenType::KW_DATE,
                        TokenType::KW_BOOL, TokenType::KW_STRING_TYPE})) {
            // tip adını string olarak al
            cast->targetTypeName = typeTok.token ? typeTok.token->token : "";
            nextToken();
        } else if (typeTok.type == TokenType::IDENTIFIER && typeTok.token) {
            cast->targetTypeName = typeTok.token->token;
            nextToken();
        } else {
            reportError(cast->loc, "E902", "expected type name after 'as'");
            cast->targetTypeName = "int"; // error recovery
        }

        // Opsiyonel '?' — nullable hedef tip: as int?
        if (currentToken().type == TokenType::TERNARY) {
            cast->targetNullable = true;
            nextToken();
        }
        return cast;
    }

    if (ct.type == TokenType::DOT || ct.type == TokenType::ARROW) {
        bool arrow = (ct.type == TokenType::ARROW);
        nextToken();

        if (currentToken().type != TokenType::IDENTIFIER) {
            reportError(currentToken().token ? currentToken().token->loc : lastLoc_, "E903",
                        std::string("expected member name after '") + (arrow ? "->" : ".") + "'");
            return left;
        }

        std::string memberName = currentToken().token->token;
        nextToken();

        // ADR-033 (#85): UFCS nokta çağrısı — expr.method(args).
        // `a.f(b)` yalnızca `f(a, b)` şekeridir (OOP değil): receiver
        // arguments[0] olur, TypeChecker alan gölgelemesini ve builtin
        // kategorisini receiver TİPİNDEN çözer, IR eski `::` çağrısıyla
        // birebir aynı CALLHOST'a düşer.
        if (!arrow && currentToken().type == TokenType::LPAREN) {
            ScopeCallNode* sc = new ScopeCallNode();
            sc->loc = ct.token ? ct.token->loc : SourceLocation{};
            sc->methodName = memberName;
            sc->dotCall = true;
            sc->arguments.push_back(left);
            left->parent = sc;

            nextToken(); // tüket: (
            if (currentToken().type != TokenType::RPAREN) {
                if (currentToken().type == TokenType::COMMA) {
                    reportError(currentToken().token ? currentToken().token->loc : sc->loc,
                                "E904", "expected expression before ','");
                    nextToken();
                } else {
                    sc->arguments.push_back(expectExpression("in argument list"));
                }
                while (currentToken().type == TokenType::COMMA) {
                    auto comma = currentToken();
                    nextToken();
                    if (currentToken().type == TokenType::COMMA || currentToken().type == TokenType::RPAREN) {
                        reportError(comma.token ? comma.token->loc : sc->loc,
                                    "E904", "expected expression after ','");
                        continue;
                    }
                    sc->arguments.push_back(expectExpression("in argument list"));
                }
            }
            if (currentToken().type == TokenType::RPAREN)
                nextToken(); // tüket: )
            else
                reportError(currentToken().token ? currentToken().token->loc : sc->loc,
                            "E905", "expected ')' after call arguments");
            return sc;
        }

        MemberAccessNode* ma = new MemberAccessNode();
        ma->loc = ct.token ? ct.token->loc : SourceLocation{};
        ma->object = left;
        ma->member = memberName;
        ma->arrow = arrow;
        left->parent = ma;
        return ma;
    }

    uint16_t prec = ct.getPowerOperator();
    nextToken();

    // #237: sağ-birleşmeli operatörlerde sağ taraf AYNI seviyeyi de yutmalı.
    // parseExpression(prec) döngüsü `precedence < next` koşuluyla ilerler,
    // yani eşit seviyede durur → sola birleşir. Bir eksik seviyeyle
    // çağırmak aynı seviyedeki bir sonraki operatörü sağ tarafa bırakır:
    //
    //   2 ** 3 ** 2   sol-birleşmeli: (2 ** 3) ** 2 = 64    ← yanlıştı
    //                 sağ-birleşmeli: 2 ** (3 ** 2) = 512   ← doğru
    //
    // RightAssociative() token.hpp'de tanımlıydı ama HİÇ ÇAĞRILMIYORDU;
    // bu yüzden `**`, `=` ve birleşik atamaların hepsi sola birleşiyordu.
    if (prec > 0 && RightAssociative(ct.type))
        prec = static_cast<uint16_t>(prec - 1);

    std::string opContext =
        "after operator '" + std::string(ct.token ? ct.token->token : "?") + "'";
    // `a >>> 1` → `>>` + `>`: saQut'ta mantıksal kaydırma yok; sessizce
    // `(a >> ?) > 1` diye ayrışmak yerine kullanıcıya nedenini söyle.
    if (ct.type == TokenType::RSHIFT && currentToken().type == TokenType::GREATER)
        opContext += " (saQut has no '>>>' operator; '>>' is an arithmetic shift)";
    ASTNode* right = expectExpression(opContext, prec);

    BinaryExpressionNode* bin = new BinaryExpressionNode();
    bin->loc = ct.token ? ct.token->loc : SourceLocation{};
    bin->Left = left;
    bin->Right = right;
    bin->Operator = ct.type;
    if (left)
        left->parent = bin;
    if (right)
        right->parent = bin;
    return bin;
}

ASTNode* Parser::parseFunctionDecl() {
    FunctionDeclNode* fn = new FunctionDeclNode();
    fn->loc = currentToken().token->loc;
    fn->returnType = currentToken().token->token;
    nextToken();

    while (currentToken().type == TokenType::LBRACKET) {
        nextToken();
        if (currentToken().type == TokenType::RBRACKET)
            nextToken();
        fn->returnType += "[]";
    }

    // ADR-021: nullable dönüş tipi — int? f()
    if (currentToken().type == TokenType::TERNARY) {
        nextToken();
        fn->returnType += "?";
    }

    fn->name = currentToken().token->token;
    nextToken();

    if (currentToken().type == TokenType::LPAREN) {
        nextToken();
        while (currentToken().type != TokenType::RPAREN &&
               currentToken().type != TokenType::SVR_VOID) {
            auto typeTok = currentToken();
            bool isTypeKw =
                typeTok.is({TokenType::KW_VOID, TokenType::KW_INT, TokenType::KW_FLOAT_TYPE,
                            TokenType::KW_DOUBLE, TokenType::KW_DECIMAL, TokenType::KW_BYTE,
                            TokenType::KW_DATE, TokenType::KW_BOOL, TokenType::KW_CHAR,
                            TokenType::KW_STRING_TYPE, TokenType::KW_AUTO}) ||
                typeTok.type == TokenType::IDENTIFIER;
            if (!isTypeKw || !typeTok.token)
                break;
            std::string paramType = typeTok.token->token;
            nextToken();
            // int[][] a — tip sonrasında [] boyutları
            while (currentToken().type == TokenType::LBRACKET) {
                nextToken();
                if (currentToken().type == TokenType::RBRACKET)
                    nextToken();
                else
                    reportError(currentToken().token ? currentToken().token->loc : fn->loc,
                                "E905", "expected ']' in parameter array type");
                paramType += "[]";
            }
            // ADR-021: nullable parametre — int? a
            if (currentToken().type == TokenType::TERNARY) {
                nextToken();
                paramType += "?";
            }
            if (currentToken().type != TokenType::IDENTIFIER || !currentToken().token)
                break;
            VariableDeclNode* param = new VariableDeclNode();
            param->loc = currentToken().token->loc;
            param->varType = paramType;
            param->name = currentToken().token->token;
            nextToken();
            fn->params.push_back(param);
            if (currentToken().type == TokenType::COMMA)
                nextToken();
        }
        if (currentToken().type == TokenType::RPAREN)
            nextToken();
        else
            reportError(currentToken().token ? currentToken().token->loc : fn->loc,
                        "E905", "expected ')' after parameter list");
    }

    if (currentToken().type == TokenType::LBRACE) {
        ASTNode* body = parseBlock();
        fn->addChild(body);
    }

    return fn;
}

// ADR-034 (#107): ffi <ret> <ad>(<params>) : <HOST_ID> from <mod> [requires <cap>];
// Gövdesiz gömülü host fonksiyon bildirimi. `from`/`requires` contextual keyword
// (IDENTIFIER metni), modül adı tırnaksız identifier.
ASTNode* Parser::parseFfiDecl() {
    FfiDeclNode* fn = new FfiDeclNode();
    fn->loc = currentToken().token->loc;
    nextToken(); // 'ffi' tüket

    // dönüş tipi
    fn->returnType = currentToken().token ? currentToken().token->token : "";
    nextToken();
    while (currentToken().type == TokenType::LBRACKET) {
        nextToken();
        if (currentToken().type == TokenType::RBRACKET)
            nextToken();
        fn->returnType += "[]";
    }
    if (currentToken().type == TokenType::TERNARY) {
        nextToken();
        fn->returnType += "?";
    }

    // ad
    fn->name = currentToken().token ? currentToken().token->token : "";
    nextToken();

    // parametreler (parseFunctionDecl ile aynı desen)
    if (currentToken().type == TokenType::LPAREN) {
        nextToken();
        while (currentToken().type != TokenType::RPAREN &&
               currentToken().type != TokenType::SVR_VOID) {
            auto typeTok = currentToken();
            bool isTypeKw =
                typeTok.is({TokenType::KW_VOID, TokenType::KW_INT, TokenType::KW_FLOAT_TYPE,
                            TokenType::KW_DOUBLE, TokenType::KW_DECIMAL, TokenType::KW_BYTE,
                            TokenType::KW_DATE, TokenType::KW_BOOL, TokenType::KW_CHAR,
                            TokenType::KW_STRING_TYPE}) ||
                typeTok.type == TokenType::IDENTIFIER;
            if (!isTypeKw || !typeTok.token)
                break;
            std::string paramType = typeTok.token->token;
            nextToken();
            while (currentToken().type == TokenType::LBRACKET) {
                nextToken();
                if (currentToken().type == TokenType::RBRACKET)
                    nextToken();
                paramType += "[]";
            }
            if (currentToken().type == TokenType::TERNARY) {
                nextToken();
                paramType += "?";
            }
            if (currentToken().type != TokenType::IDENTIFIER || !currentToken().token)
                break;
            VariableDeclNode* param = new VariableDeclNode();
            param->loc = currentToken().token->loc;
            param->varType = paramType;
            param->name = currentToken().token->token;
            nextToken();
            fn->params.push_back(param);
            if (currentToken().type == TokenType::COMMA)
                nextToken();
        }
        if (currentToken().type == TokenType::RPAREN)
            nextToken();
    }

    // : <HOST_ID>
    if (currentToken().type == TokenType::COLON) {
        nextToken();
        if (currentToken().type == TokenType::IDENTIFIER && currentToken().token) {
            fn->hostId = currentToken().token->token;
            nextToken();
        }
    }

    // from <mod>   (contextual keyword 'from', tırnaksız modül adı)
    if (currentToken().type == TokenType::IDENTIFIER && currentToken().token->token == "from") {
        nextToken();
        if (currentToken().type == TokenType::IDENTIFIER && currentToken().token) {
            fn->moduleName = currentToken().token->token;
            nextToken();
        }
    }

    // [requires <cap>]  (contextual keyword)
    if (currentToken().type == TokenType::IDENTIFIER && currentToken().token->token == "requires") {
        nextToken();
        if (currentToken().type == TokenType::IDENTIFIER && currentToken().token) {
            fn->requiresCap = currentToken().token->token;
            nextToken();
        }
    }

    if (currentToken().type == TokenType::SEMICOLON)
        nextToken();
    return fn;
}

ASTNode* Parser::parseStructDecl() {
    StructDeclNode* st = new StructDeclNode();
    st->loc = currentToken().token->loc;
    nextToken();
    if (currentToken().type == TokenType::IDENTIFIER) {
        st->name = currentToken().token->token;
        nextToken();
    }
    if (currentToken().type == TokenType::LBRACE) {
        nextToken();
        while (currentToken().type != TokenType::RBRACE &&
               currentToken().type != TokenType::SVR_VOID) {
            ASTNode* field = parseDeclaration();
            if (field)
                st->addChild(field);
            else
                break;
        }
        if (currentToken().type == TokenType::RBRACE)
            nextToken();
        else
            reportError(currentToken().token ? currentToken().token->loc : st->loc,
                        "E905", "expected '}' to close struct body");
    }
    if (currentToken().type == TokenType::SEMICOLON)
        nextToken();
    return st;
}

ASTNode* Parser::parseEnumDecl() {
    EnumDeclNode* en = new EnumDeclNode();
    en->loc = currentToken().token->loc;
    nextToken(); // 'enum' tüket
    if (currentToken().type == TokenType::IDENTIFIER) {
        en->name = currentToken().token->token;
        nextToken();
    }
    if (currentToken().type == TokenType::LBRACE) {
        nextToken();
        int nextVal = 0;
        while (currentToken().type != TokenType::RBRACE &&
               currentToken().type != TokenType::SVR_VOID) {
            if (currentToken().type != TokenType::IDENTIFIER)
                break;
            EnumMember m;
            m.name = currentToken().token->token;
            nextToken();
            // İsteğe bağlı açık değer: Red = 5
            if (currentToken().type == TokenType::EQUAL) {
                nextToken();
                if (currentToken().type == TokenType::NUMBER) {
                    m.value = std::stoi(currentToken().token->token);
                    nextToken();
                    nextVal = m.value + 1;
                }
            } else {
                m.value = nextVal++;
            }
            en->members.push_back(m);
            if (currentToken().type == TokenType::COMMA)
                nextToken();
        }
        if (currentToken().type == TokenType::RBRACE)
            nextToken();
        else
            reportError(currentToken().token ? currentToken().token->loc : en->loc,
                        "E905", "expected '}' to close enum body");
    }
    if (currentToken().type == TokenType::SEMICOLON)
        nextToken();
    return en;
}

ASTNode* Parser::parseVariableDecl() {
    VariableDeclNode* vd = new VariableDeclNode();
    vd->loc = currentToken().token->loc;
    vd->varType = currentToken().token->token;
    nextToken();

    // Java/C# stili: int[][] x — tip adından hemen sonra [] boyutları gelir
    while (currentToken().type == TokenType::LBRACKET) {
        nextToken();
        if (currentToken().type == TokenType::RBRACKET)
            nextToken();
        else
            reportError(currentToken().token ? currentToken().token->loc : vd->loc,
                        "E905", "expected ']' in variable array type");
        vd->varType += "[]";
    }

    // ADR-021: nullable soneki — int? x
    if (currentToken().type == TokenType::TERNARY) {
        nextToken();
        vd->varType += "?";
    }

    if (currentToken().type != TokenType::IDENTIFIER) {
        reportError(currentToken().token ? currentToken().token->loc : vd->loc, "E904",
                    "expected variable name");
        return vd;
    }

    vd->name = currentToken().token->token;
    nextToken();

    // C stili: int x[] — geriye dönük uyumluluk (postfix [])
    if (currentToken().type == TokenType::LBRACKET) {
        nextToken();
        while (currentToken().type != TokenType::RBRACKET &&
               currentToken().type != TokenType::SEMICOLON &&
               currentToken().type != TokenType::SVR_VOID)
            nextToken();
        if (currentToken().type == TokenType::RBRACKET)
            nextToken();
        if (vd->varType.back() != ']')
            vd->varType += "[]";
    }

    if (currentToken().type == TokenType::EQUAL) {
        nextToken();
        vd->initExpr = expectExpression("after '=' in declaration");
    }

    while (currentToken().type == TokenType::COMMA) {
        nextToken();

        if (currentToken().type != TokenType::IDENTIFIER) {
            reportError(currentToken().token ? currentToken().token->loc : vd->loc, "E904",
                        "expected variable name after ','");
            break;
        }

        VariableDeclNode* sibling = new VariableDeclNode();
        sibling->loc = currentToken().token->loc;
        sibling->varType = vd->varType;
        sibling->name = currentToken().token->token;
        nextToken();

        if (currentToken().type == TokenType::LBRACKET) {
            nextToken();
            while (currentToken().type != TokenType::RBRACKET &&
                   currentToken().type != TokenType::SEMICOLON &&
                   currentToken().type != TokenType::SVR_VOID)
                nextToken();
            if (currentToken().type == TokenType::RBRACKET)
                nextToken();
            else
                reportError(currentToken().token ? currentToken().token->loc : sibling->loc,
                            "E905", "expected ']' after postfix array declarator");
        }

        if (currentToken().type == TokenType::EQUAL) {
            nextToken();
            sibling->initExpr = expectExpression("after '=' in declaration");
        }

        vd->addChild(sibling);
    }

    expectSemicolon("declaration");

    return vd;
}

ASTNode* Parser::parseStatement() {
    auto ct = currentToken();

    if (ct.type == TokenType::LBRACE)
        return parseBlock();

    if (ct.type == TokenType::KW_IF)
        return parseIfStatement();

    if (ct.type == TokenType::KW_WHILE)
        return parseWhileStatement();

    if (ct.type == TokenType::KW_FOR)
        return parseForStatement();

    if (ct.type == TokenType::KW_DO)
        return parseDoWhileStatement();

    if (ct.type == TokenType::KW_RETURN)
        return parseReturnStatement();

    if (ct.type == TokenType::KW_BREAK)
        return parseBreakStatement();

    if (ct.type == TokenType::KW_CONTINUE)
        return parseContinueStatement();

    if (ct.type == TokenType::KW_TRY)
        return parseTryStatement();

    if (ct.type == TokenType::KW_THROW)
        return parseThrowStatement();

    if (ct.type == TokenType::KW_SWITCH)
        return parseSwitchStatement();

    // ADR-045: lock / unlock / wait deyimleri
    if (ct.type == TokenType::KW_LOCK || ct.type == TokenType::KW_UNLOCK)
        return parseLockStatement();
    if (ct.type == TokenType::KW_WAIT)
        return parseWaitStatement();

    // ADR-045: `shared` yalnız modül kapsamında (global) geçerlidir.
    if (ct.type == TokenType::KW_SHARED) {
        reportError(ct.token ? ct.token->loc : SourceLocation{}, "E014",
                    "'shared' is only allowed on module-level (global) declarations");
        nextToken();
        return parseStatement();
    }

    if (ct.is({TokenType::KW_VOID, TokenType::KW_INT, TokenType::KW_FLOAT_TYPE,
               TokenType::KW_DOUBLE, TokenType::KW_DECIMAL, TokenType::KW_BYTE, TokenType::KW_DATE,
               TokenType::KW_BOOL, TokenType::KW_CHAR, TokenType::KW_STRING_TYPE,
               TokenType::KW_POOL, TokenType::KW_LIST, TokenType::KW_THREAD_TYPE})) {
        if (lookahead(1).type == TokenType::COLON_COLON) {
            return parseExpressionStatement();
        }
        return parseVariableDecl();
    }

    if (ct.type == TokenType::KW_STRUCT)
        return parseStructDecl();

    if (ct.type == TokenType::KW_ENUM)
        return parseEnumDecl();

    if (ct.type == TokenType::KW_IMPORT)
        return parseImportDecl();

    // Kullanıcı tanımlı struct tipiyle değişken bildirimi: Point p; veya Point p = ...;
    if (ct.type == TokenType::IDENTIFIER) {
        auto la1 = lookahead(1);
        auto la2 = lookahead(2);
        // "TypeName varName" → değişken bildirimi
        if (la1.type == TokenType::IDENTIFIER)
            return parseVariableDecl();
        // "TypeName? varName" (ADR-021 nullable struct) — la1=TERNARY, la2=IDENTIFIER
        if (la1.type == TokenType::TERNARY && la2.type == TokenType::IDENTIFIER)
            return parseVariableDecl();
        // "TypeName[]...[] varName" — çok boyutlu struct/enum array bildirimi
        if (la1.type == TokenType::LBRACKET) {
            int off = 1;
            while (lookahead(off).type == TokenType::LBRACKET &&
                   lookahead(off + 1).type == TokenType::RBRACKET)
                off += 2;
            if (lookahead(off).type == TokenType::IDENTIFIER)
                return parseVariableDecl();
        }
    }

    return parseExpressionStatement();
}

ASTNode* Parser::parseBlock() {
    BlockNode* block = new BlockNode();
    block->loc = currentToken().token ? currentToken().token->loc : SourceLocation{};

    if (currentToken().type == TokenType::LBRACE)
        nextToken();

    while (currentToken().type != TokenType::RBRACE && currentToken().type != TokenType::SVR_VOID) {
        ASTNode* stmt = parseStatement();
        if (stmt)
            block->addChild(stmt);
        else
            break;
    }

    if (currentToken().type == TokenType::RBRACE)
        nextToken();
    else
        reportError(currentToken().token ? currentToken().token->loc : block->loc,
                    "E905", "expected '}' to close block");

    return block;
}

ASTNode* Parser::parseIfStatement() {
    IfStatementNode* ifNode = new IfStatementNode();
    ifNode->loc = currentToken().token->loc;
    nextToken();

    if (currentToken().type == TokenType::LPAREN) {
        nextToken();
        ifNode->condition = expectExpression("as if condition");
        if (currentToken().type == TokenType::RPAREN)
            nextToken();
        else
            reportError(currentToken().token ? currentToken().token->loc : ifNode->loc,
                        "E905", "expected ')' after if condition");
    } else {
        reportError(currentToken().token ? currentToken().token->loc : ifNode->loc,
                    "E905", "expected '(' after 'if'");
    }

    ifNode->thenBranch = parseStatement();

    if (currentToken().type == TokenType::KW_ELSE) {
        nextToken();
        ifNode->elseBranch = parseStatement();
    }

    return ifNode;
}

ASTNode* Parser::parseWhileStatement() {
    WhileStatementNode* ws = new WhileStatementNode();
    ws->loc = currentToken().token->loc;
    nextToken();

    if (currentToken().type == TokenType::LPAREN) {
        nextToken();
        ws->condition = expectExpression("as while condition");
        if (currentToken().type == TokenType::RPAREN)
            nextToken();
        else
            reportError(currentToken().token ? currentToken().token->loc : ws->loc,
                        "E905", "expected ')' after while condition");
    } else {
        reportError(currentToken().token ? currentToken().token->loc : ws->loc,
                    "E905", "expected '(' after 'while'");
    }

    ws->body = parseStatement();
    return ws;
}

ASTNode* Parser::parseForStatement() {
    ForStatementNode* fs = new ForStatementNode();
    fs->loc = currentToken().token->loc;
    nextToken();

    if (currentToken().type == TokenType::LPAREN)
        nextToken();

    // init bir deyimdir ve kendi ';'ünü tüketir (VarDecl/ExpressionStatement);
    // boş init ise ';' burada tüketilir.
    if (currentToken().type != TokenType::SEMICOLON)
        fs->init = parseStatement();
    else
        nextToken();

    if (currentToken().type != TokenType::SEMICOLON)
        fs->condition = expectExpression("as for condition");
    expectSemicolon("for condition");

    if (currentToken().type != TokenType::RPAREN)
        fs->update = expectExpression("as for update");
    if (currentToken().type == TokenType::RPAREN)
        nextToken();
    else
        reportError(currentToken().token ? currentToken().token->loc : fs->loc,
                    "E905", "expected ')' after for clauses");

    fs->body = parseStatement();

    return fs;
}

ASTNode* Parser::parseDoWhileStatement() {
    DoWhileStatementNode* dw = new DoWhileStatementNode();
    dw->loc = currentToken().token->loc;
    nextToken();

    dw->body = parseStatement();

    if (currentToken().type == TokenType::KW_WHILE) {
        nextToken();
        if (currentToken().type == TokenType::LPAREN) {
            nextToken();
            dw->condition = expectExpression("as do-while condition");
            if (currentToken().type == TokenType::RPAREN)
                nextToken();
            else
                reportError(currentToken().token ? currentToken().token->loc : dw->loc,
                            "E905", "expected ')' after do-while condition");
        } else {
            reportError(currentToken().token ? currentToken().token->loc : dw->loc,
                        "E905", "expected '(' after 'while'");
        }
        expectSemicolon("do-while statement");
    } else {
        reportError(currentToken().token ? currentToken().token->loc : dw->loc,
                    "E905", "expected 'while' after do-while body");
    }

    return dw;
}

ASTNode* Parser::parseReturnStatement() {
    ReturnStatementNode* rs = new ReturnStatementNode();
    rs->loc = currentToken().token->loc;
    nextToken();

    if (currentToken().type != TokenType::SEMICOLON && currentToken().type != TokenType::RBRACE) {
        rs->value = expectExpression("after 'return'");
    }

    expectSemicolon("return statement");

    return rs;
}

ASTNode* Parser::parseBreakStatement() {
    BreakStatementNode* bs = new BreakStatementNode();
    bs->loc = currentToken().token->loc;
    nextToken();
    expectSemicolon("'break'");
    return bs;
}

ASTNode* Parser::parseContinueStatement() {
    ContinueStatementNode* cs = new ContinueStatementNode();
    cs->loc = currentToken().token->loc;
    nextToken();
    expectSemicolon("'continue'");
    return cs;
}

ASTNode* Parser::parseExpressionStatement() {
    auto ct = currentToken();
    SourceLocation loc = ct.token ? ct.token->loc : lastLoc_;

    // Yalnız ';' — boş statement (no-op), hata değil: düzenleme sırasında
    // (satır silme/taşıma) sıkça oluşur, her seferinde tanı basmak gürültü olur.
    if (ct.type == TokenType::SEMICOLON) {
        nextToken();
        ExpressionStatementNode* empty = new ExpressionStatementNode();
        empty->loc = loc;
        return empty;
    }

    ASTNode* expr = parseExpression();
    if (!expr) {
        // Faz 2: bu noktadan önce hiçbir alt-kural bir mesaj basmadı (bkz.
        // parseNullDenotation) — tek konumlu tanı burada üretilir, ardından
        // panic-mode recovery ile bilinen bir sınıra kadar atlanır.
        std::string tokText = ct.token ? ct.token->token : "<eof>";
        return synchronizeAndMakeError(loc, "E901",
                                       "unexpected token '" + tokText + "' — expected a statement");
    }

    ExpressionStatementNode* es = new ExpressionStatementNode();
    es->loc = loc;
    es->expression = expr;
    expectSemicolon("expression");

    return es;
}

// ADR-025: try { body } catch (Error catchVar) { handler }
ASTNode* Parser::parseTryStatement() {
    TryStatementNode* ts = new TryStatementNode();
    ts->loc = currentToken().token->loc;
    nextToken(); // tüket: try

    ts->body = parseBlock();

    // catch (Error e)
    if (currentToken().type == TokenType::KW_CATCH) {
        nextToken(); // tüket: catch
        if (currentToken().type == TokenType::LPAREN)
            nextToken(); // tüket: (
        // "Error" tip adını atla
        if (currentToken().type == TokenType::IDENTIFIER ||
            currentToken().type == TokenType::KW_STRING_TYPE)
            nextToken(); // tüket: Error (ya da herhangi bir tip adı)
        // catch değişken adını al
        if (currentToken().type == TokenType::IDENTIFIER && currentToken().token)
            ts->catchVar = currentToken().token->token;
        nextToken(); // tüket: değişken adı
        if (currentToken().type == TokenType::RPAREN)
            nextToken(); // tüket: )
        else
            reportError(currentToken().token ? currentToken().token->loc : ts->loc,
                        "E905", "expected ')' after catch clause");
        ts->handler = parseBlock();
    }

    return ts;
}

// ADR-027: switch (expr) { case v1, v2: stmts break; default: stmts }
// Fallthrough YOK — her case otomatik break'li.
ASTNode* Parser::parseSwitchStatement() {
    SwitchStatementNode* sw = new SwitchStatementNode();
    sw->loc = currentToken().token->loc;
    nextToken(); // tüket: switch

    // subject: switch (expr)
    if (currentToken().type == TokenType::LPAREN) {
        nextToken();
        sw->subject = expectExpression("as switch subject");
        if (currentToken().type == TokenType::RPAREN)
            nextToken();
        else
            reportError(currentToken().token ? currentToken().token->loc : sw->loc,
                        "E905", "expected ')' after switch subject");
    }

    if (currentToken().type != TokenType::LBRACE)
        return sw;
    nextToken(); // tüket: {

    while (currentToken().type != TokenType::RBRACE && currentToken().type != TokenType::SVR_VOID) {
        auto ct = currentToken();

        if (ct.type == TokenType::KW_CASE) {
            nextToken(); // tüket: case
            CaseClause clause;

            // case değerlerini virgülle ayır: case 1, 2, 3:
            // COLON'ın önceliği 3 — parseExpression(3) ile ':'yi tüketmeyiz
            clause.values.push_back(expectExpression("after 'case'", 3));
            while (currentToken().type == TokenType::COMMA) {
                nextToken();
                clause.values.push_back(expectExpression("after ',' in case list", 3));
            }
            if (currentToken().type == TokenType::COLON)
                nextToken(); // tüket: :

            // Bu case'in body'si: case/default/} görene kadar
            while (currentToken().type != TokenType::KW_CASE &&
                   currentToken().type != TokenType::KW_DEFAULT &&
                   currentToken().type != TokenType::RBRACE &&
                   currentToken().type != TokenType::SVR_VOID) {
                ASTNode* stmt = parseStatement();
                if (stmt)
                    clause.body.push_back(stmt);
                else
                    break;
            }
            // Açık break varsa zaten tüketildi (parseStatement → parseBreakStatement)
            sw->cases.push_back(std::move(clause));

        } else if (ct.type == TokenType::KW_DEFAULT) {
            nextToken(); // tüket: default
            if (currentToken().type == TokenType::COLON)
                nextToken(); // tüket: :
            CaseClause clause;
            clause.isDefault = true;
            while (currentToken().type != TokenType::KW_CASE &&
                   currentToken().type != TokenType::KW_DEFAULT &&
                   currentToken().type != TokenType::RBRACE &&
                   currentToken().type != TokenType::SVR_VOID) {
                ASTNode* stmt = parseStatement();
                if (stmt)
                    clause.body.push_back(stmt);
                else
                    break;
            }
            sw->cases.push_back(std::move(clause));
        } else {
            // Beklenmedik token — atla
            nextToken();
        }
    }

    if (currentToken().type == TokenType::RBRACE)
        nextToken(); // tüket: }
    else
        reportError(currentToken().token ? currentToken().token->loc : sw->loc,
                    "E905", "expected '}' to close switch body");

    return sw;
}

// ADR-025: throw <ifade>;
ASTNode* Parser::parseThrowStatement() {
    ThrowStatementNode* th = new ThrowStatementNode();
    th->loc = currentToken().token->loc;
    nextToken(); // tüket: throw
    th->value = expectExpression("after 'throw'");
    expectSemicolon("throw statement");
    return th;
}

// ─────────────────────────────────────────────────────────────────────────────
// ADR-045 (Faz 3-a): izole thread dil yüzeyi
// ─────────────────────────────────────────────────────────────────────────────

std::string Parser::parseTypeName() {
    auto t = currentToken();
    const bool isTypeTok =
        t.is({TokenType::IDENTIFIER, TokenType::KW_INT, TokenType::KW_FLOAT_TYPE,
              TokenType::KW_DOUBLE, TokenType::KW_DECIMAL, TokenType::KW_BYTE, TokenType::KW_DATE,
              TokenType::KW_BOOL, TokenType::KW_CHAR, TokenType::KW_STRING_TYPE,
              TokenType::KW_THREAD_TYPE, TokenType::KW_POOL, TokenType::KW_LIST});
    if (!isTypeTok || !t.token) {
        reportError(t.token ? t.token->loc : SourceLocation{}, "E904", "expected a type name");
        return "";
    }
    std::string name = t.token->token;
    nextToken();
    while (currentToken().type == TokenType::LBRACKET &&
           lookahead(1).type == TokenType::RBRACKET) {
        nextToken();
        nextToken();
        name += "[]";
    }
    if (currentToken().type == TokenType::TERNARY) {
        nextToken();
        name += "?";
    }
    return name;
}

// shared <tip> <ad> [= başlatıcı];  — yalnız modül kapsamı (parseDeclaration).
ASTNode* Parser::parseSharedDecl() {
    auto kw = currentToken();
    nextToken(); // shared
    if (!currentToken().token) {
        reportError(kw.token ? kw.token->loc : SourceLocation{}, "E904",
                    "expected a declaration after 'shared'");
        return nullptr;
    }
    ASTNode* decl = parseVariableDecl();
    if (auto* vd = dynamic_cast<VariableDeclNode*>(decl)) {
        vd->isShared = true;
        for (ASTNode* sib : vd->getChildren())
            if (auto* sv = dynamic_cast<VariableDeclNode*>(sib)) sv->isShared = true;
    }
    return decl;
}

// lock a;  lock a, b;  unlock a;
ASTNode* Parser::parseLockStatement() {
    auto* ls     = new LockStatementNode();
    ls->loc      = currentToken().token->loc;
    ls->isUnlock = currentToken().type == TokenType::KW_UNLOCK;
    nextToken(); // lock / unlock
    for (;;) {
        auto t = currentToken();
        if (t.type != TokenType::IDENTIFIER || !t.token) {
            reportError(t.token ? t.token->loc : ls->loc, "E904",
                        std::string("expected a shared variable name after '") +
                            (ls->isUnlock ? "unlock" : "lock") + "'");
            break;
        }
        auto* id        = new IdentifierNode();
        id->loc         = t.token->loc;
        id->lexerToken  = t.token;
        id->parserToken = t;
        id->parent      = ls;
        ls->targets.push_back(id);
        nextToken();
        if (currentToken().type != TokenType::COMMA) break;
        nextToken();
    }
    expectSemicolon(ls->isUnlock ? "unlock statement" : "lock statement");
    return ls;
}

// wait(koşul);
ASTNode* Parser::parseWaitStatement() {
    auto* ws = new WaitStatementNode();
    ws->loc  = currentToken().token->loc;
    nextToken(); // wait
    if (currentToken().type == TokenType::LPAREN)
        nextToken();
    else
        reportError(currentToken().token ? currentToken().token->loc : ws->loc, "E905",
                    "expected '(' after 'wait'");
    ws->condition = expectExpression("in wait condition");
    if (ws->condition) ws->condition->parent = ws;
    if (currentToken().type == TokenType::RPAREN)
        nextToken();
    else
        reportError(currentToken().token ? currentToken().token->loc : ws->loc, "E905",
                    "expected ')' to close wait condition");
    expectSemicolon("wait statement");
    return ws;
}
