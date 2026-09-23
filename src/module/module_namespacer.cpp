// ============================================================================
// saQut Compiler — Modül Ad Alanı Geçişi (#246)
// ============================================================================
//
// Tasarım ve gerekçe: module_namespacer.hpp başlığı.
//
// ============================================================================

#include "module/module_namespacer.hpp"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "parser/nodes/binary_expr.hpp"
#include "parser/nodes/declarations.hpp"
#include "parser/nodes/expressions.hpp"
#include "parser/nodes/identifier.hpp"
#include "parser/nodes/program.hpp"
#include "parser/nodes/statements.hpp"

namespace {

using NameMap = std::unordered_map<std::string, std::string>;

// Bir birimin üst düzey bildirimlerinin adları (fonksiyon/struct/enum/global,
// kardeş global bildirimler dahil).
std::vector<std::string> topLevelNames(ASTNode* program) {
    std::vector<std::string> out;
    for (ASTNode* ch : program->getChildren()) {
        if (auto* fn = dynamic_cast<FunctionDeclNode*>(ch))
            out.push_back(fn->name);
        else if (auto* st = dynamic_cast<StructDeclNode*>(ch))
            out.push_back(st->name);
        else if (auto* en = dynamic_cast<EnumDeclNode*>(ch))
            out.push_back(en->name);
        else if (auto* vd = dynamic_cast<VariableDeclNode*>(ch)) {
            out.push_back(vd->name);
            for (ASTNode* sib : vd->getChildren())
                if (auto* s = dynamic_cast<VariableDeclNode*>(sib))
                    out.push_back(s->name);
        }
    }
    return out;
}

// ── Kapsam bilincine sahip yeniden yazıcı ────────────────────────────────────
class Rewriter {
public:
    // hidden: yalnız takma adla import edilmiş bir adın ORİJİNAL yazımı →
    // takma ad. `import {kare as sq}` sonrası `kare` bu dosyada görünmez.
    Rewriter(const NameMap& map, const NameMap& hidden, DiagnosticEngine& diag)
        : map_(map), hidden_(hidden), diag_(diag) {}

    void program(ASTNode* prog) {
        for (ASTNode* ch : prog->getChildren())
            topLevel(ch);
    }

private:
    const NameMap& map_;
    const NameMap& hidden_;
    DiagnosticEngine& diag_;
    std::vector<std::unordered_set<std::string>> scopes_;

    const std::string* lookup(const std::string& n) const {
        auto it = map_.find(n);
        return it == map_.end() ? nullptr : &it->second;
    }
    bool isLocal(const std::string& n) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
            if (it->count(n))
                return true;
        return false;
    }
    void declareLocal(const std::string& n) {
        if (!scopes_.empty())
            scopes_.back().insert(n);
    }

    // "Point", "Point[]", "Point?", "Point[]?" → taban adı yeniden yazılır.
    void typeName(std::string& t) {
        size_t cut = t.find_first_of("[?");
        std::string base = t.substr(0, cut);
        if (const std::string* to = lookup(base))
            t = *to + (cut == std::string::npos ? "" : t.substr(cut));
    }

    void rename(std::string& name) {
        if (const std::string* to = lookup(name))
            name = *to;
    }

    void topLevel(ASTNode* n) {
        if (auto* fn = dynamic_cast<FunctionDeclNode*>(n)) {
            rename(fn->name);
            typeName(fn->returnType);
            scopes_.emplace_back();
            for (auto* p : fn->params) {
                typeName(p->varType);
                declareLocal(p->name);
            }
            auto& ch = fn->getChildren();
            if (!ch.empty())
                stmt(ch[0]);
            scopes_.pop_back();
        } else if (auto* st = dynamic_cast<StructDeclNode*>(n)) {
            rename(st->name);
            for (ASTNode* f : st->getChildren())
                if (auto* vd = dynamic_cast<VariableDeclNode*>(f))
                    typeName(vd->varType);   // alan ADLARI yeniden yazılmaz
        } else if (auto* en = dynamic_cast<EnumDeclNode*>(n)) {
            rename(en->name);
        } else if (auto* vd = dynamic_cast<VariableDeclNode*>(n)) {
            typeName(vd->varType);
            rename(vd->name);
            expr(vd->initExpr);
            for (ASTNode* sib : vd->getChildren())
                if (auto* s = dynamic_cast<VariableDeclNode*>(sib)) {
                    typeName(s->varType);
                    rename(s->name);
                    expr(s->initExpr);
                }
        }
    }

    void localDecl(VariableDeclNode* vd) {
        typeName(vd->varType);
        expr(vd->initExpr);          // başlatıcı, ad tanımlanmadan ÖNCE görülür
        declareLocal(vd->name);
        for (ASTNode* sib : vd->getChildren())
            if (auto* s = dynamic_cast<VariableDeclNode*>(sib)) {
                typeName(s->varType);
                expr(s->initExpr);
                declareLocal(s->name);
            }
    }

