// ============================================================================
// saQut LSP — Tamamlama motoru (Bölüm 1, docs/lsp-decisions.md)
// ============================================================================
//
// Akış:
//   1. İmleç öncesi token dizisinden bağlam: import, `.` üyesi (önekli ya da
//      öneksiz), `::`, lock hedefi, tip beklenen konum, deyim başı, ifade.
//   2. Üye bağlamında alıcı ifadesi geriye doğru yürünür (lsp_analysis.cpp)
//      ve kök ad KAPSAM BİLİNÇLİ çözülür. Güncel analiz kökü çözemezse (dosya
//      o an bozuk) son geçerli analize düşülür.
//   3. Sıralama (sortText): yerel > aynı dosya > import edilmiş > yerleşik/
//      anahtar kelime > import edilmemiş proje sembolleri (Bölüm 3).
//
// ============================================================================

#include "lsp/lsp_handler.hpp"
#include "lsp/lsp_analysis.hpp"
#include "lsp/position.hpp"
#include "ffi/ffi_catalog.hpp"
#include "parser/nodes/declarations.hpp"
#include "symbol/scope.hpp"
#include <algorithm>
#include <cctype>
#include <set>

namespace {

// CompletionItemKind: Method=2, Function=3, Field=5, Variable=6, Module=9,
// Enum=13, Keyword=14, Snippet=15, EnumMember=20, Struct=22
int completionKind(SymbolKind k) {
    switch (k) {
        case SymbolKind::Function:   return 3;
        case SymbolKind::Variable:   return 6;
        case SymbolKind::Parameter:  return 6;
        case SymbolKind::Field:      return 5;
        case SymbolKind::Struct:     return 22;
        case SymbolKind::Enum:       return 13;
        case SymbolKind::EnumValue:  return 20;
    }
    return 6;
}

struct Ctx {
    enum Kind { Statement, Expression, Member, Scope, ImportModule, ImportSymbol,
                LockTarget, TypeName };
    Kind        kind = Statement;
    std::string prefix;          // imleçteki kısmi tanımlayıcı
    int         dotIdx = -1;     // Member: '.' token'ının indeksi
    std::string target;          // Scope: `x::` solu
    std::string importModule;    // ImportSymbol
    std::string importPath;      // ImportSymbol (dosya importu, tırnaklı)
};

// Tokens: sıralı; imleçten önce biten token'ların indeksleri (en yakın başta).
Ctx analyze(const std::string& content, const std::vector<Token*>& toks, int off) {
    Ctx ctx;
    // İmleci içeren ya da tam imlecte biten tanımlayıcı → önek.
    auto it = std::upper_bound(toks.begin(), toks.end(), off,
        [](int o, Token* t) { return o < t->start; });
    int k = static_cast<int>(it - toks.begin()) - 1;   // start < off olan son token (ya da start <= off)
    while (k >= 0 && toks[static_cast<size_t>(k)]->start >= off) --k;
    int prefixIdx = -1;
    if (k >= 0) {
        Token* t = toks[static_cast<size_t>(k)];
        bool wordish = t->gettype() == "identifier" || t->gettype() == "keyword";
        if (wordish && t->start < off && off <= t->end) {
            ctx.prefix = t->token.substr(0, static_cast<size_t>(off - t->start));
            prefixIdx = k;
            --k;
        }
    }
    // Önekten önceki token'lar (k ve geriye).
    auto tokAt = [&](int i) -> Token* {
        return (i >= 0 && i < static_cast<int>(toks.size())) ? toks[static_cast<size_t>(i)] : nullptr;
    };
    Token* p1 = tokAt(k);
    Token* p2 = tokAt(k - 1);

    // ── import bağlamları ────────────────────────────────────────────────
    if (p1 && p1->token == "import") { ctx.kind = Ctx::ImportModule; return ctx; }
    if (p1) {
        // `import { a, | } from X` — geriye `{` + `import`a kadar.
        for (int i = k; i >= 0 && i > k - 64; --i) {
            const std::string& s = toks[static_cast<size_t>(i)]->token;
            if (s == ";" || s == "}") break;
            if (s == "{") {
                if (i >= 1 && toks[static_cast<size_t>(i - 1)]->token == "import") {
                    ctx.kind = Ctx::ImportSymbol;
                    // `from` sonrası: tırnaksız modül adı ya da tırnaklı dosya yolu
                    for (size_t j = static_cast<size_t>(k + 1); j < toks.size() && j < static_cast<size_t>(k + 64); ++j) {
                        if (toks[j]->token == ";") break;
                        if (toks[j]->token == "from" && j + 1 < toks.size()) {
                            Token* m = toks[j + 1];
                            if (m->gettype() == "string") ctx.importPath = m->token;
                            else ctx.importModule = m->token;
                            break;
                        }
                    }
                    return ctx;
                }
                break;
            }
        }
    }

    // ── üye: `alıcı.` / `alıcı.önek` ──────────────────────────────────────
    // Önek bir alt satırdaysa önceki satırın sonunda kalmış yarım `x.`
    // bu önekin alıcısı değildir (bkz. lsp_analysis.cpp aynı kural): deyim başı.
    bool dotOnPrevLine = false;
    if (p1 && p1->token == "." && prefixIdx >= 0) {
        for (int q = p1->end; q < toks[static_cast<size_t>(prefixIdx)]->start &&
                              q < static_cast<int>(content.size()); ++q)
            if (content[static_cast<size_t>(q)] == '\n') { dotOnPrevLine = true; break; }
    }
    if (dotOnPrevLine) { ctx.kind = Ctx::Statement; return ctx; }
    if (p1 && p1->token == ".") {
        ctx.kind   = Ctx::Member;
        ctx.dotIdx = k;
        return ctx;
    }
    if (p1 && p1->token == "::") {
        ctx.kind = Ctx::Scope;
        if (p2 && (p2->gettype() == "identifier" || p2->gettype() == "keyword")) ctx.target = p2->token;
        return ctx;
    }

    // ── lock / unlock hedefi ──────────────────────────────────────────────
    for (int i = k; i >= 0 && i > k - 64; --i) {
        const std::string& s = toks[static_cast<size_t>(i)]->token;
        if (s == "lock" || s == "unlock") {
            // `lock a, b|` — araya yalnız ad ve virgül girebilir.
            bool ok = true;
            for (int j = i + 1; j <= k; ++j) {
                Token* t = toks[static_cast<size_t>(j)];
                if (t->token != "," && t->gettype() != "identifier") { ok = false; break; }
            }
            if (ok) { ctx.kind = Ctx::LockTarget; return ctx; }
            break;
        }
        if (s == ";" || s == "{" || s == "}" || s == "(" || s == ")") break;
    }

    // ── tip beklenen konum ────────────────────────────────────────────────
    if (p1 && p1->token == "(" && p2 && (p2->token == "Pool" || p2->token == "List")) {
        ctx.kind = Ctx::TypeName; ctx.target = "element"; return ctx;
    }
    if (p1 && p1->token == "as") { ctx.kind = Ctx::TypeName; ctx.target = "element"; return ctx; }
    if (p1 && p1->token == "shared") { ctx.kind = Ctx::TypeName; return ctx; }

    // ── deyim başı / ifade ────────────────────────────────────────────────
    if (!p1 || p1->token == ";" || p1->token == "{" || p1->token == "}") {
        ctx.kind = Ctx::Statement;
        return ctx;
    }
    ctx.kind = Ctx::Expression;
    return ctx;
}

// Yeni içerikteki offset'in eski içerikteki karşılığı, SATIR düzeyinde:
// ortak baştaki ve sondaki satırlar hizalanır; arada kalan değişmiş blok iki
// tarafta aynı sayıda satırsa satır satır eşlenir (aynı sütun, satır boyuna
// kırpılır), değilse bloğun eski karşılığının ilk satırına düşer. Birden çok
// yerde düzenleme (başlıkta yazım hatası + gövdede yarım ifade) olağan olduğu
// için bayt düzeyinde tek değişim bölgesi varsaymak yetmez.
int mapOffsetToOld(const std::string& now, const std::string& old, int off) {
    auto splitLines = [](const std::string& s) {
        std::vector<std::pair<size_t, size_t>> v;   // (başlangıç, uzunluk)
        size_t st = 0;
        for (size_t i = 0; i <= s.size(); ++i)
            if (i == s.size() || s[i] == '\n') { v.push_back({st, i - st}); st = i + 1; }
        return v;
    };
    auto nl = splitLines(now), ol = splitLines(old);
    auto lineEq = [&](size_t a, size_t b) {
        return nl[a].second == ol[b].second &&
               now.compare(nl[a].first, nl[a].second, old, ol[b].first, ol[b].second) == 0;
    };
    size_t pre = 0;
    while (pre < nl.size() && pre < ol.size() && lineEq(pre, pre)) ++pre;
    size_t suf = 0;
    while (suf < nl.size() - pre && suf < ol.size() - pre &&
           lineEq(nl.size() - 1 - suf, ol.size() - 1 - suf)) ++suf;

    // İmlecin satırı ve sütunu (yeni içerikte).
    size_t line = 0;
    while (line + 1 < nl.size() && nl[line + 1].first <= static_cast<size_t>(off)) ++line;
    size_t col = static_cast<size_t>(off) - nl[line].first;

    size_t oldLine;
    if (line < pre) {
        oldLine = line;
    } else if (line >= nl.size() - suf) {
        oldLine = line - nl.size() + ol.size();
    } else {
        size_t newMid = nl.size() - suf - pre, oldMid = ol.size() - suf - pre;
        oldLine = (newMid == oldMid) ? line : pre;
        if (oldLine >= ol.size()) oldLine = ol.empty() ? 0 : ol.size() - 1;
    }
    col = std::min(col, ol[oldLine].second);
    return static_cast<int>(ol[oldLine].first + col);
}

struct Snippet { const char* label; const char* body; const char* detail; };
const Snippet kSnippets[] = {
    {"if",     "if (${1:koşul}) {\n\t$0\n}",                                  "if deyimi"},
    {"else",   "else {\n\t$0\n}",                                              "else bloğu"},
    {"for",    "for (int ${1:i} = 0; $1 < ${2:n}; $1 = $1 + 1) {\n\t$0\n}",   "sayaçlı döngü"},
    {"while",  "while (${1:koşul}) {\n\t$0\n}",                               "while döngüsü"},
    {"try",    "try {\n\t$1\n} catch (Error ${2:e}) {\n\t$0\n}",               "try / catch"},
    {"thread", "Thread ${1:t} = thread {\n\t$0\n};",                          "thread başlat (ADR-045)"},
    {"lock",   "lock ${1:paylaşılan};",                                        "blok sonuna kadar kilitle"},
    {"wait",   "wait(${1:koşul});",                                            "koşul doğru olana kadar bekle"},
};

}  // namespace

