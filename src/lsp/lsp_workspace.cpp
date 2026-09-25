// ============================================================================
// saQut LSP — Çalışma alanı özellikleri (Bölüm 2/3, docs/lsp-decisions.md)
// ============================================================================
//
// - Bağımlılık grafiği: bir dosya değişince/kapanınca, modül grafiğinde onu
//   içeren açık belgeler yeniden analiz edilir; tanıları değiştiyse yeniden
//   yayımlanır.
// - Proje indeksi (project_index.hpp): workspace/symbol, dosyalar arası
//   references/rename, referans sayısı lens'i ve otomatik import tamamlaması.
// ============================================================================

#include "lsp/lsp_handler.hpp"
#include "lsp/lsp_analysis.hpp"
#include "lsp/position.hpp"
#include "lsp/uri.hpp"
#include "parser/nodes/declarations.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

// LSP SymbolKind: Function=12, Variable=13, Struct=23, Enum=10
static int lspKindOf(SymbolKind k) {
    switch (k) {
        case SymbolKind::Function: return 12;
        case SymbolKind::Struct:   return 23;
        case SymbolKind::Enum:     return 10;
        default:                   return 13;
    }
}

// CompletionItemKind: Function=3, Variable=6, Enum=13, Struct=22
static int completionKindOf(SymbolKind k) {
    switch (k) {
        case SymbolKind::Function: return 3;
        case SymbolKind::Struct:   return 22;
        case SymbolKind::Enum:     return 13;
        default:                   return 6;
    }
}

ModuleLoader::SourceOverlay LspHandler::overlay() {
    return [this](const std::string& path, std::string& out) -> bool {
        return store_.openContent(path, out);
    };
}

void LspHandler::idleStep() {
    index_.step(overlay());
    // İlk tarama (ya da bir değişiklik turu) bitti: istemci destekliyorsa
    // referans sayısı lens'lerini yeniden istemesini söyle.
    if (!index_.hasPending() && codeLensRefresh_) {
        JsonRpc::writeMessage(out_, {
            {"jsonrpc", "2.0"},
            {"id", "saqut-" + std::to_string(++serverRequestSeq_)},
            {"method", "workspace/codeLens/refresh"}
        });
    }
}

void LspHandler::afterAnalysis(DocumentState& state) {
    if (state.ast && !state.filePath.empty())
        index_.updateFromAnalysis(state.filePath, state.content, state.ast,
                                  state.symbolTable, state.deps);
    reanalyzeDependents(state.filePath, &state);
}

void LspHandler::reanalyzeDependents(const std::string& path, const DocumentState* except) {
    if (path.empty()) return;
    for (DocumentState* dep : store_.dependentsOf(path, except)) {
        store_.reanalyze(*dep);
        publishDiagnosticsGrouped(*dep, /*onlyIfChanged=*/true);
        if (dep->ast && !dep->filePath.empty())
            index_.updateFromAnalysis(dep->filePath, dep->content, dep->ast,
                                      dep->symbolTable, dep->deps);
    }
}

void LspHandler::applySettings(const nlohmann::json& s) {
    if (!s.is_object()) return;
    if (s.contains("inlayHints") && s["inlayHints"].is_object())
        inlayParamNames_ = s["inlayHints"].value("parameterNames", inlayParamNames_);
}

void LspHandler::handleDidChangeConfiguration(const nlohmann::json& params) {
    if (!params.contains("settings") || !params["settings"].is_object()) return;
    const auto& settings = params["settings"];
    // İstemci ayarları ya doğrudan ya da "saqut" bölümü altında gönderir.
    applySettings(settings.contains("saqut") ? settings["saqut"] : settings);
}

void LspHandler::handleDidChangeWatchedFiles(const nlohmann::json& params) {
    if (!params.contains("changes") || !params["changes"].is_array()) return;
    std::vector<std::string> changed;
    for (const auto& c : params["changes"]) {
        if (!c.is_object() || !c.contains("uri") || !c["uri"].is_string()) continue;
        std::error_code ec;
        std::string path = fs::weakly_canonical(uriToPath(c["uri"].get<std::string>()), ec).string();
        if (ec || path.empty()) continue;
        if (store_.byPath(path)) continue;   // açık belge: editör içeriği esastır
        if (c.value("type", 2) == 3) index_.remove(path);   // Deleted
        else index_.markDirty(path);                         // Created / Changed
        changed.push_back(path);
    }
    if (syncIndex_) index_.runAll(overlay());
    for (const auto& p : changed) reanalyzeDependents(p, nullptr);
}