    void stmt(ASTNode* n) {
        if (!n)
            return;
        if (auto* vd = dynamic_cast<VariableDeclNode*>(n)) {
            localDecl(vd);
        } else if (dynamic_cast<BlockNode*>(n)) {
            scopes_.emplace_back();
            for (ASTNode* ch : n->getChildren())
                stmt(ch);
            scopes_.pop_back();
        } else if (auto* i = dynamic_cast<IfStatementNode*>(n)) {
            expr(i->condition);
            stmt(i->thenBranch);
            stmt(i->elseBranch);
        } else if (auto* w = dynamic_cast<WhileStatementNode*>(n)) {
            expr(w->condition);
            stmt(w->body);
        } else if (auto* d = dynamic_cast<DoWhileStatementNode*>(n)) {
            stmt(d->body);
            expr(d->condition);
        } else if (auto* f = dynamic_cast<ForStatementNode*>(n)) {
            scopes_.emplace_back();
            stmt(f->init);
            expr(f->condition);
            expr(f->update);
            stmt(f->body);
            scopes_.pop_back();
        } else if (auto* r = dynamic_cast<ReturnStatementNode*>(n)) {
            expr(r->value);
        } else if (auto* t = dynamic_cast<ThrowStatementNode*>(n)) {
            expr(t->value);
        } else if (auto* es = dynamic_cast<ExpressionStatementNode*>(n)) {
            expr(es->expression);
        } else if (auto* tr = dynamic_cast<TryStatementNode*>(n)) {
            stmt(tr->body);
            scopes_.emplace_back();
            declareLocal(tr->catchVar);
            stmt(tr->handler);
            scopes_.pop_back();
        } else if (auto* sw = dynamic_cast<SwitchStatementNode*>(n)) {
            expr(sw->subject);
            scopes_.emplace_back();
            for (auto& c : sw->cases) {
                for (ASTNode* v : c.values)
                    expr(v);
                for (ASTNode* b : c.body)
                    stmt(b);
            }
            scopes_.pop_back();
        } else {
            // Break/Continue/Error ve bilinmeyen deyimler: ad taşımaz ya da
            // ifade olarak ele alınır.
            expr(n);
        }
    }

    void expr(ASTNode* n) {
        if (!n)
            return;
        if (auto* id = dynamic_cast<IdentifierNode*>(n)) {
            if (id->parserToken.token && !isLocal(id->parserToken.token->token)) {
                std::string& name = id->parserToken.token->token;
                auto h = hidden_.find(name);
                if (h != hidden_.end() && !map_.count(name))
                    diag_.report("E_SYMBOL_NOT_IMPORTED", id->loc,
                        "'" + name + "' was imported under the name '" + h->second +
                            "' in this file",
                        "use '" + h->second + "', or import it without `as`");
                rename(name);
            }
        } else if (auto* b = dynamic_cast<BinaryExpressionNode*>(n)) {
            expr(b->Left);
            expr(b->Right);
        } else if (auto* p = dynamic_cast<PostfixNode*>(n)) {
            expr(p->operand);
        } else if (auto* c = dynamic_cast<CallExpressionNode*>(n)) {
            expr(c->callee);
            for (ASTNode* a : c->arguments)
                expr(a);
        } else if (auto* m = dynamic_cast<MemberAccessNode*>(n)) {
            expr(m->object);                 // `Color.Red` → enum adı; alan adı değil
        } else if (auto* ix = dynamic_cast<IndexExpressionNode*>(n)) {
            expr(ix->object);
            expr(ix->index);
        } else if (auto* al = dynamic_cast<ArrayLiteralNode*>(n)) {
            for (ASTNode* e : al->elements)
                expr(e);
        } else if (auto* sc = dynamic_cast<ScopeCallNode*>(n)) {
            typeName(sc->leftTypeName);
            for (ASTNode* a : sc->arguments)
                expr(a);
        } else if (auto* ce = dynamic_cast<CastExpressionNode*>(n)) {
            expr(ce->operand);
            typeName(ce->targetTypeName);
        }
    }
};

// Bir adın "kimliği": aynı kimliği paylaşan bağlamalar aynı sembole işaret
// eder ve çakışma sayılmaz (iki modülün `sqrt`'ü aynı math::sqrt'tür).
struct Binding {
    size_t      unit;
    std::string identity;
};

} // namespace

