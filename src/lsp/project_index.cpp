// ============================================================================
// saQut LSP — Proje indeksi gerçeklemesi
// ============================================================================

#include "lsp/project_index.hpp"
#include "lsp/lsp_analysis.hpp"
#include "lsp/document_store.hpp"
#include "lsp/position.hpp"
#include "core/module_registry.hpp"
#include "diagnostic/diagnostic_engine.hpp"
#include "parser/nodes/declarations.hpp"
#include "symbol/scope.hpp"
#include "symbol/symbol_collector.hpp"
#include "symbol/symbol_table.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// Yardımcılar
// ─────────────────────────────────────────────────────────────────────────────

bool globMatch(const std::string& pat, const std::string& path) {
    // Özyinelemeli eşleştirme; desenler kısa olduğu için yeterli.
    std::function<bool(size_t, size_t)> m = [&](size_t pi, size_t si) -> bool {
        while (pi < pat.size()) {
            if (pat.compare(pi, 2, "**") == 0) {
                size_t next = pi + 2;
                if (next < pat.size() && pat[next] == '/') ++next;   // "**/" sıfır dizin de olabilir
                for (size_t k = si; k <= path.size(); ++k)
                    if (m(next, k)) return true;
                return false;
            }
            if (pat[pi] == '*') {
                for (size_t k = si; k <= path.size(); ++k) {
                    if (m(pi + 1, k)) return true;
                    if (k < path.size() && path[k] == '/') break;
                }
                return false;
            }
            if (si >= path.size()) return false;
            if (pat[pi] != '?' && pat[pi] != path[si]) return false;
            if (pat[pi] == '?' && path[si] == '/') return false;
            ++pi; ++si;
        }
        return si == path.size();
    };
    return m(0, 0);
}

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

std::string docCommentAbove(const std::string& content, int line) {
    if (line <= 1) return "";
    std::vector<std::string> lines;
    {
        size_t st = 0;
        for (size_t i = 0; i <= content.size(); ++i)
            if (i == content.size() || content[i] == '\n') {
                lines.push_back(content.substr(st, i - st));
                st = i + 1;
            }
    }
    int i = line - 2;   // 0-tabanlı: tanımın bir üstü
    if (i < 0 || i >= static_cast<int>(lines.size())) return "";
    std::vector<std::string> out;
    std::string first = trim(lines[static_cast<size_t>(i)]);
    if (first.size() >= 2 && first.compare(first.size() - 2, 2, "*/") == 0) {
        // /* ... */ bloğu: yukarı doğru /*'a kadar.
        for (; i >= 0; --i) {
            std::string l = trim(lines[static_cast<size_t>(i)]);
            bool opens = l.find("/*") != std::string::npos;
            if (l.size() >= 2 && l.compare(l.size() - 2, 2, "*/") == 0) l = trim(l.substr(0, l.size() - 2));
            if (opens) l = trim(l.substr(l.find("/*") + 2));
            while (!l.empty() && l[0] == '*') l = trim(l.substr(1));
            out.push_back(l);
            if (opens) break;
        }
    } else {
        for (; i >= 0; --i) {
            std::string l = trim(lines[static_cast<size_t>(i)]);
            if (l.rfind("//", 0) != 0) break;
            l = l.substr(2);
            if (!l.empty() && l[0] == ' ') l = l.substr(1);
            out.push_back(l);
        }
    }
    std::reverse(out.begin(), out.end());
    while (!out.empty() && out.front().empty()) out.erase(out.begin());
    while (!out.empty() && out.back().empty()) out.pop_back();
    std::string r;
    for (size_t k = 0; k < out.size(); ++k) r += (k ? "\n" : "") + out[k];
    return r;
}

static int identLength(const std::string& content, int off) {
    int n = 0;
    while (off + n < static_cast<int>(content.size())) {
        unsigned char c = static_cast<unsigned char>(content[static_cast<size_t>(off + n)]);
        if (!(std::isalnum(c) || c == '_' || c >= 0x80)) break;
        ++n;
    }
    return n;
}

static bool isTopLevelKind(SymbolKind k) {
    return k == SymbolKind::Function || k == SymbolKind::Struct ||
           k == SymbolKind::Enum || k == SymbolKind::Variable;
}

static bool isTopLevel(const Symbol* s) {
    return s->definitionLoc.isValid() && !s->isBuiltin && isTopLevelKind(s->kind) &&
           s->scope && s->scope->parent == nullptr;
}