// ── Görünüm yardımcıları ────────────────────────────────────────────────────

static AnalysisView viewOf(DocumentState& s) {
    AnalysisView v;
    v.content = &s.content; v.tokens = &s.tokens; v.table = &s.symbolTable;
    v.filePath = &s.filePath; v.ast = s.ast;
    return v;
}

static AnalysisView viewOf(AnalysisSnapshot& s) {
    AnalysisView v;
    v.content = &s.content; v.tokens = &s.tokens; v.table = &s.symbolTable;
    v.filePath = &s.filePath; v.ast = s.ast;
    return v;
}

// Bu dosyanın import bildirimleri: kaynak ad → yerel ad (dosya ve FFI).
static std::unordered_map<std::string, std::string> importedNames(ASTNode* ast) {
    std::unordered_map<std::string, std::string> out;
    if (!ast) return out;
    for (ASTNode* c : ast->getChildren()) {
        if (c->kind != ASTKind::ImportDecl) continue;
        for (auto& n : static_cast<ImportDeclNode*>(c)->importedNames)
            out[n.source] = n.local.empty() ? n.source : n.local;
    }
    return out;
}

nlohmann::json LspHandler::handleCompletion(const nlohmann::json& id,
                                            const nlohmann::json& params) {
    std::string uri  = params["textDocument"]["uri"].get<std::string>();
    int         line = params["position"]["line"].get<int>();
    int         ch   = params["position"]["character"].get<int>();

    DocumentState* state = store_.get(uri);
    if (!state) return JsonRpc::makeResponse(id, nlohmann::json::array());

    int byteCol = toByteColumn(state->content, line, ch);
    int byteOff = lspLineStartOffset(state->content, line) + (byteCol - 1);
    nlohmann::json items = nlohmann::json::array();

    AnalysisView cur = viewOf(*state);
    ScopeIndex   si  = ScopeIndex::build(state->tokens);
    Ctx ctx = analyze(state->content, state->tokens, byteOff);

    auto withPrefix = [&](const std::string& label) {
        return ctx.prefix.empty() || label.rfind(ctx.prefix, 0) == 0;
    };

    // ── import { | } from X / import | ───────────────────────────────────
    if (ctx.kind == Ctx::ImportModule) {
        for (const auto& module : FfiCatalog::instance().modules()) {
            if (!withPrefix(module)) continue;
            items.push_back({
                {"label", module}, {"kind", 9},
                {"detail", "embedded FFI module"},
                // `import|` + Enter → `import { } from module`.
                {"insertText", " { $0 } from " + module},
                {"insertTextFormat", 2}
            });
        }
        return JsonRpc::makeResponse(id, items);
    }
    if (ctx.kind == Ctx::ImportSymbol) {
        if (!ctx.importModule.empty()) {
            for (const auto& name : FfiCatalog::instance().names(ctx.importModule)) {
                if (!withPrefix(name)) continue;
                items.push_back({{"label", name}, {"kind", 3},
                                 {"detail", ctx.importModule + " FFI"},
                                 {"insertText", name}});
            }
        } else if (!ctx.importPath.empty()) {
            for (auto& it : importableNamesFromFile(*state, ctx.importPath))
                if (withPrefix(it["label"].get<std::string>())) items.push_back(it);
        }
        return JsonRpc::makeResponse(id, items);
    }

    // ── üye tamamlama ────────────────────────────────────────────────────
    if (ctx.kind == Ctx::Member) {
        RootResolver curRoot = [&](const std::string& n, int off) {
            return resolveNameAt(cur, si, n, off);
        };
        ReceiverType r = receiverTypeEndingAt(cur, ctx.dotIdx - 1, curRoot);
        SymbolTable* table = &state->symbolTable;
        if (!r.ok && state->lastGood) {
            // Son geçerli analize düş: alıcı token'ları güncel metinden, kök
            // adın kapsamı ve tipler son sağlam turun tablosundan.
            AnalysisSnapshot& lg = *state->lastGood;
            AnalysisView old = viewOf(lg);
            ScopeIndex oldSi = ScopeIndex::build(lg.tokens);
            AnalysisView hybrid = cur;
            hybrid.table = &lg.symbolTable;
            RootResolver oldRoot = [&](const std::string& n, int off) {
                return resolveNameAt(old, oldSi, n,
                                     mapOffsetToOld(state->content, lg.content, off));
            };
            r = receiverTypeEndingAt(hybrid, ctx.dotIdx - 1, oldRoot);
            table = &lg.symbolTable;
        }
        return JsonRpc::makeResponse(id, memberCompletionItems(r, *table, ctx.prefix));
    }

    // ── `x::` (ADR-033 ad alanı / eski metot sözdizimi) ───────────────────
    if (ctx.kind == Ctx::Scope) {
        if (ctx.target.empty()) return JsonRpc::makeResponse(id, items);
        Symbol* objSym = resolveNameAt(cur, si, ctx.target, byteOff);
        if (objSym && (objSym->kind == SymbolKind::Struct || objSym->kind == SymbolKind::Enum))
            return JsonRpc::makeResponse(id, items);
        if (objSym) {
            items = builtinMethodsForType(objSym->type, objSym->name);
        } else {
            Type t = Type::fromName(ctx.target);
            if (!t.isError()) items = builtinMethodsForType(t, ctx.target);
        }
        return JsonRpc::makeResponse(id, items);
    }

    // ── lock hedefi: yalnız shared primitifler ────────────────────────────
    if (ctx.kind == Ctx::LockTarget) {
        for (Symbol* s : state->symbolTable.allSymbols()) {
            if (!s->isShared || s->kind != SymbolKind::Variable) continue;
            if (!s->type.isPrimitive()) continue;          // Pool/List kilitlenmez
            if (!withPrefix(displayName(s->name))) continue;
            items.push_back({{"label", displayName(s->name)}, {"kind", 6},
                             {"detail", symbolSignature(s)}});
        }
        return JsonRpc::makeResponse(id, items);
    }

    // ── tip adı ──────────────────────────────────────────────────────────
    if (ctx.kind == Ctx::TypeName) {
        // Pool(T)/List(T) elemanı ve `as` hedefi değer tipidir: koleksiyon/
        // Thread tipleri orada geçersiz (target == "element").
        const bool elementOnly = ctx.target == "element";
        for (const auto& t : lspBuiltinTypeNames())
            if (withPrefix(t) && t != "void" &&
                !(elementOnly && (t == "Pool" || t == "List" || t == "Thread")))
                items.push_back({{"label", t}, {"kind", 25},   // TypeParameter
                                 {"sortText", "3_" + t}});
        for (Symbol* s : state->symbolTable.allSymbols()) {
            if (s->kind != SymbolKind::Struct && s->kind != SymbolKind::Enum) continue;
            if (!symbolVisibleAt(cur, si, s, byteOff)) continue;
            std::string n = displayName(s->name);
            if (!withPrefix(n)) continue;
            const char* rank = !s->definitionLoc.isValid() ? "3_" : "1_";   // yerleşik Error
            items.push_back({{"label", n}, {"kind", completionKind(s->kind)},
                             {"detail", symbolSignature(s)}, {"sortText", rank + n}});
        }
        return JsonRpc::makeResponse(id, items);
    }

    // ── deyim başı / ifade: kapsamdaki semboller ─────────────────────────
    auto imports = importedNames(state->ast);
    std::set<std::string> seen;
    for (Symbol* s : state->symbolTable.allSymbols()) {
        if (s->kind == SymbolKind::Field || s->name.empty()) continue;
        std::string label = displayName(s->name);
        std::string rank;
        const bool builtin   = !s->definitionLoc.isValid();
        const bool otherFile = !builtin && s->definitionLoc.filePath() != state->filePath;
        if (otherFile) {
            // Başka dosyanın sembolü yalnız bu dosyaya import edildiyse görünür.
            auto imp = imports.find(label);
            if (imp == imports.end()) continue;
            label = imp->second;
            rank  = "2";
        } else if (builtin) {
            rank = "3";
        } else {
            if (!symbolVisibleAt(cur, si, s, byteOff)) continue;
            rank = (s->scope && s->scope->parent == nullptr) ? "1" : "0";
        }
        if (s->kind == SymbolKind::EnumValue) continue;   // `Renk.Kirmizi` ile gelir
        if (!withPrefix(label) || !seen.insert(label).second) continue;

        std::string detail = (s->kind == SymbolKind::Function) ? symbolSignature(s)
                                                               : s->type.toString();
        if (s->kind == SymbolKind::Struct || s->kind == SymbolKind::Enum) detail = symbolSignature(s);
        items.push_back({{"label", label}, {"kind", completionKind(s->kind)},
                         {"detail", detail}, {"sortText", rank + "_" + label}});
    }

    // Anahtar kelimeler ve snippet'ler: deyim başında hepsi, ifadede yalnız
    // değer üretenler.
    static const std::set<std::string> kExprKeywords = {
        "true", "false", "null", "thread", "Pool", "List"
    };
    std::set<std::string> snippetLabels;
    if (ctx.kind == Ctx::Statement) {
        for (const auto& sn : kSnippets) {
            if (!withPrefix(sn.label)) continue;
            snippetLabels.insert(sn.label);
            items.push_back({{"label", sn.label}, {"kind", 15}, {"detail", sn.detail},
                             {"insertText", sn.body}, {"insertTextFormat", 2},
                             {"sortText", std::string("3_") + sn.label}});
        }
    }
    for (const auto& kw : lspKeywords()) {
        if (!withPrefix(kw) || snippetLabels.count(kw) || seen.count(kw)) continue;
        if (ctx.kind == Ctx::Expression && !kExprKeywords.count(kw)) continue;
        items.push_back({{"label", kw}, {"kind", 14}, {"sortText", "3_" + kw}});
    }

    // Import edilmemiş proje sembolleri (Bölüm 3) — yalnız önek varken.
    if (!ctx.prefix.empty()) appendProjectCompletions(*state, ctx.prefix, seen, items);

    return JsonRpc::makeResponse(id, items);
}