void namespaceModules(ModuleGraph& graph, DiagnosticEngine& diag) {
    const size_t n = graph.units.size();
    if (n == 0)
        return;

    std::unordered_map<std::string, size_t> unitByPath;
    for (size_t i = 0; i < n; ++i)
        unitByPath[graph.units[i].filePath] = i;

    // 1. Ad → bağlamalar. Üst düzey bildirim kendi birimine özgü bir kimlik
    //    taşır; FFI importu hedef host fonksiyonunu kimlik olarak taşır.
    std::map<std::string, std::vector<Binding>> bindings;
    for (size_t i = 0; i < n; ++i) {
        for (const auto& name : topLevelNames(graph.units[i].ast))
            bindings[name].push_back({i, "decl:" + std::to_string(i)});
        for (ASTNode* ch : graph.units[i].ast->getChildren()) {
            auto* imp = dynamic_cast<ImportDeclNode*>(ch);
            if (!imp || !imp->isModuleName)
                continue;
            for (const auto& in : imp->importedNames) {
                const std::string local = in.local.empty() ? in.source : in.local;
                bindings[local].push_back({i, "ffi:" + imp->sourcePath + "::" + in.source});
            }
        }
    }

    // 2. Çakışan adlar: giriş modülünün (units[0]) kimliği — yoksa ilk
    //    kimlik — adı korur; farklı kimlikli diğer bağlamalar `ad@N` olur.
    std::vector<NameMap> own(n);
    for (auto& [name, list] : bindings) {
        std::set<std::string> ids;
        for (const auto& b : list)
            ids.insert(b.identity);
        if (ids.size() < 2)
            continue;
        std::string keeper = list.front().identity;
        for (const auto& b : list)
            if (b.unit == 0) {
                keeper = b.identity;
                break;
            }
        for (const auto& b : list)
            if (b.identity != keeper)
                own[b.unit][name] = name + "@" + std::to_string(b.unit);
    }

    // FFI import yerel adlarını ve dosya importlarını son adlara bağla.
    std::vector<NameMap> rewrite = own;
    std::vector<NameMap> hidden(n);
    std::vector<std::unordered_set<std::string>> directImports(n);
    for (size_t u = 0; u < n; ++u) {
        std::unordered_set<std::string> declaredHere;
        for (const auto& name : topLevelNames(graph.units[u].ast))
            declaredHere.insert(name);
        NameMap aliasTargets;   // yerel ad → son hedef adı (dosya importu)

        for (ASTNode* ch : graph.units[u].ast->getChildren()) {
            auto* imp = dynamic_cast<ImportDeclNode*>(ch);
            if (!imp)
                continue;
            if (imp->isModuleName) {
                for (auto& in : imp->importedNames) {
                    const std::string local = in.local.empty() ? in.source : in.local;
                    auto it = own[u].find(local);
                    if (it != own[u].end() && !declaredHere.count(local))
                        in.local = it->second;
                }
                continue;
            }
            auto src = unitByPath.find(imp->resolvedPath);
            if (src == unitByPath.end())
                continue;   // loader E_MODULE_NOT_FOUND raporlamıştır
            const NameMap& srcOwn = own[src->second];
            for (auto& in : imp->importedNames) {
                const std::string local = in.local.empty() ? in.source : in.local;
                auto so = srcOwn.find(in.source);
                const std::string target = so == srcOwn.end() ? in.source : so->second;

                const std::string original = in.source;
                if (declaredHere.count(local)) {
                    diag.report("E002", imp->loc,
                        "'" + local + "' is imported from '" + imp->sourcePath +
                            "' and also defined in this file",
                        "rename the local definition, or import it under another name: "
                        "`import { " + in.source + " as other } from \"" + imp->sourcePath +
                            "\";`");
                    // İzleyen tanılar (E_IMPORT_UNKNOWN) çakışmayı tekrar etmesin:
                    // import son adıyla kalır, yerel kullanım yeniden yazılmaz.
                    in.source = target;
                    in.local = target;
                    continue;
                }
                auto prev = aliasTargets.find(local);
                if (prev != aliasTargets.end() && prev->second != target) {
                    diag.report("E002", imp->loc,
                        "'" + local + "' is imported twice from different sources",
                        "import one of them under another name with `as`");
                    in.source = target;
                    in.local = target;
                    continue;
                }
                // Takma adla import: orijinal ad (hedef yeniden adlandırılmadıysa
                // kullanıcının yazabileceği ad) bu dosyada görünmemeli.
                if (local != original && target == original)
                    hidden[u][original] = local;
                if (local == original)
                    directImports[u].insert(original);
                aliasTargets[local] = target;
                // Import artık son adla ifade edilir: sembol toplayıcı hedefi
                // bu adla bulur, export denetimi bu adla yapılır, erişim
                // listesi bu adı içerir.
                in.source = target;
                in.local = target;
                if (local != target)
                    rewrite[u][local] = target;
            }
        }
    }

    // 3. Yeniden yaz.
    for (size_t u = 0; u < n; ++u) {
        // Aynı ad hem takma adla hem doğrudan import edildiyse gizlenmez.
        for (auto it = hidden[u].begin(); it != hidden[u].end();)
            it = (rewrite[u].count(it->first) || directImports[u].count(it->first))
                     ? hidden[u].erase(it) : std::next(it);
        if (!rewrite[u].empty() || !hidden[u].empty())
            Rewriter(rewrite[u], hidden[u], diag).program(graph.units[u].ast);
    }
}
