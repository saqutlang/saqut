#include "lsp/lsp_handler.hpp"
#include "ffi/ffi_catalog.hpp"
#include "symbol/symbol_table.hpp"
#include "symbol/symbol.hpp"
#include "core/type.hpp"
#include "lsp/uri.hpp"
#include "lsp/position.hpp"
#include "data/data_registry.hpp"
#include "parser/nodes/binary_expr.hpp"
#include "parser/nodes/declarations.hpp"
#include "parser/nodes/expressions.hpp"
#include "parser/nodes/statements.hpp"
#include <algorithm>
#include <cctype>
#include <map>

// ─────────────────────────────────────────────────────────────────────────────
// dispatch — gelen JSON-RPC mesajını yönlendir
// ─────────────────────────────────────────────────────────────────────────────

nlohmann::json LspHandler::dispatch(const nlohmann::json& msg) {
    if (msg.is_discarded() || !msg.is_object()) return nullptr;

    nlohmann::json id = msg.value("id", nlohmann::json(nullptr));

    // Faz 6 (#84): method eksik ya da string değilse (bozuk istemci) çökme —
    // istek ise InvalidRequest dön, notification ise sessizce at.
    if (!msg.contains("method") || !msg["method"].is_string()) {
        if (!id.is_null())
            return JsonRpc::makeError(id, -32600, "Invalid request: missing method");
        return nullptr;
    }

    std::string method = msg["method"].get<std::string>();
    nlohmann::json params = msg.value("params", nlohmann::json::object());

    // Faz 6 (#84): params doğrulaması. nlohmann::json'un CONST operator[]'ı
    // eksik anahtarda assert/abort eder (istisna DEĞİL — try/catch yakalamaz);
    // bozuk istemci mesajı sunucuyu düşürmesin diye handler'lara girmeden
    // burada doğrula: istek ise InvalidParams dön, notification ise at.
    if (method.rfind("textDocument/", 0) == 0) {
        auto invalidParams = [&]() -> nlohmann::json {
            if (!id.is_null())
                return JsonRpc::makeError(id, -32602, "Invalid params: " + method);
            return nullptr;
        };
        if (!params.contains("textDocument") ||
            !params["textDocument"].contains("uri") ||
            !params["textDocument"]["uri"].is_string())
            return invalidParams();

        static const std::vector<std::string> positional = {
            "textDocument/definition",        "textDocument/hover",
            "textDocument/references",        "textDocument/documentHighlight",
            "textDocument/completion",        "textDocument/rename",
            "textDocument/signatureHelp",
        };
        bool needsPos = std::find(positional.begin(), positional.end(),
                                  method) != positional.end();
        if (needsPos && (!params.contains("position") ||
                         !params["position"].contains("line") ||
                         !params["position"]["line"].is_number_integer() ||
                         !params["position"].contains("character") ||
                         !params["position"]["character"].is_number_integer()))
            return invalidParams();

        if (method == "textDocument/rename" &&
            (!params.contains("newName") || !params["newName"].is_string()))
            return invalidParams();

        if (method == "textDocument/didOpen" &&
            (!params["textDocument"].contains("text") ||
             !params["textDocument"]["text"].is_string()))
            return nullptr; // notification — sessizce at
    }

    if (method == "initialize")
        return handleInitialize(id, params);

    if (method == "initialized")
        return nullptr; // notification, cevap yok

    if (method == "shutdown") {
        shutdownRequested_ = true;
        return JsonRpc::makeResponse(id, nullptr);
    }

    if (method == "exit") {
        std::exit(shutdownRequested_ ? 0 : 1);
    }

    if (method == "textDocument/didOpen") {
        handleDidOpen(params);
        return nullptr;
    }
    if (method == "textDocument/didChange") {
        handleDidChange(params);
        return nullptr;
    }
    if (method == "textDocument/didClose") {
        handleDidClose(params);
        return nullptr;
    }

    if (method == "textDocument/definition")
        return handleDefinition(id, params);

    if (method == "textDocument/hover")
        return handleHover(id, params);

    if (method == "textDocument/references")
        return handleReferences(id, params);

    if (method == "textDocument/documentSymbol")
        return handleDocumentSymbol(id, params);

    if (method == "textDocument/documentHighlight")
        return handleDocumentHighlight(id, params);

    if (method == "textDocument/completion")
        return handleCompletion(id, params);

    if (method == "textDocument/rename")
        return handleRename(id, params);

    if (method == "textDocument/signatureHelp")
        return handleSignatureHelp(id, params);

    if (method == "textDocument/semanticTokens/full")
        return handleSemanticTokens(id, params);

    // Bilinmeyen metod — null döndür (notification) veya boş cevap
    if (!id.is_null())
        return JsonRpc::makeError(id, -32601, "Method not found: " + method);
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tier 0 — Zorunlu metodlar
// ─────────────────────────────────────────────────────────────────────────────

nlohmann::json LspHandler::handleInitialize(const nlohmann::json& id,
                                             const nlohmann::json& params) {
    // Faz 3: konum birimi anlaşması. LSP varsayılanı UTF-16'dır; istemci
    // general.positionEncodings'te "utf-8" listeliyorsa onu seçiyoruz —
    // SourceLocation.column zaten byte/UTF-8 code unit saydığı için bu
    // durumda hiç dönüşüm gerekmez (src/lsp/position.hpp devre dışı kalır).
    positionEncoding_ = "utf-16";
    if (params.contains("capabilities") && params["capabilities"].contains("general")) {
        auto& general = params["capabilities"]["general"];
        if (general.contains("positionEncodings") && general["positionEncodings"].is_array()) {
            for (auto& enc : general["positionEncodings"]) {
                if (enc.is_string() && enc.get<std::string>() == "utf-8") {
                    positionEncoding_ = "utf-8";
                    break;
                }
            }
        }
    }

    nlohmann::json capabilities = {
        {"textDocumentSync", 1},
        {"positionEncoding", positionEncoding_},
        {"definitionProvider",        true},
        {"referencesProvider",        true},
        {"hoverProvider",             true},
        {"documentSymbolProvider",    true},
        {"documentHighlightProvider", true},
        {"completionProvider", {
            {"triggerCharacters", nlohmann::json::array({":", "."})}
        }},
        {"renameProvider",            true},
        {"signatureHelpProvider", {
            {"triggerCharacters", nlohmann::json::array({"(", ","})}
        }},
        {"semanticTokensProvider", {
            {"legend", {
                {"tokenTypes", {
                    "keyword", "type", "function", "builtin",
                    "parameter", "variable", "string", "number"
                }},
                {"tokenModifiers", nlohmann::json::array()}
            }},
            {"full", true}
        }},
    };
    nlohmann::json result = {
        {"capabilities", capabilities},
        {"serverInfo",   {{"name", "saQut"}, {"version", SAQUT_VERSION}}}
    };
    return JsonRpc::makeResponse(id, result);
}

void LspHandler::handleDidOpen(const nlohmann::json& params) {
    auto& doc = params["textDocument"];
    std::string uri     = doc["uri"].get<std::string>();
    std::string content = doc["text"].get<std::string>();
    int         version = doc.value("version", 0);

    DocumentState& state = store_.update(uri, content, version);
    publishDiagnosticsGrouped(state);
}

void LspHandler::handleDidChange(const nlohmann::json& params) {
    std::string uri     = params["textDocument"]["uri"].get<std::string>();
    int         version = params["textDocument"].value("version", 0);
    std::string content;

    if (params.contains("contentChanges") && params["contentChanges"].is_array() &&
        !params["contentChanges"].empty()) {
        // Faz 6 (#84): "text" alanı eksikse const operator[] abort eder — value() kullan.
        const auto& last = params["contentChanges"].back();
        if (last.is_object()) content = last.value("text", "");
    }

    DocumentState& state = store_.update(uri, content, version);
    publishDiagnosticsGrouped(state);
}

void LspHandler::handleDidClose(const nlohmann::json& params) {
    std::string uri = params["textDocument"]["uri"].get<std::string>();
    store_.close(uri);
    // Kapalı belgeden tanılamaları temizle
    auto notif = JsonRpc::makeNotification("textDocument/publishDiagnostics", {
        {"uri", uri}, {"diagnostics", nlohmann::json::array()}
    });
    JsonRpc::writeMessage(out_, notif);
}

// Faz 3: state.diagnostics artık modül grafiğindeki TÜM dosyalardan gelen
// tanıları içerebilir (bir import edilen modülün hatası da burada olabilir).
// Eskiden hepsi sorgulanan `uri`'ye basılıyordu (kök neden #4 — import edilen
// modülün hatası ana dosyada görünüyordu). Şimdi loc.filePath'e göre gruplayıp
// her dosya için ayrı publishDiagnostics gönderiyoruz. std::map (sıralı) —
// bildirim SIRASI testte önemli, unordered_map olsaydı çalıştırmalar arası
// deterministik olmazdı.
void LspHandler::publishDiagnosticsGrouped(DocumentState& state) {
    std::map<std::string, nlohmann::json> byFile;
    byFile[state.filePath] = nlohmann::json::array(); // sorgulanan dosya her zaman bir bildirim alır (stale temizliği)

    // Tanı sayısı büyük olabilir (üretilmiş dosyada binlerce W) → dosya
    // başına içerik + satır indeksini bir kez kur.
    std::map<std::string, std::pair<std::string, std::vector<int>>> fileCache;
    for (const auto& d : state.diagnostics.all()) {
        // Konumsuz tanılar (ör. E_MODULE_NOT_FOUND — SourceLocation{} boş
        // filePath'le gelir) sorgulanan dosyaya düşer; eski davranışla aynı.
        std::string fp = d.loc.filePath().empty() ? state.filePath : d.loc.filePath();
        const std::string* content;
        const std::vector<int>* starts;
        if (fp == state.filePath) {
            content = &state.content;
            starts  = &state.lineStarts;
        } else {
            auto it = fileCache.find(fp);
            if (it == fileCache.end()) {
                std::string c = store_.contentForPath(fp);
                it = fileCache.emplace(fp,
                        std::make_pair(std::move(c), std::vector<int>{})).first;
                it->second.second = buildLineStarts(it->second.first);
            }
            content = &it->second.first;
            starts  = &it->second.second;
        }
        LspPosition pos = toLspPos(*content, *starts, d.loc);

        // Geçici UX kuralı: AST düğümleri henüz güvenilir end-offset taşımadığı
        // için tek karakterlik tanı yerine ilgili satırın tamamını vurgula.
        // Özellikle `return void` ve `b as int` hatalarında tek karakter
        // seçmek kullanıcıya yanlış konum hissi veriyordu.
        int lineEndOffset = static_cast<int>(content->size());
        if (pos.line + 1 < static_cast<int>(starts->size()))
            lineEndOffset = (*starts)[static_cast<size_t>(pos.line + 1)];
        int lineStartOffset = (*starts)[static_cast<size_t>(std::max(0, pos.line))];
        if (lineEndOffset > lineStartOffset && (*content)[lineEndOffset - 1] == '\n')
            --lineEndOffset;
        std::string lineText = content->substr(static_cast<size_t>(lineStartOffset),
                                               static_cast<size_t>(std::max(lineStartOffset,
                                                                            lineEndOffset) - lineStartOffset));
        int lineCharacterEnd = positionEncoding_ == "utf-16"
            ? byteOffsetToUtf16(lineText, static_cast<int>(lineText.size()))
            : static_cast<int>(lineText.size());

        // Aralık tanının işaret ettiği token'dır: başlangıç + tokenLength
        // (satır sonuna kırpılır). Eskiden bütün satır vurgulanıyordu; editör
        // hatanın yerini göstermiyor, golden'lar da bu yüzden kırmızıydı.
        const int startChar = std::min(std::max(0, pos.character), lineCharacterEnd);
        const int endChar = std::min(lineCharacterEnd, startChar + std::max(1, d.tokenLength));
        nlohmann::json item;
        item["range"] = {
            {"start", {{"line", pos.line}, {"character", startChar}}},
            {"end",   {{"line", pos.line}, {"character", std::max(endChar, startChar)}}}
        };
        item["severity"] = (d.level == DiagLevel::Error) ? 1 : 2;
        item["code"]     = d.code;
        item["message"]  = d.hint.empty() ? d.message : d.message + "\n" + d.hint;
        item["source"]   = "saQut";

        byFile[fp].push_back(item);
    }

    for (auto& [fp, diagsJson] : byFile) {
        auto notif = JsonRpc::makeNotification("textDocument/publishDiagnostics", {
            {"uri",         store_.uriForPath(fp)},
            {"diagnostics", diagsJson}
        });
        JsonRpc::writeMessage(out_, notif);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Faz 3 — pozisyon dönüşümü yardımcıları
// ─────────────────────────────────────────────────────────────────────────────

int LspHandler::toByteColumn(const std::string& content, int line, int character) const {
    if (positionEncoding_ == "utf-8") return character + 1; // zaten byte birimi
    return lspToByteCol(content, line, character);
}

LspPosition LspHandler::toLspPos(const std::string& content, const SourceLocation& loc) const {
    if (!loc.isValid()) return {0, 0};
    if (positionEncoding_ == "utf-8" || content.empty())
        return loc.toLspPosition(); // byte==utf-8-birim; içerik yoksa en iyi çaba
    return { loc.line - 1, byteColToLsp(content, loc.line - 1, loc.column) };
}

LspPosition LspHandler::toLspPos(const std::string& content,
                                 const std::vector<int>& lineStarts,
                                 const SourceLocation& loc) const {
    if (!loc.isValid()) return {0, 0};
    if (positionEncoding_ == "utf-8" || content.empty())
        return loc.toLspPosition();
    return { loc.line - 1, byteColToLspAt(content, lineStarts, loc.line - 1, loc.column) };
}

std::string LspHandler::contentForLoc(DocumentState& state, const SourceLocation& loc) const {
    if (loc.filePath() == state.filePath) return state.content;
    return store_.contentForPath(loc.filePath());
}

// ─────────────────────────────────────────────────────────────────────────────
// Tier 1 — Sembol bilgisi metodları
// ─────────────────────────────────────────────────────────────────────────────

// Faz 3: token binary search + (offset→Symbol*) indeksi (kök neden #3).
// 1) İstemci pozisyonunu (utf-16 veya utf-8) bu belgenin byte offset'ine çevir.
// 2) O offset'i kapsayan token'ı state.tokens'ta binary search ile bul.
// 3) Token identifier değilse (keyword/operator/...) sembol yok.
// 4) Token'ın başlangıç offset'i state.symbolByOffset'te varsa, scope-doğru
//    çözülmüş sembolü döndür (runPipeline'da SymbolCollector'ın resolvedSymbol
//    ataması sırasında toplanan referans/definition offsetlerinden kurulur).
Token* LspHandler::identifierTokenAt(DocumentState& state, int line, int character) const {
    int byteCol = toByteColumn(state.content, line, character);
    int offset  = lspLineStartOffset(state.content, line) + (byteCol - 1);

    const auto& toks = state.tokens;
    auto it = std::upper_bound(toks.begin(), toks.end(), offset,
        [](int off, Token* t) { return off < t->start; });
    if (it == toks.begin()) return nullptr;
    Token* tok = *std::prev(it);
    if (offset < tok->start || offset >= tok->end) return nullptr;
    if (tok->gettype() != "identifier") return nullptr;
    return tok;
}

Symbol* LspHandler::findSymbolAt(DocumentState& state, int line, int character) {
    Token* tok = identifierTokenAt(state, line, character);
    if (!tok) return nullptr;

    auto found = state.symbolByOffset.find(tok->start);
    return (found != state.symbolByOffset.end()) ? found->second : nullptr;
}

nlohmann::json LspHandler::handleDefinition(const nlohmann::json& id,
                                             const nlohmann::json& params) {
    std::string uri  = params["textDocument"]["uri"].get<std::string>();
    int         line = params["position"]["line"].get<int>();
    int         ch   = params["position"]["character"].get<int>();

    DocumentState* state = store_.get(uri);
    if (!state) return JsonRpc::makeResponse(id, nullptr);

    Symbol* sym = findSymbolAt(*state, line, ch);
    if (!sym || !sym->definitionLoc.isValid())
        return JsonRpc::makeResponse(id, nullptr);

    // Faz 3: tanım sorgulanan dosyada olmayabilir (import edilen sembol) —
    // artık HER ZAMAN sorgulanan URI değil, sym->definitionLoc.filePath'in
    // gerçek URI'si döner (kök neden #4).
    std::string targetContent = contentForLoc(*state, sym->definitionLoc);
    LspPosition pos = toLspPos(targetContent, sym->definitionLoc);
    nlohmann::json result = {
        {"uri", store_.uriForPath(sym->definitionLoc.filePath())},
        {"range", {
            {"start", {{"line", pos.line}, {"character", pos.character}}},
            {"end",   {{"line", pos.line}, {"character", pos.character + (int)sym->name.size()}}}
        }}
    };
    return JsonRpc::makeResponse(id, result);
}

nlohmann::json LspHandler::handleHover(const nlohmann::json& id,
                                        const nlohmann::json& params) {
    std::string uri  = params["textDocument"]["uri"].get<std::string>();
    int         line = params["position"]["line"].get<int>();
    int         ch   = params["position"]["character"].get<int>();

    DocumentState* state = store_.get(uri);
    if (!state) return JsonRpc::makeResponse(id, nullptr);

    Symbol* sym = findSymbolAt(*state, line, ch);

    std::string content;
    if (!sym) {
        // Üye erişimi hover'ı: struct field'ları Symbol nesnesi DEĞİLDİR —
        // SymbolCollector bunları tabloya kaydetmez, structLayouts haritasında
        // yaşar (symbol_table.hpp::structLayouts). "s.top" gibi zincirleri
        // token'lar üzerinden yürütürüz: en soldaki identifier sembol
        // indeksinden çözülür, kalan alanlar derleyicinin getFieldType
        // API'siyle tipe indirgenir (örn. "int top").
        Token* memberTok = identifierTokenAt(*state, line, ch);
        if (memberTok) {
            const auto& toks = state->tokens;
            int idx = static_cast<int>(std::upper_bound(toks.begin(), toks.end(),
                memberTok->start, [](int off, Token* t) { return off < t->start; })
                - toks.begin()) - 1;
            if (idx >= 0 && toks[idx] == memberTok) {
                // Desen: ... ident . ident . ident — zincirin en soluna yürü.
                int root = idx;
                while (root >= 2 && toks[root - 1]->token == "." &&
                       toks[root - 2]->gettype() == "identifier")
                    root -= 2;
                if (root != idx) {
                    auto found = state->symbolByOffset.find(toks[root]->start);
                    if (found != state->symbolByOffset.end()) {
                        Type cur = found->second->type;
                        bool ok  = true;
                        for (int i = root + 2; i <= idx && ok; i += 2) {
                            if (!cur.isStruct()) { ok = false; break; }
                            cur = state->symbolTable.getFieldType(cur.structName,
                                                                  toks[i]->token);
                            if (cur.isError()) { ok = false; break; }
                        }
                        if (ok)
                            content = "```sqt\n" + cur.toString() + " " +
                                      memberTok->token + "\n```";
                    }
                }
            }
        }
        if (content.empty())
            return JsonRpc::makeResponse(id, nullptr);
    } else if (sym->kind == SymbolKind::Function && sym->type.isFunction()) {
        // "int gcd(int a, int b)"
        std::string ret = sym->type.returnType ? sym->type.returnType->toString() : "void";
        std::string sig = ret + " " + sym->name + "(";
        for (size_t i = 0; i < sym->type.paramTypes.size(); ++i) {
            if (i > 0) sig += ", ";
            sig += sym->type.paramTypes[i].toString();
            if (i < sym->paramNames.size())
                sig += " " + sym->paramNames[i];
        }
        sig += ")";
        content = "```sqt\n" + sig + "\n```";
    } else if (sym->kind == SymbolKind::Struct) {
        content = "```sqt\nstruct " + sym->name + "\n```";
    } else if (sym->kind == SymbolKind::Enum) {
        content = "```sqt\nenum " + sym->name + "\n```";
    } else {
        // değişken / parametre / alan
        std::string typeStr = sym->type.toString();
        content = "```sqt\n" + typeStr + " " + sym->name + "\n```";
    }

    nlohmann::json result = {
        {"contents", {{"kind", "markdown"}, {"value", content}}}
    };
    return JsonRpc::makeResponse(id, result);
}

nlohmann::json LspHandler::handleReferences(const nlohmann::json& id,
                                             const nlohmann::json& params) {
    std::string uri  = params["textDocument"]["uri"].get<std::string>();
    int         line = params["position"]["line"].get<int>();
    int         ch   = params["position"]["character"].get<int>();

    DocumentState* state = store_.get(uri);
    if (!state) return JsonRpc::makeResponse(id, nullptr);

    Symbol* sym = findSymbolAt(*state, line, ch);
    if (!sym) return JsonRpc::makeResponse(id, nlohmann::json::array());

    bool includeDecl = params.value("context", nlohmann::json::object())
                             .value("includeDeclaration", false);

    nlohmann::json locs = nlohmann::json::array();

    // Faz 3: her konum KENDİ dosyasının URI'siyle döner — sym->references
    // farklı dosyalardan gelebilir (bu sembolü import eden başka bir modül),
    // eskiden hepsi sorgulanan `uri`'ye kopyalanıyordu (kök neden #4).
    // Referans sayısı büyük olabilir → dosya başına satır indeksi bir kez kur.
    std::map<std::string, std::pair<std::string, std::vector<int>>> fileCache;
    auto addLoc = [&](const SourceLocation& loc) {
        if (!loc.isValid()) return;
        const std::string* content;
        const std::vector<int>* starts;
        if (loc.filePath() == state->filePath) {
            content = &state->content;
            starts  = &state->lineStarts;
        } else {
            auto it = fileCache.find(loc.filePath());
            if (it == fileCache.end()) {
                std::string c = store_.contentForPath(loc.filePath());
                it = fileCache.emplace(loc.filePath(),
                        std::make_pair(std::move(c), std::vector<int>{})).first;
                it->second.second = buildLineStarts(it->second.first);
            }
            content = &it->second.first;
            starts  = &it->second.second;
        }
        LspPosition pos = toLspPos(*content, *starts, loc);
        locs.push_back({
            {"uri", store_.uriForPath(loc.filePath())},
            {"range", {
                {"start", {{"line", pos.line}, {"character", pos.character}}},
                {"end",   {{"line", pos.line}, {"character", pos.character + (int)sym->name.size()}}}
            }}
        });
    };

    if (includeDecl) addLoc(sym->definitionLoc);
    for (const auto& ref : sym->references) addLoc(ref);

    return JsonRpc::makeResponse(id, locs);
}

// LSP SymbolKind sayıları: Function=12, Variable=13, Struct=23, Enum=10, EnumMember=22, Field=8
static int lspSymbolKind(SymbolKind k) {
    switch (k) {
        case SymbolKind::Function:   return 12;
        case SymbolKind::Struct:     return 23;
        case SymbolKind::Enum:       return 10;
        case SymbolKind::EnumValue:  return 22;
        case SymbolKind::Field:      return 8;
        case SymbolKind::Variable:   return 13;
        case SymbolKind::Parameter:  return 13;
    }
    return 13;
}

nlohmann::json LspHandler::handleDocumentSymbol(const nlohmann::json& id,
                                                 const nlohmann::json& params) {
    std::string uri = params["textDocument"]["uri"].get<std::string>();
    DocumentState* state = store_.get(uri);
    if (!state) return JsonRpc::makeResponse(id, nlohmann::json::array());

    nlohmann::json symbols = nlohmann::json::array();

    for (Symbol* sym : state->symbolTable.allSymbols()) {
        // Parametre ve alan sembollerini gizle — gürültü yapar
        if (sym->kind == SymbolKind::Parameter) continue;
        if (sym->kind == SymbolKind::Field)     continue;
        // Boş isimli sembol (syntax-error recovery'de oluşabilir) VS Code
        // istemcisinin DocumentSymbol dönüştürücüsünü düşürür ("name must not
        // be falsy") — outline'da da işe yaramaz, atla.
        if (sym->name.empty())                  continue;
        if (!sym->definitionLoc.isValid())      continue;
        // Faz 3: symbolTable tüm modül grafiğini kapsar (import edilen
        // dosyaların sembolleri de içinde) — yalnızca BU belgeye ait olanları
        // listele (kök neden #4).
        if (sym->definitionLoc.filePath() != state->filePath) continue;

        LspPosition pos = toLspPos(state->content, state->lineStarts, sym->definitionLoc);
        int  end = pos.character + static_cast<int>(sym->name.size());

        nlohmann::json range = {
            {"start", {{"line", pos.line}, {"character", pos.character}}},
            {"end",   {{"line", pos.line}, {"character", end}}}
        };

        symbols.push_back({
            {"name",            sym->name},
            {"kind",            lspSymbolKind(sym->kind)},
            {"range",           range},
            {"selectionRange",  range}
        });
    }

    return JsonRpc::makeResponse(id, symbols);
}

// HighlightKind: Text=1, Read=2, Write=3
nlohmann::json LspHandler::handleDocumentHighlight(const nlohmann::json& id,
                                                    const nlohmann::json& params) {
    std::string uri  = params["textDocument"]["uri"].get<std::string>();
    int         line = params["position"]["line"].get<int>();
    int         ch   = params["position"]["character"].get<int>();

    DocumentState* state = store_.get(uri);
    if (!state) return JsonRpc::makeResponse(id, nlohmann::json::array());

    Symbol* sym = findSymbolAt(*state, line, ch);
    if (!sym)   return JsonRpc::makeResponse(id, nlohmann::json::array());

    nlohmann::json highlights = nlohmann::json::array();

    auto makeHighlight = [&](const SourceLocation& loc, int kind) {
        // documentHighlight protokolde tek bir belgeye özeldir (uri alanı
        // yok) — başka dosyadaki referansları BURAYA sızdırmıyoruz (kök
        // neden #4'ün documentHighlight varyantı).
        if (!loc.isValid() || loc.filePath() != state->filePath) return;
        LspPosition pos = toLspPos(state->content, state->lineStarts, loc);
        int  end = pos.character + static_cast<int>(sym->name.size());
        highlights.push_back({
            {"range", {
                {"start", {{"line", pos.line}, {"character", pos.character}}},
                {"end",   {{"line", pos.line}, {"character", end}}}
            }},
            {"kind", kind}
        });
    };

    if (sym->definitionLoc.isValid())
        makeHighlight(sym->definitionLoc, 3); // Write — tanım noktası

    for (const auto& ref : sym->references)
        makeHighlight(ref, 2); // Read — kullanım noktaları

    return JsonRpc::makeResponse(id, highlights);
}

// ─────────────────────────────────────────────────────────────────────────────
// Completion (Faz 4 — token/sembol tabanlı)
// ─────────────────────────────────────────────────────────────────────────────
//
// Faz 4'te string-hack yardımcıları (wordPrefix/lineUpToCursor/wordBefore)
// kaldırıldı. Yerine:
//   - İmleç öncesi bağlam token dizisinden çıkarılıyor (`.`, `::`, zincir)
//   - Zincir çözümü: findSymbolAt + structLayouts yürüyüşüyle a.b.c.
//   - Scope filtrelemesi: yalnızca imlecin bulunduğu fonksiyonun lokalleri
//     + globaller önerilir; başka fonksiyonun lokali ASLA önerilmez
//   - Builtin metodlar BuiltinMethodRegistry'den üretilir
//     (src/builtin/builtin_methods.hpp — tek doğruluk kaynağı)

// Dil anahtar kelimeleri — completion önerileri ve rename hedef-ad doğrulaması
// ortak kullanır.
static const std::vector<std::string> kKeywords = {
    "int","float","bool","string","void",
    "if","else","while","for","return",
    "true","false","null",
    "struct","enum","import","export",
    "break","continue","throw","try","catch",
    "switch","case","default","as",
    // ADR-045 threading
    "shared","thread","lock","unlock","wait","Pool","List","Thread"
};

// CompletionItemKind: Function=3, Variable=6, Field=5, Struct=22, Enum=13, EnumMember=20, Keyword=14
static int completionKind(SymbolKind k) {
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

// ── AST yardımcıları (scope filtrelemesi için) ──────────────────────────────

// Bir AST alt ağacındaki en büyük offset'i döndürür (düğümün bittiği yaklaşık konum).
static int astMaxOffset(ASTNode* node) {
    int max = node->loc.offset;
    for (auto* child : node->getChildren()) {
        int cm = astMaxOffset(child);
        if (cm > max) max = cm;
    }
    return max;
}

// Verilen offset'i kapsayan FunctionDecl AST düğümünü bulur.
// Bulamazsa nullptr (imleç global scope'ta).
static ASTNode* findEnclosingFunction(ASTNode* node, int offset) {
    if (!node) return nullptr;
    for (auto* child : node->getChildren()) {
        if (child->kind == ASTKind::FunctionDecl && child->loc.isValid()) {
            int end = astMaxOffset(child);
            if (offset >= child->loc.offset && offset <= end) return child;
        }
        ASTNode* found = findEnclosingFunction(child, offset);
        if (found) return found;
    }
    return nullptr;
}

// ── Scope filtrelemesi ──────────────────────────────────────────────────────

// İmlecin bulunduğu kapsamdan görünür sembolleri döndürür.
// Kural:
//   - Global semboller (scope->parent == nullptr) her zaman görünür
//   - Tanım konumu geçersiz (builtin) her zaman görünür
//   - Import edilmiş semboller her zaman görünür
//   - Lokal semboller yalnızca imleçle aynı fonksiyonun içindeyse görünür
//   - Field'lar hariç (onlar sadece . zinciriyle gelir)
static std::vector<Symbol*> visibleSymbols(DocumentState& state, int byteOffset) {
    ASTNode* func = state.ast ? findEnclosingFunction(state.ast, byteOffset) : nullptr;
    int funcStart = func ? func->loc.offset : 0;
    int funcEnd   = func ? astMaxOffset(func) : 0;

    std::vector<Symbol*> result;
    for (Symbol* sym : state.symbolTable.allSymbols()) {
        if (sym->kind == SymbolKind::Field) continue;

        // Global scope'ta tanımlanmış semboller her zaman görünür
        if (sym->scope && sym->scope->parent == nullptr) {
            result.push_back(sym);
            continue;
        }

        // Tanım konumu yok (builtin/yerleşik) — her zaman görünür
        if (!sym->definitionLoc.isValid()) {
            result.push_back(sym);
            continue;
        }

        // Başka dosyadan import edilmiş — her zaman görünür
        if (sym->definitionLoc.filePath() != state.filePath) {
            result.push_back(sym);
            continue;
        }

        // Lokal sembol: yalnızca imleçle aynı fonksiyon içindeyse
        if (func && sym->definitionLoc.offset >= funcStart &&
                     sym->definitionLoc.offset <= funcEnd) {
            result.push_back(sym);
            continue;
        }

        // İmleç global scope'ta (hiçbir fonksiyonun içinde değil) — tüm lokaller görünür
        if (!func) {
            result.push_back(sym);
        }
    }
    return result;
}

// ── Zincir çözümü (a.b.c. için) ─────────────────────────────────────────────

// Bir tanımlayıcı zincirini (`chain`) çözerek sonundaki Type'ı döndürür.
// Örn: ["p", "adres", "sehir"] → p'nin tipinden başla, adres alanının tipine
// geç, sehir alanının tipini döndür. Herhangi bir adım başarısız olursa
// Type::error() döner.
static Type resolveChainType(DocumentState& state, const std::vector<std::string>& chain) {
    if (chain.empty()) return Type::error();

    // İlk tanımlayıcıyı sembol tablosunda bul
    Symbol* sym = nullptr;
    for (Symbol* s : state.symbolTable.allSymbols()) {
        if (s->name == chain[0] && s->kind != SymbolKind::Field) {
            sym = s;
            break;
        }
    }
    if (!sym) return Type::error();

    Type current = sym->type;

    for (size_t i = 1; i < chain.size(); ++i) {
        // Nullable / array wrapper'ları soy
        Type t = current;
        while (t.isArray() && t.elementType) t = *t.elementType;
        if (!t.isStruct()) return Type::error();

        auto it = state.symbolTable.structLayouts.find(t.structName);
        if (it == state.symbolTable.structLayouts.end()) return Type::error();

        bool found = false;
        for (auto& [fn, ft] : it->second) {
            if (fn == chain[i]) { current = ft; found = true; break; }
        }
        if (!found) return Type::error();
    }
    return current;
}

// ── Token-tabanlı bağlam çıkarma ────────────────────────────────────────────

struct CompletionCtx {
    enum Kind { Normal, Dot, Scope, ImportModule, ImportSymbol };
    Kind kind = Normal;
    std::vector<std::string> chain;  // Dot: zincirdeki tanımlayıcılar (sıralı)
    std::string target;              // Scope: :: solundaki ifade
    std::string prefix;              // Normal: kısmî tanımlayıcı metni
    std::string importModule;        // ImportSymbol: `from <module>` bağlamı
};

// İmleç öncesi token dizisinden completion bağlamını çıkarır.
// byteOffset: imlecin 0-tabanlı byte offset'i (positionEncoding dönüşümü sonrası).
static CompletionCtx analyzeContext(DocumentState& state, int byteOffset) {
    CompletionCtx ctx;
    const auto& toks = state.tokens;
    if (toks.empty()) return ctx;

    // İmleçten ÖNCE biten token'ları topla (binary search ile imleç konumunu bul)
    auto it = std::upper_bound(toks.begin(), toks.end(), byteOffset,
        [](int off, Token* t) { return off < t->start; });

    // İmleçten önce biten token'ları geriye doğru topla (en yakın en başta)
    std::vector<Token*> left;
    auto rit = it;
    while (rit != toks.begin()) {
        --rit;
        Token* tok = *rit;
        if (tok->end > byteOffset) continue; // imlecin ÖTESİNE taşan token'ı atla
        left.push_back(tok);
        if (left.size() >= 20) break;
    }

    if (left.empty()) return ctx;
    Token* nearest = left[0];

    // `import|` / `import m|` — gömülü FFI modül adları.
    if (nearest->token == "import" ||
        (nearest->gettype() == "identifier" && left.size() > 1 &&
         left[1]->token == "import")) {
        ctx.kind = CompletionCtx::ImportModule;
        if (nearest->token != "import")
            ctx.prefix = nearest->token.substr(0, std::max(0, byteOffset - nearest->start));
        return ctx;
    }

    // `import { | } from math` — math kataloğundaki FFI isimleri.
    if (nearest->token == "{" && left.size() > 1 && left[1]->token == "import") {
        ctx.kind = CompletionCtx::ImportSymbol;
        size_t lineStart = state.content.rfind('\n', static_cast<size_t>(byteOffset));
        lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
        size_t from = state.content.find("from", static_cast<size_t>(byteOffset));
        size_t lineEnd = state.content.find('\n', static_cast<size_t>(byteOffset));
        if (from != std::string::npos && (lineEnd == std::string::npos || from < lineEnd)) {
            size_t p = from + 4;
            while (p < state.content.size() && std::isspace((unsigned char)state.content[p])) ++p;
            size_t e = p;
            while (e < state.content.size() &&
                   (std::isalnum((unsigned char)state.content[e]) || state.content[e] == '_')) ++e;
            ctx.importModule = state.content.substr(p, e - p);
        }
        return ctx;
    }

    // ── "." zinciri: a.b.c.| ──────────────────────────────────────────────
    if (nearest->token == ".") {
        ctx.kind = CompletionCtx::Dot;
        size_t i = 1;
        while (i < left.size()) {
            if (left[i]->gettype() == "identifier") {
                ctx.chain.push_back(left[i]->token);
                ++i;
                if (i < left.size() && left[i]->token == ".") {
                    ++i;
                } else {
                    break;
                }
            } else {
                break;
            }
        }
        std::reverse(ctx.chain.begin(), ctx.chain.end());
        return ctx;
    }

    // ── "::" scope çağrısı: expr::| ───────────────────────────────────────
    if (nearest->token == "::") {
        ctx.kind = CompletionCtx::Scope;
        if (left.size() > 1 && left[1]->gettype() == "identifier") {
            ctx.target = left[1]->token;
        }
        return ctx;
    }

    // ── Normal prefix: identif| veya foo| ─────────────────────────────────
    if (nearest->gettype() == "identifier") {
        if (byteOffset >= nearest->start && byteOffset <= nearest->end) {
            ctx.prefix = nearest->token.substr(0, byteOffset - nearest->start);
        } else if (nearest->end <= byteOffset) {
            // İmleç tanımlayıcının hemen sonrasında (boşluk olabilir)
            ctx.prefix = nearest->token;
        }
        return ctx;
    }

    return ctx;
}

// ── Builtin metod listesi (BuiltinMethodRegistry'den) ───────────────────────

// BuiltinMethodRegistry'deki bir metodu LSP CompletionItem'a dönüştürür.
static nlohmann::json builtinMethodItem(const DataMethod* m) {
    // insertText: methodAdı(arg1, arg2) — receiver (params[0]) hariç
    std::string insertText = std::string(m->name) + "(";
    for (size_t i = 1; i < m->params.size(); ++i) {
        if (i > 1) insertText += ", ";
        insertText += "${" + std::to_string(i) + "}";
    }
    insertText += ")";

    // detail: (argTipleri) → dönüşTipi
    std::string detail = "(";
    for (size_t i = 1; i < m->params.size(); ++i) {
        if (i > 1) detail += ", ";
        switch (m->params[i].kind) {
            case DataParamKind::Fixed:     detail += m->params[i].fixedType.toString(); break;
            case DataParamKind::ElemType:  detail += "T";   break;
            case DataParamKind::ElemArray: detail += "T[]"; break;
            case DataParamKind::StringVal: detail += "string"; break;
        }
    }
    detail += ") → ";
    switch (m->ret.kind) {
        case DataReturnKind::Fixed:     detail += m->ret.fixedType.toString(); break;
        case DataReturnKind::ElemType:  detail += "T";   break;
        case DataReturnKind::ElemArray: detail += "T[]"; break;
    }

    return {
        {"label",            m->name},
        {"kind",             2},  // Method
        {"detail",           detail},
        {"insertText",       insertText},
        {"insertTextFormat", 2},  // Snippet
    };
}

// ADR-045: Pool/List/Thread metotları (TypeChecker::checkThreadIntrinsic ile
// aynı liste; BuiltinMethodRegistry'de değiller). Receiver başka tipse boş.
static nlohmann::json threadMethodsForType(const Type& t) {
    struct M { const char* name; const char* detail; const char* snippet; };
    static const M kPool[] = {
        {"push",   "void push(T value) — kuyruk doluysa bekler", "push(${1:value})"},
        {"pop",    "T pop() — kuyruk boşsa bekler",               "pop()"},
        {"setMax", "void setMax(int n) — kapasite sınırı",       "setMax(${1:n})"},
        {"length", "int length()",                                 "length()"},
    };
    static const M kList[] = {
        {"append", "void append(T value)", "append(${1:value})"},
        {"get",    "T get(int index)",     "get(${1:index})"},
        {"length", "int length()",         "length()"},
    };
    static const M kThread[] = {
        {"join",    "void join() — thread bitene kadar bekler", "join()"},
        {"stop",    "void stop() — durdurma ister, beklemez",   "stop()"},
        {"running", "bool running()",                          "running()"},
    };
    nlohmann::json items = nlohmann::json::array();
    auto add = [&](const M* b, const M* e) {
        for (const M* m = b; m != e; ++m)
            items.push_back({{"label", m->name}, {"kind", 2},  // Method
                             {"detail", m->detail},
                             {"insertText", m->snippet}, {"insertTextFormat", 2}});
    };
    if (t.isPool())        add(std::begin(kPool),   std::end(kPool));
    else if (t.isList())   add(std::begin(kList),   std::end(kList));
    else if (t.isThread()) add(std::begin(kThread), std::end(kThread));
    return items;
}

// Verilen tip için BuiltinMethodRegistry'deki uygun metodları döndürür.
// Kategori filtrelemesi: Array metodları yalnızca array receiver için,
// StringVal metodları yalnızca string için, StructVal metodları yalnızca struct için.
static nlohmann::json builtinMethodsForType(const Type& receiverType, const std::string& typeName) {
    nlohmann::json items = nlohmann::json::array();
    
    bool isReceiverArray = receiverType.isArray();
    bool isString        = receiverType.isString();
    bool isStruct        = receiverType.isStruct();

    // Struct array elemanı için de struct kabul et
    if (isReceiverArray && receiverType.elementType && receiverType.elementType->isStruct())
        isStruct = true;
    // Tip adı büyük harfle başlıyorsa struct kabul et
    if (!typeName.empty() && std::isupper(static_cast<unsigned char>(typeName[0])))
        isStruct = true;

    // Belirli bir tip grubuna girmeyen skaler tipler için (int, float, bool vb.)
    // hiçbir builtin metod göstermiyoruz — registry'de bunlara ait metod yok.
    if (!isReceiverArray && !isString && !isStruct)
        return items;

    // Uygun kategorilere göre filtrele
    for (const DataMethod& method : dataAllMethods()) {
        const DataMethod* m = &method;

        bool include = false;
        switch (m->category) {
            case DataMethodCategory::Array:
                include = isReceiverArray;
                // `toString` yalnız byte[]'da geçerli (UTF-8 çözme); tip
                // denetleyici diğer dizilerde reddeder. Önermek, seçildiğinde
                // hata veren kod üretmek demekti.
                if (include && std::string(m->name) == "toString")
                    include = receiverType.elementType && receiverType.elementType->isByte();
                break;
            case DataMethodCategory::StringVal:
                include = isString;
                break;
            case DataMethodCategory::StructVal:
                include = isStruct;
                break;
        }
        if (include) items.push_back(builtinMethodItem(m));
    }
    return items;
}

// ── handleCompletion (Faz 4 — yeniden yazıldı) ──────────────────────────────

nlohmann::json LspHandler::handleCompletion(const nlohmann::json& id,
                                             const nlohmann::json& params) {
    std::string uri  = params["textDocument"]["uri"].get<std::string>();
    int         line = params["position"]["line"].get<int>();
    int         ch   = params["position"]["character"].get<int>();

    DocumentState* state = store_.get(uri);
    if (!state) return JsonRpc::makeResponse(id, nlohmann::json::array());

    // İmleci byte offset'e çevir (positionEncoding dönüşümü)
    int byteCol  = toByteColumn(state->content, line, ch);
    int byteOff  = lspLineStartOffset(state->content, line) + (byteCol - 1);
    nlohmann::json items = nlohmann::json::array();

    // Token dizisinden bağlam çıkar
    CompletionCtx ctx = analyzeContext(*state, byteOff);

    if (ctx.kind == CompletionCtx::ImportModule) {
        const auto modules = FfiCatalog::instance().modules();
        for (const auto& module : modules) {
            if (!ctx.prefix.empty() && module.rfind(ctx.prefix, 0) != 0) continue;
            items.push_back({
                {"label", module}, {"kind", 9},
                {"detail", "embedded FFI module"},
                // `import|` + Enter → `import { } from module`.
                // $0 imleci boş import listesinin içine yerleştirir.
                {"insertText", " { $0 } from " + module},
                {"insertTextFormat", 2}
            });
        }
        return JsonRpc::makeResponse(id, items);
    }

    if (ctx.kind == CompletionCtx::ImportSymbol) {
        for (const auto& name : FfiCatalog::instance().names(ctx.importModule)) {
            items.push_back({
                {"label", name}, {"kind", 3},
                {"detail", ctx.importModule + " FFI"},
                {"insertText", name}
            });
        }
        return JsonRpc::makeResponse(id, items);
    }

    // ── "." zinciri: a.b.c.| → alan tamamlama ──────────────────────────────
    if (ctx.kind == CompletionCtx::Dot) {
        Type targetType;
        if (ctx.chain.empty()) {
            // Sadece ".|" — öncesinde tanımlayıcı yok, yapabileceğimiz bir şey yok
            return JsonRpc::makeResponse(id, items);
        }

        // Zinciri çöz
        targetType = resolveChainType(*state, ctx.chain);
        if (targetType.isError()) return JsonRpc::makeResponse(id, items);

        // ADR-033 (#85): UFCS nokta çağrısı — receiver array/string ise
        // builtin metodları öner (arr.push, s.upper). Struct-array'de
        // alanlara zaten index'siz erişilemez; metod listesi doğru öneri.
        if (targetType.isArray() || targetType.isString()) {
            items = builtinMethodsForType(targetType, "");
            return JsonRpc::makeResponse(id, items);
        }
        if (targetType.isPool() || targetType.isList() || targetType.isThread())
            return JsonRpc::makeResponse(id, threadMethodsForType(targetType));
        if (!targetType.isStruct()) return JsonRpc::makeResponse(id, items);

        // Struct: alanlar + struct builtin metodları (toJson/dump — ADR-033)
        auto it = state->symbolTable.structLayouts.find(targetType.structName);
        if (it != state->symbolTable.structLayouts.end()) {
            for (auto& [fieldName, fieldType] : it->second) {
                items.push_back({
                    {"label",  fieldName},
                    {"kind",   5},  // Field
                    {"detail", fieldType.toString()},
                });
            }
        }
        for (auto& m : builtinMethodsForType(targetType, targetType.structName)) {
            // Alan gölgeleme (ADR-033): aynı adlı alan varsa metod önerme —
            // TypeChecker o çağrıyı zaten reddeder.
            bool shadowed = false;
            if (it != state->symbolTable.structLayouts.end())
                for (auto& [fieldName, fieldType] : it->second)
                    if (fieldName == m["label"].get<std::string>()) { shadowed = true; break; }
            if (!shadowed) items.push_back(m);
        }
        return JsonRpc::makeResponse(id, items);
    }

    // ── "::" scope: expr::| → builtin metod tamamlama ─────────────────────
    if (ctx.kind == CompletionCtx::Scope) {
        if (ctx.target.empty()) return JsonRpc::makeResponse(id, items);

        // Sembolü bul
        Symbol* objSym = nullptr;
        for (Symbol* s : state->symbolTable.allSymbols()) {
            if (s->name == ctx.target) { objSym = s; break; }
        }

        // Tip adına (struct/enum) :: koymak anlamsız — boş dön
        if (objSym && (objSym->kind == SymbolKind::Struct ||
                       objSym->kind == SymbolKind::Enum))
            return JsonRpc::makeResponse(id, items);

        if (objSym) {
            items = builtinMethodsForType(objSym->type, objSym->name);
        } else {
            // Tanımlayıcı çözülemedi — tip adı olabilir ("int", "string", ...)
            Type t = Type::fromName(ctx.target);
            if (!t.isError()) {
                items = builtinMethodsForType(t, ctx.target);
            }
        }
        return JsonRpc::makeResponse(id, items);
    }

    // ── Normal prefix tamamlama (scope filtreli) ──────────────────────────
    std::string prefix = ctx.prefix;

    // Scope-filtreli semboller
    for (Symbol* sym : visibleSymbols(*state, byteOff)) {
        if (!prefix.empty() && sym->name.rfind(prefix, 0) != 0) continue;

        std::string detail;
        if (sym->kind == SymbolKind::Function && sym->type.isFunction()) {
            std::string ret = sym->type.returnType ? sym->type.returnType->toString() : "void";
            detail = ret + " " + sym->name + "(";
            for (size_t i = 0; i < sym->type.paramTypes.size(); ++i) {
                if (i > 0) detail += ", ";
                detail += sym->type.paramTypes[i].toString();
                if (i < sym->paramNames.size()) detail += " " + sym->paramNames[i];
            }
            detail += ")";
        } else {
            detail = sym->type.toString();
        }

        items.push_back({
            {"label",  sym->name},
            {"kind",   completionKind(sym->kind)},
            {"detail", detail},
        });
    }

    // Anahtar kelimeler
    for (const auto& kw : kKeywords) {
        if (!prefix.empty() && kw.rfind(prefix, 0) != 0) continue;
        items.push_back({{"label", kw}, {"kind", 14}});
    }

    return JsonRpc::makeResponse(id, items);
}

// ─────────────────────────────────────────────────────────────────────────────
// Rename (Faz 5, #84)
// ─────────────────────────────────────────────────────────────────────────────

// Hedef ad geçerli bir tanımlayıcı mı? (ASCII kural: [A-Za-z_][A-Za-z0-9_]*,
// anahtar kelime değil)
static bool isValidIdentifier(const std::string& name) {
    if (name.empty()) return false;
    auto isAlpha = [](unsigned char c) { return std::isalpha(c) || c == '_'; };
    auto isAlnum = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
    if (!isAlpha(static_cast<unsigned char>(name[0]))) return false;
    for (char c : name)
        if (!isAlnum(static_cast<unsigned char>(c))) return false;
    for (const auto& kw : kKeywords)
        if (kw == name) return false;
    return true;
}

// Rename yardımcıları: struct alanları sembol tablosunda yaşamaz
// (structLayouts), bu yüzden AST'den toplanır. Ziyaretçi yalnız alan
// yeniden adlandırması için gereken iki düğüm türünü biriktirir.
namespace {
struct FieldSites {
    std::vector<MemberAccessNode*> accesses;
    std::vector<StructDeclNode*>   structs;
};

void collectFieldSites(ASTNode* n, FieldSites& out) {
    if (!n) return;
    if (auto* st = dynamic_cast<StructDeclNode*>(n)) { out.structs.push_back(st); return; }
    if (auto* ma = dynamic_cast<MemberAccessNode*>(n)) {
        out.accesses.push_back(ma);
        collectFieldSites(ma->object, out);
        return;
    }
    if (auto* fn = dynamic_cast<FunctionDeclNode*>(n)) {
        for (ASTNode* c : fn->getChildren()) collectFieldSites(c, out);
        return;
    }
    if (auto* vd = dynamic_cast<VariableDeclNode*>(n)) {
        collectFieldSites(vd->initExpr, out);
        for (ASTNode* c : vd->getChildren()) collectFieldSites(c, out);
        return;
    }
    if (auto* b = dynamic_cast<BinaryExpressionNode*>(n)) { collectFieldSites(b->Left, out); collectFieldSites(b->Right, out); return; }
    if (auto* p = dynamic_cast<PostfixNode*>(n)) { collectFieldSites(p->operand, out); return; }
    if (auto* c = dynamic_cast<CallExpressionNode*>(n)) { collectFieldSites(c->callee, out); for (auto* a : c->arguments) collectFieldSites(a, out); return; }
    if (auto* ix = dynamic_cast<IndexExpressionNode*>(n)) { collectFieldSites(ix->object, out); collectFieldSites(ix->index, out); return; }
    if (auto* al = dynamic_cast<ArrayLiteralNode*>(n)) { for (auto* e : al->elements) collectFieldSites(e, out); return; }
    if (auto* sc = dynamic_cast<ScopeCallNode*>(n)) { for (auto* a : sc->arguments) collectFieldSites(a, out); return; }
    if (auto* ce = dynamic_cast<CastExpressionNode*>(n)) { collectFieldSites(ce->operand, out); return; }
    if (auto* i = dynamic_cast<IfStatementNode*>(n)) { collectFieldSites(i->condition, out); collectFieldSites(i->thenBranch, out); collectFieldSites(i->elseBranch, out); return; }
    if (auto* w = dynamic_cast<WhileStatementNode*>(n)) { collectFieldSites(w->condition, out); collectFieldSites(w->body, out); return; }
    if (auto* d = dynamic_cast<DoWhileStatementNode*>(n)) { collectFieldSites(d->body, out); collectFieldSites(d->condition, out); return; }
    if (auto* f = dynamic_cast<ForStatementNode*>(n)) { collectFieldSites(f->init, out); collectFieldSites(f->condition, out); collectFieldSites(f->update, out); collectFieldSites(f->body, out); return; }
    if (auto* r = dynamic_cast<ReturnStatementNode*>(n)) { collectFieldSites(r->value, out); return; }
    if (auto* t = dynamic_cast<ThrowStatementNode*>(n)) { collectFieldSites(t->value, out); return; }
    if (auto* es = dynamic_cast<ExpressionStatementNode*>(n)) { collectFieldSites(es->expression, out); return; }
    if (auto* tr = dynamic_cast<TryStatementNode*>(n)) { collectFieldSites(tr->body, out); collectFieldSites(tr->handler, out); return; }
    if (auto* sw = dynamic_cast<SwitchStatementNode*>(n)) {
        collectFieldSites(sw->subject, out);
        for (auto& cc : sw->cases) { for (auto* v : cc.values) collectFieldSites(v, out); for (auto* b2 : cc.body) collectFieldSites(b2, out); }
        return;
    }
    for (ASTNode* c : n->getChildren()) collectFieldSites(c, out);   // Program, Block
}

// Nesne ifadesinin struct adı (nullable ise tabanı); struct değilse boş.
std::string structNameOf(ASTNode* obj) {
    auto* e = dynamic_cast<ExpressionNode*>(obj);
    if (!e || !e->resolvedType.isStruct()) return "";
    return e->resolvedType.structName;
}
} // namespace

nlohmann::json LspHandler::handleRename(const nlohmann::json& id,
                                         const nlohmann::json& params) {
    std::string uri     = params["textDocument"]["uri"].get<std::string>();
    int         line    = params["position"]["line"].get<int>();
    int         ch      = params["position"]["character"].get<int>();
    std::string newName = params["newName"].get<std::string>();

    DocumentState* state = store_.get(uri);
    if (!state) return JsonRpc::makeResponse(id, nullptr);

    Symbol* sym = findSymbolAt(*state, line, ch);

    if (!isValidIdentifier(newName))
        return JsonRpc::makeError(id, -32602,
            "invalid identifier: '" + newName + "'");

    // Struct alanı: sembol değildir; AST'den bildirim + tüm `x.alan`
    // erişimleri toplanır (bu belge içinde). Eskiden sonuç boş dönüyordu.
    if (!sym) {
        Token* tok = identifierTokenAt(*state, line, ch);
        if (!tok || !state->ast) return JsonRpc::makeResponse(id, nullptr);
        const std::string field = tok->token;
        FieldSites sites;
        collectFieldSites(state->ast, sites);
        auto memberTokenOffset = [&](MemberAccessNode* ma) -> int {
            auto it = std::upper_bound(state->tokens.begin(), state->tokens.end(),
                ma->loc.offset, [](int off, Token* t) { return off < t->start; });
            return it == state->tokens.end() ? -1 : (*it)->start;
        };
        std::string owner;
        for (auto* ma : sites.accesses)
            if (ma->member == field && memberTokenOffset(ma) == tok->start)
                owner = structNameOf(ma->object);
        for (auto* st : sites.structs)
            for (ASTNode* c : st->getChildren())
                if (auto* vd = dynamic_cast<VariableDeclNode*>(c))
                    if (vd->name == field &&
                        identOffsetFromDecl(state->content, vd->loc.offset, field) == tok->start)
                        owner = st->name;
        if (owner.empty()) return JsonRpc::makeResponse(id, nullptr);

        std::vector<int> offsets;
        for (auto* st : sites.structs)
            if (st->name == owner)
                for (ASTNode* c : st->getChildren())
                    if (auto* vd = dynamic_cast<VariableDeclNode*>(c))
                        if (vd->name == field)
                            offsets.push_back(identOffsetFromDecl(state->content, vd->loc.offset, field));
        for (auto* ma : sites.accesses)
            if (ma->member == field && structNameOf(ma->object) == owner)
                offsets.push_back(memberTokenOffset(ma));
        std::sort(offsets.begin(), offsets.end());
        offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
        nlohmann::json edits = nlohmann::json::array();
        for (int off : offsets) {
            if (off < 0) continue;
            auto it = std::upper_bound(state->lineStarts.begin(), state->lineStarts.end(), off);
            int li = static_cast<int>(std::distance(state->lineStarts.begin(), it)) - 1;
            SourceLocation l;
            l.setFilePath(state->filePath);
            l.line = li + 1;
            l.column = off - state->lineStarts[static_cast<size_t>(li)] + 1;
            l.offset = off;
            LspPosition pos = toLspPos(state->content, state->lineStarts, l);
            edits.push_back({{"range", {{"start", {{"line", pos.line}, {"character", pos.character}}},
                                        {"end", {{"line", pos.line}, {"character", pos.character + (int)field.size()}}}}},
                             {"newText", newName}});
        }
        nlohmann::json changes = nlohmann::json::object();
        changes[state->uri] = edits;
        return JsonRpc::makeResponse(id, {{"changes", changes}});
    }

    // Builtin (print, Error, ...) yeniden adlandırılamaz — tanımı kullanıcı
    // kodunda değil.
    if (sym->isBuiltin || !sym->definitionLoc.isValid())
        return JsonRpc::makeError(id, -32602,
            "cannot rename builtin symbol: " + sym->name);

    // Standart kütüphane (FFI) fonksiyonu: tanımı gömülü root.sqt'tedir.
    // Eskiden var olmayan `<builtin:root.sqt>` belgesine düzenleme
    // gönderiliyor, editör WorkspaceEdit'i uygulayamayıp rename'i tümden
    // bozuyordu; import satırı da geçersiz hale geliyordu.
    if (sym->hostFnId >= 0)
        return JsonRpc::makeError(id, -32602,
            "cannot rename standard library function '" + sym->name +
            "'; import it under another name: `import { " + sym->name + " as newName } from " +
            sym->ffiModule + ";`");

    // Tanım + tüm referanslar; (dosya, offset) ile tekilleştir — aynı konum
    // hem definitionLoc hem references'ta görünürse çifte edit üretme.
    std::vector<SourceLocation> locs;
    {
        // definitionLoc bildirim BAŞINI gösterir ("int deger"de `int` token'ı) —
        // edit tanımlayıcının kendisini hedeflemeli, yoksa tip adı bozulur.
        SourceLocation defLoc     = sym->definitionLoc;
        std::string    defContent = contentForLoc(*state, defLoc);
        int identOff = identOffsetFromDecl(defContent, defLoc.offset, sym->name);
        if (identOff >= 0 && identOff != defLoc.offset) {
            std::vector<int> starts = buildLineStarts(defContent);
            auto it = std::upper_bound(starts.begin(), starts.end(), identOff);
            int lineIdx   = static_cast<int>(std::distance(starts.begin(), it)) - 1;
            defLoc.offset = identOff;
            defLoc.line   = lineIdx + 1;
            defLoc.column = identOff - starts[lineIdx] + 1;
        }
        locs.push_back(defLoc);
    }
    for (const auto& ref : sym->references)
        if (ref.isValid()) locs.push_back(ref);

    // Struct/enum: tip konumundaki kullanımlar (`Point p;`, `Point f(...)`,
    // `Color.Red`) sembol referansı olarak kaydedilmiyor; rename yalnız
    // bildirimi değiştirip kodu derlenmez halde bırakıyordu. Bu belgenin
    // token'larından, başka bir sembole bağlı olmayan ve `.` ile başlamayan
    // aynı adlı tanımlayıcılar eklenir.
    if (sym->kind == SymbolKind::Struct || sym->kind == SymbolKind::Enum) {
        const auto& toks = state->tokens;
        for (size_t ti = 0; ti < toks.size(); ++ti) {
            Token* t = toks[ti];
            if (t->gettype() != "identifier" || t->token != sym->name) continue;
            if (ti > 0 && toks[ti - 1]->token == ".") continue;
            // definitionLoc bildirim başını (tip token'ını) gösterir: `P p;`
            // içindeki `P`, `p` değişkenine bağlı görünür. Adı farklı bir
            // sembole bağlıysa bu token o sembolün tip kısmıdır, sayılır.
            auto bound = state->symbolByOffset.find(t->start);
            if (bound != state->symbolByOffset.end() && bound->second != sym &&
                bound->second->name == t->token)
                continue;
            auto it = std::upper_bound(state->lineStarts.begin(), state->lineStarts.end(), t->start);
            int li = static_cast<int>(std::distance(state->lineStarts.begin(), it)) - 1;
            SourceLocation l;
            l.setFilePath(state->filePath);
            l.line = li + 1;
            l.column = t->start - state->lineStarts[static_cast<size_t>(li)] + 1;
            l.offset = t->start;
            locs.push_back(l);
        }
    }
    // Dosya başına içerik + satır indeksi bir kez kurulur (handleReferences
    // ile aynı desen) — import taraması ve edit üretimi ortak kullanır.
    std::map<std::string, std::pair<std::string, std::vector<int>>> fileCache;
    auto fileData = [&](const std::string& fp)
        -> std::pair<const std::string*, const std::vector<int>*> {
        if (fp == state->filePath) return {&state->content, &state->lineStarts};
        auto it = fileCache.find(fp);
        if (it == fileCache.end()) {
            std::string c = store_.contentForPath(fp);
            it = fileCache.emplace(fp,
                    std::make_pair(std::move(c), std::vector<int>{})).first;
            it->second.second = buildLineStarts(it->second.first);
        }
        return {&it->second.first, &it->second.second};
    };

    // Import bağlayıcıları: `import { helper } from "..."` içindeki ad sembol
    // tablosunda referans olarak KAYITLI DEĞİL (ImportDeclNode ad başına konum
    // taşımıyor — TODO(#84): konum eklenince bu tarama kalkar). Rename sonrası
    // kod bozulmasın diye referans geçen her dosyanın import satırlarında
    // yalnızca {...} arasındaki tam-kelime geçişler de edit'e dahil edilir
    // ("from \"helper.sqt\"" yol string'i bilerek kapsam dışı).
    {
        std::vector<std::string> files;
        for (const auto& l : locs)
            if (std::find(files.begin(), files.end(), l.filePath()) == files.end())
                files.push_back(l.filePath());
        auto isWord = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
        for (const auto& fp : files) {
            auto [content, starts] = fileData(fp);
            for (size_t li = 0; li < starts->size(); ++li) {
                size_t ls = (*starts)[li];
                size_t le = (li + 1 < starts->size())
                    ? (size_t)(*starts)[li + 1] : content->size();
                std::string lineStr = content->substr(ls, le - ls);
                size_t p = lineStr.find_first_not_of(" \t");
                if (p == std::string::npos || lineStr.compare(p, 6, "import") != 0)
                    continue;
                size_t ob = lineStr.find('{'), cb = lineStr.find('}');
                if (ob == std::string::npos || cb == std::string::npos || cb < ob)
                    continue;
                for (size_t q = lineStr.find(sym->name, ob);
                     q != std::string::npos && q + sym->name.size() <= cb;
                     q = lineStr.find(sym->name, q + sym->name.size())) {
                    bool sOk = !isWord(static_cast<unsigned char>(lineStr[q - 1]));
                    bool eOk = !isWord(static_cast<unsigned char>(lineStr[q + sym->name.size()]));
                    if (!sOk || !eOk) continue;
                    SourceLocation il;
                    il.setFilePath(fp);
                    il.line     = static_cast<int>(li) + 1;
                    il.column   = static_cast<int>(q) + 1;
                    il.offset   = static_cast<int>(ls + q);
                    locs.push_back(il);
                }
            }
        }
    }

    std::sort(locs.begin(), locs.end(),
        [](const SourceLocation& a, const SourceLocation& b) {
            if (a.filePath() != b.filePath()) return a.filePath() < b.filePath();
            return a.offset < b.offset;
        });
    locs.erase(std::unique(locs.begin(), locs.end(),
        [](const SourceLocation& a, const SourceLocation& b) {
            return a.filePath() == b.filePath() && a.offset == b.offset;
        }), locs.end());

    // Edit'leri dosya URI'sine göre grupla (WorkspaceEdit.changes) —
    // referanslar farklı dosyalardan gelebilir (çok dosyalı rename).
    nlohmann::json changes = nlohmann::json::object();
    for (const auto& loc : locs) {
        auto [content, starts] = fileData(loc.filePath());
        LspPosition pos = toLspPos(*content, *starts, loc);
        changes[store_.uriForPath(loc.filePath())].push_back({
            {"range", {
                {"start", {{"line", pos.line}, {"character", pos.character}}},
                {"end",   {{"line", pos.line}, {"character", pos.character + (int)sym->name.size()}}}
            }},
            {"newText", newName}
        });
    }

    return JsonRpc::makeResponse(id, {{"changes", changes}});
}

// ─────────────────────────────────────────────────────────────────────────────
// SignatureHelp (Faz 5, #84)
// ─────────────────────────────────────────────────────────────────────────────

// İmleci saran fonksiyon çağrısının bağlamı: token dizisi geriye taranır,
// eşleşmemiş '(' bulunduğunda solundaki tanımlayıcı callee'dir; derinlik-0'da
// sayılan virgüller aktif parametre indeksini verir.
struct CallCtx {
    std::string callee;       // çağrılan fonksiyon/metod adı
    std::string scopeTarget;  // "x::push(" desenindeki x (builtin metod için)
    std::string dotReceiver;  // "arr.push(" desenindeki arr (UFCS, ADR-033)
    int         activeParam = 0;
    bool        valid = false;
};

static CallCtx findCallContext(DocumentState& state, int byteOffset) {
    CallCtx ctx;
    const auto& toks = state.tokens;
    if (toks.empty()) return ctx;

    // İmleçten önce biten token'ları geriye doğru topla (en yakın en başta)
    auto it = std::upper_bound(toks.begin(), toks.end(), byteOffset,
        [](int off, Token* t) { return off < t->start; });
    std::vector<Token*> left;
    auto rit = it;
    while (rit != toks.begin()) {
        --rit;
        Token* tok = *rit;
        if (tok->end > byteOffset) continue;
        left.push_back(tok);
        if (left.size() >= 256) break; // uzun ifadelerde tarama tavanı
    }

    int depth = 0, commas = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        const std::string& s = left[i]->token;
        if (s == ")" || s == "]") { depth++; continue; }
        if (s == "[") {
            if (depth > 0) { depth--; continue; }
            return ctx; // eşleşmemiş '[' — indeksleme bağlamı, imza yok
        }
        if (s == "(") {
            if (depth > 0) { depth--; continue; }
            // Eşleşmemiş '(' — solundaki token callee olmalı
            if (i + 1 >= left.size() || left[i + 1]->gettype() != "identifier")
                return ctx; // gruplama pareni: (a + b
            ctx.callee      = left[i + 1]->token;
            ctx.activeParam = commas;
            ctx.valid       = true;
            // "x::adet(" deseni: builtin metod çağrısı — receiver'ı yakala.
            // Sol taraf değişken (identifier) ya da tip adı olabilir; tip
            // adları (string::upper, struct::toJson) KEYWORD token'dır.
            if (i + 3 < left.size() && left[i + 2]->token == "::" &&
                (left[i + 3]->gettype() == "identifier" ||
                 left[i + 3]->gettype() == "keyword"))
                ctx.scopeTarget = left[i + 3]->token;
            // "arr.push(" deseni: UFCS nokta çağrısı (ADR-033) — receiver
            // tek tanımlayıcıysa yakala (zincirli receiver şimdilik yok).
            else if (i + 3 < left.size() && left[i + 2]->token == "." &&
                     left[i + 3]->gettype() == "identifier")
                ctx.dotReceiver = left[i + 3]->token;
            return ctx;
        }
        if (depth == 0) {
            if (s == ",") { commas++; continue; }
            if (s == ";" || s == "{" || s == "}")
                return ctx; // deyim sınırı — çağrı bağlamı yok
        }
    }
    return ctx;
}

// Kullanıcı fonksiyonu sembolünden SignatureInformation üret.
static nlohmann::json signatureForFunction(Symbol* sym) {
    std::string ret = sym->type.returnType ? sym->type.returnType->toString() : "void";
    nlohmann::json paramsArr = nlohmann::json::array();
    std::string label = ret + " " + sym->name + "(";

    // print gibi geçici builtin'ler parametresiz Type::function ile kayıtlı
    // (TODO(#89) builtin kataloğu) — imzada tek bir "value" göster.
    if (sym->isBuiltin && sym->type.paramTypes.empty()) {
        label += "value";
        paramsArr.push_back({{"label", "value"}});
    } else {
        for (size_t i = 0; i < sym->type.paramTypes.size(); ++i) {
            std::string p = sym->type.paramTypes[i].toString();
            if (i < sym->paramNames.size()) p += " " + sym->paramNames[i];
            if (i > 0) label += ", ";
            label += p;
            paramsArr.push_back({{"label", p}});
        }
    }
    label += ")";
    return {{"label", label}, {"parameters", paramsArr}};
}

// BuiltinMethod kaydından SignatureInformation üret.
// includeReceiver: `array::push(arr, x)` biçiminde receiver AÇIK ilk argümandır
// — imzada görünmeli ki activeParameter hizalansın; UFCS'te (arr.push(x))
// receiver örtük olduğundan atlanır.
static nlohmann::json signatureForBuiltinMethod(const DataMethod* m,
                                                bool includeReceiver = false) {
    auto paramTypeStr = [](const DataParamRule& p) -> std::string {
        switch (p.kind) {
            case DataParamKind::Fixed:     return p.fixedType.toString();
            case DataParamKind::ElemType:  return "T";
            case DataParamKind::ElemArray: return "T[]";
            case DataParamKind::StringVal: return "string";
        }
        return "?";
    };
    nlohmann::json paramsArr = nlohmann::json::array();
    std::string label = std::string(m->name) + "(";
    for (size_t i = includeReceiver ? 0 : 1; i < m->params.size(); ++i) {
        std::string p = paramTypeStr(m->params[i]);
        if (!paramsArr.empty()) label += ", ";
        label += p;
        paramsArr.push_back({{"label", p}});
    }
    label += ") → ";
    switch (m->ret.kind) {
        case DataReturnKind::Fixed:     label += m->ret.fixedType.toString(); break;
        case DataReturnKind::ElemType:  label += "T";   break;
        case DataReturnKind::ElemArray: label += "T[]"; break;
    }
    return {{"label", label}, {"parameters", paramsArr}};
}

nlohmann::json LspHandler::handleSignatureHelp(const nlohmann::json& id,
                                                const nlohmann::json& params) {
    std::string uri  = params["textDocument"]["uri"].get<std::string>();
    int         line = params["position"]["line"].get<int>();
    int         ch   = params["position"]["character"].get<int>();

    DocumentState* state = store_.get(uri);
    if (!state) return JsonRpc::makeResponse(id, nullptr);

    int byteCol = toByteColumn(state->content, line, ch);
    int byteOff = lspLineStartOffset(state->content, line) + (byteCol - 1);

    CallCtx ctx = findCallContext(*state, byteOff);
    if (!ctx.valid) return JsonRpc::makeResponse(id, nullptr);

    nlohmann::json sig;

    // Bir tipin builtin metod imzasını üret (UFCS + :: ortak yolu).
    // includeReceiver: :: biçimlerinde receiver açık ilk argümandır.
    auto builtinSigForType = [&](const Type& recvType, bool includeReceiver) {
        if (recvType.isError()) return;
        std::string leftName = recvType.isStruct()
            ? recvType.structName : recvType.toString();
        const DataMethod* m = dataLookupMethod(
            leftName, ctx.callee, recvType.isStruct(), recvType.isArray());
        if (m) sig = signatureForBuiltinMethod(m, includeReceiver);
    };
    // Sembol tablosunda ada göre tip bul (Field hariç)
    auto typeOfName = [&](const std::string& name) -> Type {
        for (Symbol* s : state->symbolTable.allSymbols())
            if (s->name == name && s->kind != SymbolKind::Field)
                return s->type;
        return Type::fromName(name);
    };

    if (!ctx.dotReceiver.empty()) {
        // "arr.push(" — UFCS (ADR-033): receiver örtük, imzada görünmez
        builtinSigForType(typeOfName(ctx.dotReceiver), false);
    } else if (ctx.scopeTarget == "array") {
        // ADR-033 ad alanı: array::push(arr, x) — kategori sabit
        const DataMethod* m = dataLookupMethod(
            "array", ctx.callee, false, false);
        if (m) sig = signatureForBuiltinMethod(m, true);
    } else if (ctx.scopeTarget == "struct") {
        // ADR-033 ad alanı: struct::toJson(p)
        const DataMethod* m = dataLookupMethod(
            "struct", ctx.callee, true, false);
        if (m) sig = signatureForBuiltinMethod(m, true);
    } else if (!ctx.scopeTarget.empty()) {
        // "x::push(" — değişken ya da eski tip-adı sözdizimi (receiver açık)
        builtinSigForType(typeOfName(ctx.scopeTarget), true);
    } else {
        // Kullanıcı fonksiyonu (ya da print gibi builtin fonksiyon sembolü)
        for (Symbol* s : state->symbolTable.allSymbols()) {
            if (s->name == ctx.callee && s->kind == SymbolKind::Function) {
                sig = signatureForFunction(s);
                break;
            }
        }
    }

    if (sig.is_null()) return JsonRpc::makeResponse(id, nullptr);

    return JsonRpc::makeResponse(id, {
        {"signatures",      nlohmann::json::array({sig})},
        {"activeSignature", 0},
        {"activeParameter", ctx.activeParam}
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// semanticTokens — textDocument/semanticTokens/full
// ─────────────────────────────────────────────────────────────────────────────
//
// Sınıflandırma GRAMMAR regex'inden değil derleyicinin sembol tablosundan
// gelir: builtin'ler Symbol::isBuiltin ile işaretlidir (symbol_collector,
// BUILTIN_ID), kullanıcı tipleri SymbolKind::Struct/Enum'dır. Böylece builtin
// listesi LSP'de elle tutulmaz — derleyicinin kaydı tek kaynaktır.
//
// İki kademeli çözüm:
//   1. symbolByOffset — çözülmüş referans/tanım konumları (çağrılar, değişken
//      kullanımları). `print(...)` gibi builtin çağrıları buraya düşer.
//   2. isim tabanlı resolve — tip konumları (örn. `Stack s;` bildirimindeki
//      Stack) sembol indeksine girmez; isimle tablodan çözülür.
//
// LSP kuralı: delta kodlama (deltaLine, deltaStartChar, length, type, mods),
// UTF-16 karakter birimi — positionEncoding anlaşması byteColToLspAt ile
// uygulanır.
nlohmann::json LspHandler::handleSemanticTokens(const nlohmann::json& id,
                                                 const nlohmann::json& params) {
    std::string uri = params["textDocument"]["uri"].get<std::string>();
    DocumentState* state = store_.get(uri);
    if (!state) return JsonRpc::makeResponse(id, nullptr);

    // Legend indeksleri — initialize'daki tokenTypes sırasıyla birebir.
    enum : int {
        T_KEYWORD = 0, T_TYPE = 1, T_FUNCTION = 2, T_BUILTIN = 3,
        T_PARAMETER = 4, T_VARIABLE = 5, T_STRING = 6, T_NUMBER = 7
    };

    std::vector<int> data;
    int prevLine = 0, prevChar = 0;
    auto emit = [&](int line, int ch, int len, int type) {
        data.push_back(line - prevLine);
        data.push_back(line == prevLine ? ch - prevChar : ch);
        data.push_back(len);
        data.push_back(type);
        data.push_back(0); // tokenModifiers — boş legend
        prevLine = line;
        prevChar = ch;
    };

    const auto& toks   = state->tokens;
    const auto& starts = state->lineStarts;
    for (Token* tok : toks) {
        const std::string& kind = tok->gettype();
        int type = -1;

        if (kind == "keyword") {
            type = T_KEYWORD;
        } else if (kind == "string") {
            type = T_STRING;
        } else if (kind == "number") {
            type = T_NUMBER;
        } else if (kind == "identifier") {
            Symbol* sym = nullptr;
            auto found = state->symbolByOffset.find(tok->start);
            if (found != state->symbolByOffset.end()) {
                sym = found->second;
                // Symbol::definitionLoc bildirimin BAŞINA işaret eder
                // (document_store.hpp): "Stack s;" içindeki Stack konumu, s
                // değişkeninin tanımı sanılır. İsim eşleşmiyorsa konum bir
                // TİP ADIDIR (Stack, MyType) — tablodan tip olarak çöz.
                if (sym->kind == SymbolKind::Variable ||
                    sym->kind == SymbolKind::Parameter ||
                    sym->kind == SymbolKind::Field ||
                    sym->kind == SymbolKind::EnumValue) {
                    if (sym->name != tok->token)
                        sym = state->symbolTable.resolve(tok->token);
                }
            } else {
                // Tip konumları (parametre tipleri, fonksiyon dönüş tipleri)
                // hiçbir sembole bağlanmaz; isim tabanlı çözümle sınıflandır.
                sym = state->symbolTable.resolve(tok->token);
            }
            if (sym) {
                switch (sym->kind) {
                    case SymbolKind::Function:
                        type = sym->isBuiltin ? T_BUILTIN : T_FUNCTION;
                        break;
                    case SymbolKind::Struct:
                    case SymbolKind::Enum:
                        type = T_TYPE;
                        break;
                    case SymbolKind::Parameter:
                        type = T_PARAMETER;
                        break;
                    case SymbolKind::Variable:
                    case SymbolKind::Field:
                    case SymbolKind::EnumValue:
                        type = T_VARIABLE;
                        break;
                }
            }
        }
        if (type < 0) continue;

        // byte offset → (satır, LSP karakter) — utf-16/utf-8 anlaşması.
        auto it = std::upper_bound(starts.begin(), starts.end(), tok->start);
        int line = static_cast<int>(it - starts.begin()) - 1;
        if (line < 0) continue;
        int colByte = tok->start - starts[line] + 1; // 1-bazlı byte kolon
        int ch = byteColToLspAt(state->content, starts, line, colByte);
        emit(line, ch, tok->end - tok->start, type);
    }

    return JsonRpc::makeResponse(id, nlohmann::json{{"data", data}});
}
