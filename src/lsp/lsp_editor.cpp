// ============================================================================
// saQut LSP — Editör hızlandırıcıları (Bölüm 4, docs/lsp-decisions.md)
// ============================================================================
//
// foldingRange, hiyerarşik documentSymbol, gereksiz kod ipuçları
// (DiagnosticTag.Unnecessary, Hint), codeAction hızlı düzeltmeleri,
// inlayHint (parametre adları), referans sayısı codeLens'i.
// Yalnız ön uç verisi: token dizisi, AST, sembol tablosu, proje indeksi.
// ============================================================================

#include "lsp/lsp_handler.hpp"
#include "lsp/lsp_analysis.hpp"
#include "lsp/position.hpp"
#include "parser/nodes/binary_expr.hpp"
#include "parser/nodes/declarations.hpp"
#include "parser/nodes/expressions.hpp"
#include "parser/nodes/identifier.hpp"
#include "parser/nodes/statements.hpp"
#include "symbol/scope.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

// ── Konum yardımcıları ──────────────────────────────────────────────────────

int lineOf(const DocumentState& s, int off) {
    auto it = std::upper_bound(s.lineStarts.begin(), s.lineStarts.end(), off);
    return std::max(0, static_cast<int>(it - s.lineStarts.begin()) - 1);
}

nlohmann::json posAt(const DocumentState& s, int off, const std::string& enc) {
    off = std::max(0, std::min(off, static_cast<int>(s.content.size())));
    int li = lineOf(s, off);
    int colByte = off - s.lineStarts[static_cast<size_t>(li)] + 1;
    int ch = enc == "utf-8" ? colByte - 1 : byteColToLspAt(s.content, s.lineStarts, li, colByte);
    return {{"line", li}, {"character", ch}};
}

nlohmann::json rangeAt(const DocumentState& s, int start, int end, const std::string& enc) {
    return {{"start", posAt(s, start, enc)}, {"end", posAt(s, end, enc)}};
}