// ── Konum yardımcıları ──────────────────────────────────────────────────────

namespace {
struct FileText {
    std::string      content;
    std::vector<int> starts;
};
}  // namespace

static nlohmann::json lspRange(const FileText& ft, int line1, int col1, int length,
                               const std::string& encoding) {
    const int line = line1 - 1;
    int startCh, endCh;
    if (encoding == "utf-8") {
        startCh = col1 - 1;
        endCh   = startCh + length;
    } else {
        startCh = byteColToLspAt(ft.content, ft.starts, line, col1);
        endCh   = byteColToLspAt(ft.content, ft.starts, line, col1 + length);
    }
    return {{"start", {{"line", line}, {"character", startCh}}},
            {"end",   {{"line", line}, {"character", endCh}}}};
}

std::vector<std::tuple<std::string, int, int>>
LspHandler::projectReferenceOffsets(const std::string& file, const std::string& name) {
    std::vector<std::tuple<std::string, int, int>> out;
    for (const auto& [path, ref] : index_.refsTo(file, name))
        out.emplace_back(path, ref->offset, ref->length);
    return out;
}

nlohmann::json LspHandler::projectReferenceLocations(
        const std::string& file, const std::string& name,
        const std::set<std::pair<std::string, int>>& skip) {
    nlohmann::json locs = nlohmann::json::array();
    std::map<std::string, FileText> cache;
    for (const auto& [path, ref] : index_.refsTo(file, name)) {
        if (skip.count({path, ref->offset})) continue;
        auto it = cache.find(path);
        if (it == cache.end()) {
            FileText ft;
            ft.content = store_.contentForPath(path);
            ft.starts  = buildLineStarts(ft.content);
            it = cache.emplace(path, std::move(ft)).first;
        }
        locs.push_back({{"uri", store_.uriForPath(path)},
                        {"range", lspRange(it->second, ref->line, ref->col, ref->length,
                                           positionEncoding_)}});
    }
    return locs;
}

// ── workspace/symbol ────────────────────────────────────────────────────────

// Büyük/küçük harf duyarsız alt dizi eşleşmesi ("tpl" → "topla").
static bool fuzzyMatch(const std::string& query, const std::string& name) {
    size_t qi = 0;
    for (size_t i = 0; i < name.size() && qi < query.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(name[i])) ==
            std::tolower(static_cast<unsigned char>(query[qi]))) ++qi;
    return qi == query.size();
}

nlohmann::json LspHandler::handleWorkspaceSymbol(const nlohmann::json& id,
                                                 const nlohmann::json& params) {
    const std::string query = params.value("query", "");
    nlohmann::json out = nlohmann::json::array();
    std::map<std::string, FileText> cache;
    for (const auto& [path, entry] : index_.entries()) {
        for (const auto& s : entry.symbols) {
            if (!fuzzyMatch(query, s.name)) continue;
            auto it = cache.find(path);
            if (it == cache.end()) {
                FileText ft;
                ft.content = store_.contentForPath(path);
                ft.starts  = buildLineStarts(ft.content);
                it = cache.emplace(path, std::move(ft)).first;
            }
            const std::string container = fs::path(path).filename().string();
            out.push_back({
                {"name", s.name},
                {"kind", lspKindOf(s.kind)},
                {"containerName", container},
                {"location", {{"uri", store_.uriForPath(path)},
                              {"range", lspRange(it->second, s.line, s.col,
                                                 static_cast<int>(s.name.size()),
                                                 positionEncoding_)}}}
            });
            if (out.size() >= 1000) return JsonRpc::makeResponse(id, out);
        }
    }
    return JsonRpc::makeResponse(id, out);
}

// ── Otomatik import ─────────────────────────────────────────────────────────