IndexEntry buildIndexEntry(const std::string& path, const std::string& content,
                           ASTNode* ast, SymbolTable& table,
                           const std::vector<std::string>& deps) {
    IndexEntry e;
    e.path = path;
    e.hash = std::hash<std::string>{}(content);
    e.deps = deps;
    std::vector<int> starts = buildLineStarts(content);
    auto lineCol = [&](int off, int& line, int& col) {
        auto it = std::upper_bound(starts.begin(), starts.end(), off);
        int li = static_cast<int>(it - starts.begin()) - 1;
        if (li < 0) li = 0;
        line = li + 1;
        col  = off - starts[static_cast<size_t>(li)] + 1;
    };

    // AST: export/shared bayrakları ve import bildirimleri.
    std::map<std::string, std::pair<bool, bool>> flags;   // ad → (exported, shared)
    if (ast) {
        for (ASTNode* c : ast->getChildren()) {
            switch (c->kind) {
                case ASTKind::FunctionDecl: {
                    auto* f = static_cast<FunctionDeclNode*>(c);
                    flags[f->name] = {f->isExported, false};
                    break;
                }
                case ASTKind::StructDecl: {
                    auto* s = static_cast<StructDeclNode*>(c);
                    flags[s->name] = {s->isExported, false};
                    break;
                }
                case ASTKind::EnumDecl: {
                    auto* en = static_cast<EnumDeclNode*>(c);
                    flags[en->name] = {en->isExported, false};
                    break;
                }
                case ASTKind::VariableDecl: {
                    auto* v = static_cast<VariableDeclNode*>(c);
                    flags[v->name] = {v->isExported, v->isShared};
                    break;
                }
                case ASTKind::ImportDecl: {
                    auto* imp = static_cast<ImportDeclNode*>(c);
                    IndexImport ii;
                    if (imp->isModuleName) ii.module = imp->sourcePath;
                    else ii.resolvedPath = imp->resolvedPath;
                    for (auto& n : imp->importedNames)
                        ii.names.push_back({n.source, n.local.empty() ? n.source : n.local});
                    ii.declOffset = imp->loc.offset;
                    size_t semi = content.find(';', static_cast<size_t>(std::max(0, ii.declOffset)));
                    ii.endOffset = semi == std::string::npos ? -1 : static_cast<int>(semi);
                    size_t close = content.find('}', static_cast<size_t>(std::max(0, ii.declOffset)));
                    if (close != std::string::npos && (semi == std::string::npos || close < semi))
                        ii.closeBrace = static_cast<int>(close);
                    e.imports.push_back(std::move(ii));
                    break;
                }
                default: break;
            }
        }
    }

    for (Symbol* s : table.allSymbols()) {
        if (!isTopLevel(s)) continue;
        const std::string name = displayName(s->name);

        if (s->definitionLoc.filePath() == path) {
            IndexSymbol is;
            is.name       = name;
            is.kind       = s->kind;
            auto fl = flags.find(name);
            is.exported   = fl != flags.end() && fl->second.first;
            is.shared     = s->isShared;
            is.signature  = symbolSignature(s);
            is.declOffset = s->definitionLoc.offset;
            int ident = identOffsetFromDecl(content, s->definitionLoc.offset, name);
            is.offset = ident >= 0 ? ident : s->definitionLoc.offset;
            lineCol(is.offset, is.line, is.col);
            is.doc = docCommentAbove(content, s->definitionLoc.line);
            e.symbols.push_back(std::move(is));
        }

        for (const auto& ref : s->references) {
            if (ref.filePath() != path || ref.offset < 0) continue;
            IndexRef r;
            r.targetFile = s->definitionLoc.filePath();
            r.targetName = name;
            r.offset     = ref.offset;
            r.length     = identLength(content, ref.offset);
            lineCol(ref.offset, r.line, r.col);
            e.refs.push_back(std::move(r));
        }
    }
    std::sort(e.symbols.begin(), e.symbols.end(),
              [](const IndexSymbol& a, const IndexSymbol& b) { return a.offset < b.offset; });
    std::sort(e.refs.begin(), e.refs.end(),
              [](const IndexRef& a, const IndexRef& b) { return a.offset < b.offset; });
    return e;
}

// ─────────────────────────────────────────────────────────────────────────────
// ProjectIndex
// ─────────────────────────────────────────────────────────────────────────────

void ProjectIndex::configure(std::vector<std::string> roots, std::vector<std::string> excludes) {
    roots_.clear();
    for (auto& r : roots) {
        std::error_code ec;
        fs::path p = fs::weakly_canonical(r, ec);
        if (!ec && fs::is_directory(p, ec)) roots_.push_back(p.string());
    }
    excludes_ = std::move(excludes);
}