bool isIdentChar(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

// [from, to) aralığında `word`ün ilk tam-kelime geçişi; yoksa -1.
int findWord(const std::string& text, const std::string& word, int from, int to) {
    to = std::min(to, static_cast<int>(text.size()));
    for (int i = std::max(0, from); i + static_cast<int>(word.size()) <= to; ++i) {
        if (text.compare(static_cast<size_t>(i), word.size(), word) != 0) continue;
        bool sOk = i == 0 || !isIdentChar(static_cast<unsigned char>(text[static_cast<size_t>(i - 1)]));
        size_t after = static_cast<size_t>(i) + word.size();
        bool eOk = after >= text.size() || !isIdentChar(static_cast<unsigned char>(text[after]));
        if (sOk && eOk) return i;
    }
    return -1;
}

// ── Gereksiz kod tespiti ────────────────────────────────────────────────────

struct Unnecessary {
    std::string       code;      // unused-variable | unused-import | unused-function | unreachable-code
    std::string       message;
    int               start = 0, end = 0;
    ImportDeclNode*   imp = nullptr;      // unused-import
    std::string       importName;         // yerel ad
    VariableDeclNode* var = nullptr;      // unused-variable
};

bool isPureExpr(ASTNode* e) {
    if (!e) return true;
    switch (e->kind) {
        case ASTKind::Literal:
        case ASTKind::Identifier:
            return true;
        case ASTKind::ArrayLiteral:
            for (auto* x : static_cast<ArrayLiteralNode*>(e)->elements)
                if (!isPureExpr(x)) return false;
            return true;
        case ASTKind::BinaryExpression: {
            auto* b = static_cast<BinaryExpressionNode*>(e);
            switch (b->Operator) {
                // Bölme/mod çalışma zamanı hatası verebilir; atamalar yan etkidir.
                case TokenType::PLUS: case TokenType::MINUS: case TokenType::STAR:
                case TokenType::LESS: case TokenType::LESS_EQUAL:
                case TokenType::GREATER: case TokenType::GREATER_EQUAL:
                case TokenType::EQUAL_EQUAL: case TokenType::BANG_EQUAL:
                case TokenType::AMPERSAND: case TokenType::PIPE: case TokenType::CARET:
                case TokenType::AMPERSAND_AMPERSAND: case TokenType::PIPE_PIPE:
                    return isPureExpr(b->Left) && isPureExpr(b->Right);
                default:
                    return false;
            }
        }
        default:
            return false;   // çağrı, cast (aralık hatası), indeks (sınır hatası)...
    }
}

bool isTerminator(ASTNode* n) {
    return n && (n->kind == ASTKind::ReturnStatement || n->kind == ASTKind::ThrowStatement ||
                 n->kind == ASTKind::BreakStatement  || n->kind == ASTKind::ContinueStatement);
}

std::vector<Unnecessary> findUnnecessary(DocumentState& st) {
    std::vector<Unnecessary> out;
    if (!st.ast || st.filePath.empty()) return out;
    AnalysisView v;
    v.content = &st.content; v.tokens = &st.tokens; v.table = &st.symbolTable;
    v.filePath = &st.filePath; v.ast = st.ast;
    ScopeIndex si = ScopeIndex::build(st.tokens);

    auto refsInFile = [&](const Symbol* s, int excludeFrom = -1, int excludeTo = -1) {
        int n = 0;
        for (const auto& r : s->references) {
            if (r.filePath() != st.filePath) continue;
            if (excludeFrom >= 0 && r.offset >= excludeFrom && r.offset <= excludeTo) continue;
            ++n;
        }
        return n;
    };

    // Bildirim düğümleri (offset → VariableDecl) — değişken silme düzeltmesi için.
    std::unordered_map<int, VariableDeclNode*> varDecls;
    walkAst(st.ast, [&](ASTNode* n) {
        if (n->kind == ASTKind::VariableDecl) varDecls[n->loc.offset] = static_cast<VariableDeclNode*>(n);
    });

    // (a) kullanılmayan yerel değişkenler
    for (Symbol* s : st.symbolTable.allSymbols()) {
        if (s->kind != SymbolKind::Variable || s->name.empty()) continue;
        if (!s->definitionLoc.isValid() || s->definitionLoc.filePath() != st.filePath) continue;
        if (!s->scope || s->scope->parent == nullptr) continue;          // global
        const int d = s->definitionLoc.offset;
        int b = si.innermostBrace(d), p = si.innermostParen(d);
        if (p >= 0 && (b < 0 || si.paren(p).open > si.brace(b).open)) continue;   // for-init / catch
        if (refsInFile(s) > 0) continue;
        int ident = identOffsetFromDecl(st.content, d, s->name);
        if (ident < 0) continue;
        Unnecessary u;
        u.code = "unused-variable";
        u.message = "'" + s->name + "' is declared but never used";
        u.start = ident;
        u.end = ident + static_cast<int>(s->name.size());
        auto vd = varDecls.find(d);
        if (vd != varDecls.end()) u.var = vd->second;
        out.push_back(u);
    }

    // (b) kullanılmayan import adları
    for (ASTNode* c : st.ast->getChildren()) {
        if (c->kind != ASTKind::ImportDecl) continue;
        auto* imp = static_cast<ImportDeclNode*>(c);
        size_t semi = st.content.find(';', static_cast<size_t>(std::max(0, imp->loc.offset)));
        int declEnd = semi == std::string::npos ? static_cast<int>(st.content.size()) : static_cast<int>(semi);
        for (auto& nm : imp->importedNames) {
            const std::string local = nm.local.empty() ? nm.source : nm.local;
            const Symbol* target = nullptr;
            for (Symbol* s : st.symbolTable.allSymbols()) {
                if (displayName(s->name) != nm.source) continue;
                if (imp->isModuleName) {
                    if (s->hostFnId >= 0 && s->ffiModule == imp->sourcePath) { target = s; break; }
                } else if (s->definitionLoc.isValid() &&
                           s->definitionLoc.filePath() == imp->resolvedPath &&
                           s->scope && s->scope->parent == nullptr) {
                    target = s; break;
                }
            }
            if (!target || refsInFile(target) > 0) continue;
            // Tip konumundaki kullanımlar (`Nokta p;`, `Nokta f()`) sembol
            // referansı olarak kaydedilmez: yerel adla eşleşen, import
            // bildiriminin dışında ve `.` ile başlamayan bir tanımlayıcı
            // token'ı varsa kullanılıyor say (yanlış pozitif vermemek için).
            bool usedAsToken = false;
            for (size_t ti = 0; ti < st.tokens.size() && !usedAsToken; ++ti) {
                Token* t = st.tokens[ti];
                if (t->start >= imp->loc.offset && t->start <= declEnd) continue;
                if (t->token != local || t->gettype() != "identifier") continue;
                if (ti > 0 && st.tokens[ti - 1]->token == ".") continue;
                usedAsToken = true;
            }
            if (usedAsToken) continue;
            int at = findWord(st.content, nm.source, imp->loc.offset, declEnd);
            if (at < 0) continue;
            int end = at + static_cast<int>(nm.source.size());
            if (!nm.local.empty() && nm.local != nm.source) {
                int al = findWord(st.content, nm.local, end, declEnd);
                if (al >= 0) end = al + static_cast<int>(nm.local.size());
            }
            Unnecessary u;
            u.code = "unused-import";
            u.message = "'" + local + "' is imported but never used";
            u.start = at; u.end = end;
            u.imp = imp; u.importName = nm.source;
            out.push_back(u);
        }
    }

    // (c) export edilmemiş ve hiç çağrılmayan fonksiyonlar
    for (ASTNode* c : st.ast->getChildren()) {
        if (c->kind != ASTKind::FunctionDecl) continue;
        auto* f = static_cast<FunctionDeclNode*>(c);
        if (f->isExported || f->name == "main" || f->name.empty()) continue;
        Symbol* s = nullptr;
        for (Symbol* x : st.symbolTable.allSymbols())
            if (x->kind == SymbolKind::Function && x->name == f->name &&
                x->definitionLoc.isValid() && x->definitionLoc.filePath() == st.filePath) { s = x; break; }
        if (!s) continue;
        // Kendi gövdesindeki özyinelemeli çağrılar kullanım sayılmaz.
        int bodyOpen = -1, bodyClose = -1;
        size_t k = std::lower_bound(st.tokens.begin(), st.tokens.end(), f->loc.offset,
                       [](Token* t, int o) { return t->start < o; }) - st.tokens.begin();
        for (; k < st.tokens.size(); ++k)
            if (st.tokens[k]->token == "{") {
                int bi = si.braceOpeningAt(st.tokens[k]->start);
                if (bi >= 0) { bodyOpen = si.brace(bi).open; bodyClose = si.brace(bi).close; }
                break;
            }
        if (refsInFile(s, bodyOpen, bodyClose) > 0) continue;
        int ident = identOffsetFromDecl(st.content, f->loc.offset, f->name);
        if (ident < 0) continue;
        Unnecessary u;
        u.code = "unused-function";
        u.message = "'" + f->name + "' is never called (not exported)";
        u.start = ident; u.end = ident + static_cast<int>(f->name.size());
        out.push_back(u);
    }

    // (d) return/throw/break/continue sonrasındaki erişilemez deyimler
    auto trimEnd = [&](int end) {
        while (end > 0 && std::isspace(static_cast<unsigned char>(st.content[static_cast<size_t>(end - 1)]))) --end;
        return end;
    };
    auto checkList = [&](const std::vector<ASTNode*>& stmts, bool caseBody) {
        for (size_t i = 0; i + 1 < stmts.size(); ++i) {
            if (!isTerminator(stmts[i])) continue;
            ASTNode* first = stmts[i + 1];
            if (!first || first->loc.offset < 0) break;
            int b = si.innermostBrace(stmts[i]->loc.offset);
            int end = b >= 0 && si.brace(b).close != INT_MAX ? si.brace(b).close
                                                             : static_cast<int>(st.content.size());
            if (caseBody) {
                // Sonraki case/default etiketine kadar.
                size_t k = std::lower_bound(st.tokens.begin(), st.tokens.end(), first->loc.offset,
                               [](Token* t, int o) { return t->start < o; }) - st.tokens.begin();
                for (; k < st.tokens.size() && st.tokens[k]->start < end; ++k)
                    if ((st.tokens[k]->token == "case" || st.tokens[k]->token == "default") &&
                        si.innermostBrace(st.tokens[k]->start) == b) { end = st.tokens[k]->start; break; }
            }
            Unnecessary u;
            u.code = "unreachable-code";
            u.message = "Unreachable code";
            u.start = first->loc.offset;
            u.end = std::max(u.start + 1, trimEnd(end));
            out.push_back(u);
            break;
        }
    };
    walkAst(st.ast, [&](ASTNode* n) {
        if (n->kind == ASTKind::Block) checkList(n->getChildren(), false);
        else if (n->kind == ASTKind::SwitchStatement)
            for (auto& cc : static_cast<SwitchStatementNode*>(n)->cases) checkList(cc.body, true);
    });

    std::sort(out.begin(), out.end(),
              [](const Unnecessary& a, const Unnecessary& b) { return a.start < b.start; });
    return out;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Gereksiz kod ipuçları (publishDiagnostics'e eklenir)
// ─────────────────────────────────────────────────────────────────────────────

nlohmann::json LspHandler::unnecessaryDiagnostics(DocumentState& state) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& u : findUnnecessary(state))
        arr.push_back({
            {"range",    rangeAt(state, u.start, u.end, positionEncoding_)},
            {"severity", 4},                           // Hint — hata listesini kirletmez
            {"tags",     nlohmann::json::array({1})},  // Unnecessary → soluk gösterim
            {"code",     u.code},
            {"message",  u.message},
            {"source",   "saQut"}
        });
    return arr;
}

// ─────────────────────────────────────────────────────────────────────────────
// foldingRange
// ─────────────────────────────────────────────────────────────────────────────

nlohmann::json LspHandler::handleFoldingRange(const nlohmann::json& id,
                                              const nlohmann::json& params) {
    DocumentState* st = store_.get(params["textDocument"]["uri"].get<std::string>());
    nlohmann::json out = nlohmann::json::array();
    if (!st) return JsonRpc::makeResponse(id, out);
    std::vector<std::tuple<int, int, std::string>> ranges;   // start, end, kind

    // { } blokları: fonksiyon, struct/enum gövdesi, if/while/for, thread { }.
    // Kapanış satırı görünür kalır (VS Code alışkanlığı).
    ScopeIndex si = ScopeIndex::build(st->tokens);
    for (const auto& p : si.braces()) {
        if (p.close == INT_MAX) continue;
        int a = lineOf(*st, p.open), b = lineOf(*st, p.close);
        if (b - 1 > a) ranges.emplace_back(a, b - 1, "");
    }

    // Yorumlar: çok satırlı /* */ ve ardışık // satırları. String içindeki
    // "/*" yorum sayılmaz.
    const std::string& c = st->content;
    std::vector<int> lineComment(st->lineStarts.size(), 0);
    for (size_t i = 0; i < c.size(); ++i) {
        if (c[i] == '"') {
            for (++i; i < c.size() && c[i] != '"' && c[i] != '\n'; ++i)
                if (c[i] == '\\') ++i;
        } else if (c.compare(i, 2, "//") == 0) {
            int li = lineOf(*st, static_cast<int>(i));
            size_t ls = static_cast<size_t>(st->lineStarts[static_cast<size_t>(li)]);
            if (c.find_first_not_of(" \t", ls) == i) lineComment[static_cast<size_t>(li)] = 1;
            while (i < c.size() && c[i] != '\n') ++i;
        } else if (c.compare(i, 2, "/*") == 0) {
            size_t e = c.find("*/", i + 2);
            if (e == std::string::npos) e = c.size();
            int a = lineOf(*st, static_cast<int>(i)), b = lineOf(*st, static_cast<int>(e));
            if (b > a) ranges.emplace_back(a, b, "comment");
            i = e + 1;
        }
    }
    for (size_t li = 0; li < lineComment.size();) {
        if (!lineComment[li]) { ++li; continue; }
        size_t lj = li;
        while (lj + 1 < lineComment.size() && lineComment[lj + 1]) ++lj;
        if (lj > li) ranges.emplace_back(static_cast<int>(li), static_cast<int>(lj), "comment");
        li = lj + 1;
    }

    // Import grubu (ilk import'tan son import'un ';' satırına).
    if (st->ast) {
        int first = -1, last = -1;
        for (ASTNode* ch : st->ast->getChildren()) {
            if (ch->kind != ASTKind::ImportDecl) continue;
            size_t semi = c.find(';', static_cast<size_t>(std::max(0, ch->loc.offset)));
            int a = lineOf(*st, ch->loc.offset);
            int b = semi == std::string::npos ? a : lineOf(*st, static_cast<int>(semi));
            if (first < 0) first = a;
            last = std::max(last, b);
        }
        if (first >= 0 && last > first) ranges.emplace_back(first, last, "imports");
    }

    std::sort(ranges.begin(), ranges.end());
    ranges.erase(std::unique(ranges.begin(), ranges.end()), ranges.end());
    for (auto& [a, b, kind] : ranges) {
        nlohmann::json r = {{"startLine", a}, {"endLine", b}};
        if (!kind.empty()) r["kind"] = kind;
        out.push_back(r);
    }
    return JsonRpc::makeResponse(id, out);
}

// ─────────────────────────────────────────────────────────────────────────────
// documentSymbol — hiyerarşik (outline + breadcrumbs)
// ─────────────────────────────────────────────────────────────────────────────

nlohmann::json LspHandler::handleDocumentSymbol(const nlohmann::json& id,
                                                const nlohmann::json& params) {
    DocumentState* st = store_.get(params["textDocument"]["uri"].get<std::string>());
    nlohmann::json out = nlohmann::json::array();
    if (!st || !st->ast) return JsonRpc::makeResponse(id, out);
    ScopeIndex si = ScopeIndex::build(st->tokens);
    const std::string& enc = positionEncoding_;

    // start'tan sonraki ilk `{`'nun eşi (yoksa start satırının sonu).
    auto blockEnd = [&](int start) {
        size_t k = std::lower_bound(st->tokens.begin(), st->tokens.end(), start,
                       [](Token* t, int o) { return t->start < o; }) - st->tokens.begin();
        for (; k < st->tokens.size(); ++k) {
            if (st->tokens[k]->token == ";") return st->tokens[k]->end;
            if (st->tokens[k]->token == "{") {
                int bi = si.braceOpeningAt(st->tokens[k]->start);
                if (bi >= 0 && si.brace(bi).close != INT_MAX) return si.brace(bi).close + 1;
                return static_cast<int>(st->content.size());
            }
        }
        return static_cast<int>(st->content.size());
    };
    auto stmtEnd = [&](int start) {
        size_t semi = st->content.find(';', static_cast<size_t>(std::max(0, start)));
        return semi == std::string::npos ? static_cast<int>(st->content.size()) : static_cast<int>(semi) + 1;
    };
    auto makeSym = [&](const std::string& name, int kind, const std::string& detail,
                       int start, int end, int ident) {
        if (ident < 0) ident = start;
        nlohmann::json s = {
            {"name", name}, {"kind", kind},
            {"range", rangeAt(*st, start, std::max(end, ident + static_cast<int>(name.size())), enc)},
            {"selectionRange", rangeAt(*st, ident, ident + static_cast<int>(name.size()), enc)}
        };
        if (!detail.empty()) s["detail"] = detail;
        return s;
    };
    auto symbolNamed = [&](const std::string& name, SymbolKind k) -> Symbol* {
        for (Symbol* s : st->symbolTable.allSymbols())
            if (s->kind == k && s->name == name && s->definitionLoc.isValid() &&
                s->definitionLoc.filePath() == st->filePath) return s;
        return nullptr;
    };

    // Bildirimin başı: AST konumu tipten başlar; hemen önündeki `export` /
    // `shared` niteleyicileri aralığa dahil edilir.
    auto declStart = [&](int off) {
        size_t k = std::lower_bound(st->tokens.begin(), st->tokens.end(), off,
                       [](Token* t, int o) { return t->start < o; }) - st->tokens.begin();
        while (k > 0 && (st->tokens[k - 1]->token == "export" || st->tokens[k - 1]->token == "shared"))
            off = st->tokens[--k]->start;
        return off;
    };

    for (ASTNode* c : st->ast->getChildren()) {
        if (c->loc.offset < 0) continue;
        const int start = declStart(c->loc.offset);
        switch (c->kind) {
            case ASTKind::FunctionDecl: {
                auto* f = static_cast<FunctionDeclNode*>(c);
                if (f->name.empty()) break;
                const int end = blockEnd(start);
                Symbol* fs = symbolNamed(f->name, SymbolKind::Function);
                nlohmann::json sym = makeSym(f->name, 12, fs ? symbolSignature(fs) : "",
                                             start, end, identOffsetFromDecl(st->content, start, f->name));
                // Gövdedeki yerel değişkenler (parametreler outline'da gürültü).
                nlohmann::json children = nlohmann::json::array();
                std::vector<Symbol*> locals;
                for (Symbol* s : st->symbolTable.allSymbols())
                    if (s->kind == SymbolKind::Variable && !s->name.empty() &&
                        s->definitionLoc.isValid() && s->definitionLoc.filePath() == st->filePath &&
                        s->scope && s->scope->parent != nullptr &&
                        s->definitionLoc.offset > start && s->definitionLoc.offset < end)
                        locals.push_back(s);
                std::sort(locals.begin(), locals.end(), [](Symbol* a, Symbol* b) {
                    return a->definitionLoc.offset < b->definitionLoc.offset; });
                for (Symbol* s : locals) {
                    int ident = identOffsetFromDecl(st->content, s->definitionLoc.offset, s->name);
                    children.push_back(makeSym(s->name, 13, s->type.toString(),
                                               s->definitionLoc.offset,
                                               ident + static_cast<int>(s->name.size()), ident));
                }
                if (!children.empty()) sym["children"] = children;
                out.push_back(sym);
                break;
            }
            case ASTKind::StructDecl: {
                auto* sd = static_cast<StructDeclNode*>(c);
                if (sd->name.empty()) break;
                nlohmann::json sym = makeSym(sd->name, 23, "struct", start, blockEnd(start),
                                             identOffsetFromDecl(st->content, start, sd->name));
                nlohmann::json children = nlohmann::json::array();
                for (ASTNode* fc : sd->getChildren()) {
                    if (fc->kind != ASTKind::VariableDecl) continue;
                    auto* vd = static_cast<VariableDeclNode*>(fc);
                    int ident = identOffsetFromDecl(st->content, vd->loc.offset, vd->name);
                    children.push_back(makeSym(vd->name, 8, vd->varType, vd->loc.offset,
                                               stmtEnd(vd->loc.offset), ident));
                }
                if (!children.empty()) sym["children"] = children;
                out.push_back(sym);
                break;
            }
            case ASTKind::EnumDecl: {
                auto* ed = static_cast<EnumDeclNode*>(c);
                if (ed->name.empty()) break;
                const int end = blockEnd(start);
                nlohmann::json sym = makeSym(ed->name, 10, "enum", start, end,
                                             identOffsetFromDecl(st->content, start, ed->name));
                nlohmann::json children = nlohmann::json::array();
                int from = start;
                for (const auto& m : ed->members) {
                    int at = findWord(st->content, m.name, from + 1, end);
                    if (at < 0) continue;
                    from = at;
                    children.push_back(makeSym(m.name, 22, std::to_string(m.value),
                                               at, at + static_cast<int>(m.name.size()), at));
                }
                if (!children.empty()) sym["children"] = children;
                out.push_back(sym);
                break;
            }
            case ASTKind::VariableDecl: {
                auto* vd = static_cast<VariableDeclNode*>(c);
                if (vd->name.empty()) break;
                std::string detail = (vd->isShared ? "shared " : "") + vd->varType;
                out.push_back(makeSym(vd->name, 13, detail, start, stmtEnd(start),
                                      identOffsetFromDecl(st->content, start, vd->name)));
                break;
            }
            default: break;
        }
    }
    return JsonRpc::makeResponse(id, out);
}

// ─────────────────────────────────────────────────────────────────────────────
// codeLens — yalnız referans sayısı
// ─────────────────────────────────────────────────────────────────────────────

nlohmann::json LspHandler::handleCodeLens(const nlohmann::json& id, const nlohmann::json& params) {
    const std::string uri = params["textDocument"]["uri"].get<std::string>();
    DocumentState* st = store_.get(uri);
    nlohmann::json out = nlohmann::json::array();
    if (!st || !st->ast) return JsonRpc::makeResponse(id, out);
    for (ASTNode* c : st->ast->getChildren()) {
        std::string name;
        if (c->kind == ASTKind::FunctionDecl) name = static_cast<FunctionDeclNode*>(c)->name;
        else if (c->kind == ASTKind::VariableDecl) name = static_cast<VariableDeclNode*>(c)->name;
        else continue;
        if (name.empty()) continue;
        int ident = identOffsetFromDecl(st->content, c->loc.offset, name);
        if (ident < 0) continue;
        // Proje indeksi: bu dosya dahil tüm dosyalardan kullanımlar (açık
        // belgeninki analizinden, diğerlerininki indeksten).
        nlohmann::json locs = projectReferenceLocations(st->filePath, name);
        const size_t n = locs.size();
        nlohmann::json range = rangeAt(*st, ident, ident + static_cast<int>(name.size()),
                                       positionEncoding_);
        out.push_back({
            {"range", range},
            {"command", {
                {"title", "Referanslar: " + std::to_string(n)},
                {"command", "saqut.showReferences"},
                {"arguments", nlohmann::json::array({uri, range["start"], locs})}
            }}
        });
    }
    return JsonRpc::makeResponse(id, out);
}

// ─────────────────────────────────────────────────────────────────────────────
// inlayHint — çağrı yerinde parametre adları (literal argümanlarda)
// ─────────────────────────────────────────────────────────────────────────────

nlohmann::json LspHandler::handleInlayHint(const nlohmann::json& id, const nlohmann::json& params) {
    DocumentState* st = store_.get(params["textDocument"]["uri"].get<std::string>());
    nlohmann::json out = nlohmann::json::array();
    if (!st || !st->ast || !inlayParamNames_) return JsonRpc::makeResponse(id, out);
    int fromLine = 0, toLine = INT_MAX;
    if (params.contains("range") && params["range"].is_object()) {
        fromLine = params["range"].value("start", nlohmann::json::object()).value("line", 0);
        toLine   = params["range"].value("end", nlohmann::json::object()).value("line", INT_MAX);
    }
    walkAst(st->ast, [&](ASTNode* n) {
        if (n->kind != ASTKind::Call) return;
        auto* call = static_cast<CallExpressionNode*>(n);
        if (!call->callee || call->callee->kind != ASTKind::Identifier) return;
        auto* idn = static_cast<IdentifierNode*>(call->callee);
        Symbol* fn = idn->resolvedSymbol;
        if (!fn) {
            auto f = st->symbolByOffset.find(idn->loc.offset);
            if (f != st->symbolByOffset.end()) fn = f->second;
        }
        if (!fn || fn->kind != SymbolKind::Function || fn->paramNames.empty()) return;
        for (size_t i = 0; i < call->arguments.size() && i < fn->paramNames.size(); ++i) {
            ASTNode* a = call->arguments[i];
            if (!a || a->kind != ASTKind::Literal || a->loc.offset < 0) continue;
            int line = lineOf(*st, a->loc.offset);
            if (line < fromLine || line > toLine) continue;
            out.push_back({
                {"position", posAt(*st, a->loc.offset, positionEncoding_)},
                {"label", fn->paramNames[i] + ":"},
                {"kind", 2},            // Parameter
                {"paddingRight", true}
            });
        }
    });
    return JsonRpc::makeResponse(id, out);
}

// ─────────────────────────────────────────────────────────────────────────────
// codeAction — hızlı düzeltmeler
// ─────────────────────────────────────────────────────────────────────────────

nlohmann::json LspHandler::handleCodeAction(const nlohmann::json& id, const nlohmann::json& params) {
    const std::string uri = params["textDocument"]["uri"].get<std::string>();
    DocumentState* st = store_.get(uri);
    nlohmann::json out = nlohmann::json::array();
    if (!st || !st->ast) return JsonRpc::makeResponse(id, out);

    int rs = 0, re = static_cast<int>(st->content.size());
    if (params.contains("range") && params["range"].is_object()) {
        auto offOf = [&](const nlohmann::json& p) {
            int line = p.value("line", 0), ch = p.value("character", 0);
            if (line >= static_cast<int>(st->lineStarts.size())) return static_cast<int>(st->content.size());
            return lspLineStartOffset(st->content, line) + (toByteColumn(st->content, line, ch) - 1);
        };
        rs = offOf(params["range"].value("start", nlohmann::json::object()));
        re = offOf(params["range"].value("end", nlohmann::json::object()));
    }
    const nlohmann::json ctxDiags = params.value("context", nlohmann::json::object())
                                          .value("diagnostics", nlohmann::json::array());
    auto matchingDiag = [&](const std::string& code) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& d : ctxDiags)
            if (d.is_object() && d.value("code", "") == code) arr.push_back(d);
        return arr;
    };
    auto action = [&](const std::string& title, const std::string& code, nlohmann::json edits,
                      bool preferred) {
        nlohmann::json a = {{"title", title}, {"kind", "quickfix"},
                            {"edit", {{"changes", {{uri, edits}}}}}};
        nlohmann::json diags = matchingDiag(code);
        if (!diags.empty()) a["diagnostics"] = diags;
        if (preferred) a["isPreferred"] = true;
        out.push_back(a);
    };
    // Tüm satırı (satır sonu dahil) silen aralık; satırda başka kod varsa yalnız [a, b).
    auto deleteRange = [&](int a, int b) {
        int ls = st->lineStarts[static_cast<size_t>(lineOf(*st, a))];
        int le = static_cast<int>(st->content.find('\n', static_cast<size_t>(b)));
        if (le < 0) le = static_cast<int>(st->content.size());
        bool onlyBefore = st->content.find_first_not_of(" \t", static_cast<size_t>(ls)) == static_cast<size_t>(a);
        bool onlyAfter  = st->content.find_first_not_of(" \t\r", static_cast<size_t>(b)) == static_cast<size_t>(le);
        if (onlyBefore && onlyAfter)
            return rangeAt(*st, ls, std::min(le + 1, static_cast<int>(st->content.size())), positionEncoding_);
        return rangeAt(*st, a, b, positionEncoding_);
    };

    for (const auto& u : findUnnecessary(*st)) {
        if (u.end < rs || u.start > re) continue;
        if (u.code == "unused-import" && u.imp) {
            size_t semi = st->content.find(';', static_cast<size_t>(u.imp->loc.offset));
            int declEnd = semi == std::string::npos ? static_cast<int>(st->content.size())
                                                    : static_cast<int>(semi) + 1;
            nlohmann::json edit;
            if (u.imp->importedNames.size() == 1) {
                edit = {{"range", deleteRange(u.imp->loc.offset, declEnd)}, {"newText", ""}};
            } else {
                // ", ad" ya da "ad, " — virgülüyle birlikte.
                int a = u.start, b = u.end;
                int j = b;
                while (j < declEnd && std::isspace(static_cast<unsigned char>(st->content[static_cast<size_t>(j)]))) ++j;
                if (j < declEnd && st->content[static_cast<size_t>(j)] == ',') {
                    b = j + 1;
                    while (b < declEnd && st->content[static_cast<size_t>(b)] == ' ') ++b;
                } else {
                    int i = a;
                    while (i > 0 && std::isspace(static_cast<unsigned char>(st->content[static_cast<size_t>(i - 1)]))) --i;
                    if (i > 0 && st->content[static_cast<size_t>(i - 1)] == ',') a = i - 1;
                }
                edit = {{"range", rangeAt(*st, a, b, positionEncoding_)}, {"newText", ""}};
            }
            action("Kullanılmayan import'u kaldır: " + u.importName, "unused-import",
                   nlohmann::json::array({edit}), true);
        } else if (u.code == "unused-variable" && u.var && isPureExpr(u.var->initExpr)) {
            int a = u.var->loc.offset;
            size_t semi = st->content.find(';', static_cast<size_t>(a));
            if (semi == std::string::npos) continue;
            action("Kullanılmayan değişkeni kaldır: " + u.var->name, "unused-variable",
                   nlohmann::json::array({{{"range", deleteRange(a, static_cast<int>(semi) + 1)},
                                           {"newText", ""}}}), true);
        }
    }

    // Tanımsız ad (E001) → projede bu adı export eden dosyadan import ekle.
    for (const auto& d : ctxDiags) {
        if (!d.is_object() || d.value("code", "") != "E001") continue;
        const std::string msg = d.value("message", "");
        if (msg.find("is not defined") == std::string::npos) continue;
        size_t q1 = msg.find('\''), q2 = msg.find('\'', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) continue;
        const std::string name = msg.substr(q1 + 1, q2 - q1 - 1);
        for (const auto& [path, entry] : index_.entries()) {
            if (path == st->filePath) continue;
            for (const auto& s : entry.symbols) {
                if (!s.exported || s.name != name) continue;
                nlohmann::json edit = importEditFor(*st, path, name);
                if (edit.is_null()) continue;
                std::string rel = fs::path(path)
                    .lexically_relative(fs::path(st->filePath).parent_path()).generic_string();
                nlohmann::json a = {{"title", "İmport Et: " + name + " (\"" + rel + "\")"},
                                    {"kind", "quickfix"},
                                    {"diagnostics", nlohmann::json::array({d})},
                                    {"edit", {{"changes", {{uri, nlohmann::json::array({edit})}}}}}};
                out.push_back(a);
            }
        }
    }
    return JsonRpc::makeResponse(id, out);
}