nlohmann::json LspHandler::importEditFor(DocumentState& state, const std::string& targetFile,
                                         const std::string& name) {
    if (!state.ast || targetFile == state.filePath) return nullptr;
    auto posOf = [&](int off) {
        auto it = std::upper_bound(state.lineStarts.begin(), state.lineStarts.end(), off);
        int li = std::max(0, static_cast<int>(it - state.lineStarts.begin()) - 1);
        int colByte = off - state.lineStarts[static_cast<size_t>(li)] + 1;
        int ch = positionEncoding_ == "utf-8"
            ? colByte - 1
            : byteColToLspAt(state.content, state.lineStarts, li, colByte);
        return nlohmann::json{{"line", li}, {"character", ch}};
    };

    int lastImportEnd = -1;
    for (ASTNode* c : state.ast->getChildren()) {
        if (c->kind != ASTKind::ImportDecl) continue;
        auto* imp = static_cast<ImportDeclNode*>(c);
        size_t semi = state.content.find(';', static_cast<size_t>(std::max(0, imp->loc.offset)));
        if (semi != std::string::npos) lastImportEnd = std::max(lastImportEnd, static_cast<int>(semi));
        if (imp->isModuleName || imp->resolvedPath != targetFile) continue;
        for (auto& n : imp->importedNames)
            if (n.source == name) return nullptr;   // zaten import edilmiş
        // Mevcut `import { a } from "x.sqt"` bloğuna ekle: `}`'dan önce ", ad".
        size_t close = state.content.find('}', static_cast<size_t>(std::max(0, imp->loc.offset)));
        if (close == std::string::npos || (semi != std::string::npos && close > semi)) return nullptr;
        size_t ins = close;
        while (ins > 0 && (state.content[ins - 1] == ' ' || state.content[ins - 1] == '\t')) --ins;
        bool empty = state.content[ins - 1] == '{';
        auto p = posOf(static_cast<int>(ins));
        return {{"range", {{"start", p}, {"end", p}}},
                {"newText", (empty ? "" : ", ") + name}};
    }

    std::error_code ec;
    std::string rel = fs::path(targetFile)
        .lexically_relative(fs::path(state.filePath).parent_path()).generic_string();
    if (rel.empty()) rel = fs::path(targetFile).filename().string();
    const std::string text = "import {" + name + "} from \"" + rel + "\";\n";
    nlohmann::json p;
    if (lastImportEnd >= 0) {
        // Son import bildiriminin bittiği satırın ardına.
        auto it = std::upper_bound(state.lineStarts.begin(), state.lineStarts.end(), lastImportEnd);
        int nextLine = static_cast<int>(it - state.lineStarts.begin());
        p = {{"line", nextLine}, {"character", 0}};
    } else {
        p = {{"line", 0}, {"character", 0}};
    }
    return {{"range", {{"start", p}, {"end", p}}}, {"newText", text}};
}

nlohmann::json LspHandler::importableNamesFromFile(DocumentState& state, const std::string& rawPath) {
    nlohmann::json items = nlohmann::json::array();
    std::string raw = rawPath;
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') raw = raw.substr(1, raw.size() - 2);
    if (raw.empty() || state.filePath.empty()) return items;
    std::error_code ec;
    std::string path = fs::weakly_canonical(fs::path(state.filePath).parent_path() / raw, ec).string();
    if (ec) return items;
    const IndexEntry* e = index_.ensure(path, overlay());
    if (!e) return items;
    for (const auto& s : e->symbols) {
        if (!s.exported) continue;
        nlohmann::json it = {{"label", s.name}, {"kind", completionKindOf(s.kind)},
                             {"detail", s.signature}};
        if (!s.doc.empty()) it["documentation"] = s.doc;
        items.push_back(it);
    }
    return items;
}

void LspHandler::appendProjectCompletions(DocumentState& state, const std::string& prefix,
                                          const std::set<std::string>& seen, nlohmann::json& items) {
    if (prefix.empty()) return;
    for (const auto& [path, entry] : index_.entries()) {
        if (path == state.filePath) continue;
        for (const auto& s : entry.symbols) {
            if (!s.exported || s.name.rfind(prefix, 0) != 0 || seen.count(s.name)) continue;
            nlohmann::json edit = importEditFor(state, path, s.name);
            if (edit.is_null()) continue;
            std::string rel = fs::path(path)
                .lexically_relative(fs::path(state.filePath).parent_path()).generic_string();
            nlohmann::json it = {
                {"label", s.name},
                {"kind", completionKindOf(s.kind)},
                {"detail", s.signature},
                {"labelDetails", {{"description", rel}}},
                {"sortText", "4_" + s.name},
                {"additionalTextEdits", nlohmann::json::array({edit})}
            };
            if (!s.doc.empty()) it["documentation"] = s.doc;
            items.push_back(it);
        }
    }
}