bool ProjectIndex::excluded(const std::string& path) const {
    for (const auto& root : roots_) {
        std::error_code ec;
        std::string rel = fs::relative(path, root, ec).generic_string();
        if (ec || rel.empty() || rel.rfind("..", 0) == 0) continue;
        for (const auto& pat : excludes_)
            if (globMatch(pat, rel)) return true;
    }
    return false;
}

void ProjectIndex::scheduleFullScan() {
    std::vector<std::string> files;
    for (const auto& root : roots_) {
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            const fs::path& p = it->path();
            const std::string name = p.filename().string();
            std::error_code ec2;
            if (it->is_directory(ec2)) {
                if (name == ".git" || name == "node_modules" || name.rfind("build", 0) == 0 ||
                    excluded(p.string()))
                    it.disable_recursion_pending();
                continue;
            }
            if (p.extension() != ".sqt" || excluded(p.string())) continue;
            files.push_back(fs::weakly_canonical(p, ec2).string());
        }
    }
    std::sort(files.begin(), files.end());
    for (auto& f : files)
        if (pendingSet_.insert(f).second) pending_.push_back(f);
    scanned_ = true;
}

void ProjectIndex::step(const Overlay& overlay) {
    if (pending_.empty()) return;
    std::string path = pending_.front();
    pending_.pop_front();
    pendingSet_.erase(path);
    indexFile(path, overlay);
}

void ProjectIndex::indexFile(const std::string& path, const Overlay& overlay) {
    auto existing = entries_.find(path);
    if (existing != entries_.end() && existing->second.fromOpenDocument) return;   // analiz güncel tutar

    std::string content;
    if (!(overlay && overlay(path, content))) content = readFile(path);
    if (existing != entries_.end() &&
        existing->second.hash == std::hash<std::string>{}(content))
        return;   // değişmemiş — önbellek

    ModuleRegistry   registry;
    DiagnosticEngine diag;
    // Derinlik 1: dosyanın kendi sembolleri + doğrudan import'larının
    // export'ları yeter. Sınırsız yükleme bir import zincirinde her dosya
    // için tüm zinciri yeniden parse eder (200 dosyalık zincirde O(N²)).
    ModuleLoader loader(registry, diag, overlay);
    loader.setMaxDepth(1);
    ModuleGraph graph = loader.load(path);
    if (graph.units.empty()) { entries_.erase(path); ++version_; return; }
    SymbolTable table;
    SymbolCollector(table, diag).collectModuleGraph(graph);
    std::vector<std::string> deps;
    for (auto& u : graph.units) deps.push_back(u.filePath);
    const std::string key = graph.units[0].filePath;
    entries_[key] = buildIndexEntry(key, content, graph.units[0].ast, table, deps);
    ++version_;
}

void ProjectIndex::markDirty(const std::string& path) {
    auto it = entries_.find(path);
    if (it != entries_.end()) {
        it->second.fromOpenDocument = false;
        it->second.hash = 0;   // bir sonraki indekslemede yeniden oku
    }
    if (pendingSet_.insert(path).second) pending_.push_back(path);
}

void ProjectIndex::remove(const std::string& path) {
    entries_.erase(path);
    ++version_;
    if (pendingSet_.erase(path))
        pending_.erase(std::remove(pending_.begin(), pending_.end(), path), pending_.end());
}

void ProjectIndex::updateFromAnalysis(const std::string& path, const std::string& content,
                                      ASTNode* ast, SymbolTable& table,
                                      const std::vector<std::string>& deps) {
    IndexEntry e = buildIndexEntry(path, content, ast, table, deps);
    e.fromOpenDocument = true;
    entries_[path] = std::move(e);
    ++version_;
}

const IndexEntry* ProjectIndex::ensure(const std::string& path, const Overlay& overlay) {
    if (!get(path)) indexFile(path, overlay);
    return get(path);
}

const IndexEntry* ProjectIndex::get(const std::string& path) const {
    auto it = entries_.find(path);
    return it == entries_.end() ? nullptr : &it->second;
}

std::vector<std::pair<std::string, const IndexRef*>>
ProjectIndex::refsTo(const std::string& file, const std::string& name) const {
    if (reverseVersion_ != version_) {
        reverse_.clear();
        for (const auto& [p, e] : entries_)
            for (const auto& r : e.refs)
                reverse_[r.targetFile + '\n' + r.targetName].push_back({p, &r});
        reverseVersion_ = version_;
    }
    auto it = reverse_.find(file + '\n' + name);
    if (it == reverse_.end()) return {};
    return it->second;
}
