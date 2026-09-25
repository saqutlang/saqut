// ============================================================================
// saQut LSP — Kapsam ve alıcı analizi gerçeklemesi
// ============================================================================

#include "lsp/lsp_analysis.hpp"
#include "data/data_registry.hpp"
#include "symbol/scope.hpp"
#include <algorithm>
#include <cctype>

// ─────────────────────────────────────────────────────────────────────────────
// ScopeIndex
// ─────────────────────────────────────────────────────────────────────────────

ScopeIndex ScopeIndex::build(const std::vector<Token*>& toks) {
    ScopeIndex si;
    std::vector<size_t> braceStack, parenStack;
    for (Token* t : toks) {
        if (t->gettype() != "delimiter" && t->gettype() != "operator") continue;
        const std::string& s = t->token;
        if (s == "{") {
            braceStack.push_back(si.braces_.size());
            si.braces_.push_back({t->start, INT_MAX});
        } else if (s == "}") {
            if (!braceStack.empty()) {
                si.braces_[braceStack.back()].close = t->start;
                braceStack.pop_back();
            }
        } else if (s == "(") {
            parenStack.push_back(si.parens_.size());
            si.parens_.push_back({t->start, INT_MAX});
        } else if (s == ")") {
            if (!parenStack.empty()) {
                si.parens_[parenStack.back()].close = t->start;
                parenStack.pop_back();
            }
        }
    }
    return si;   // push sırası = open sırası → zaten sıralı
}

int ScopeIndex::innermost(const std::vector<Pair>& v, int off) {
    // open'a göre sıralı; off'tan önce açılanlar arasında off'u içeren en son
    // açılan (en içteki) çift. Yuvalanma gereği geriye doğru ilk içeren odur.
    auto it = std::lower_bound(v.begin(), v.end(), off,
        [](const Pair& p, int o) { return p.open < o; });
    while (it != v.begin()) {
        --it;
        if (it->open < off && off < it->close) return static_cast<int>(it - v.begin());
    }
    return -1;
}

int ScopeIndex::braceOpeningAt(int openOff) const {
    auto it = std::lower_bound(braces_.begin(), braces_.end(), openOff,
        [](const Pair& p, int o) { return p.open < o; });
    if (it != braces_.end() && it->open == openOff)
        return static_cast<int>(it - braces_.begin());
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Kapsam bilinçli çözüm
// ─────────────────────────────────────────────────────────────────────────────

static bool isGlobalSymbol(const AnalysisView& v, const Symbol* s) {
    if (!s->definitionLoc.isValid()) return true;                  // yerleşik
    if (s->definitionLoc.filePath() != *v.filePath) return true;   // import
    return s->scope && s->scope->parent == nullptr;
}

// offset'ten sonraki ilk token'ın indeksi (start >= off).
static size_t tokenIndexAtOrAfter(const std::vector<Token*>& toks, int off) {
    auto it = std::lower_bound(toks.begin(), toks.end(), off,
        [](Token* t, int o) { return t->start < o; });
    return static_cast<size_t>(it - toks.begin());
}

ScopeIndex::Pair localScopeRange(const AnalysisView& v, const ScopeIndex& si,
                                 const Symbol* s) {
    if (isGlobalSymbol(v, s)) return {-1, INT_MAX};
    const int d = s->definitionLoc.offset;
    const int b = si.innermostBrace(d);
    const int p = si.innermostParen(d);
    // Parametre / for-init / catch değişkeni: tanım bir ( ) içinde ve o
    // parantez küme parantezinden DAHA İÇTE; kapsamı hemen ardından gelen
    // { } gövdesidir.
    if (p >= 0 && (b < 0 || si.paren(p).open > si.brace(b).open) &&
        si.paren(p).close != INT_MAX) {
        const auto& toks = *v.tokens;
        size_t k = tokenIndexAtOrAfter(toks, si.paren(p).close + 1);
        if (k < toks.size() && toks[k]->token == "{") {
            int bi = si.braceOpeningAt(toks[k]->start);
            if (bi >= 0) return si.brace(bi);
        }
    }
    if (b >= 0) return si.brace(b);
    return {-1, INT_MAX};
}

bool symbolVisibleAt(const AnalysisView& v, const ScopeIndex& si,
                     const Symbol* s, int offset) {
    if (s->kind == SymbolKind::Field) return false;
    if (isGlobalSymbol(v, s)) return true;
    ScopeIndex::Pair r = localScopeRange(v, si, s);
    if (offset < r.open || offset > r.close) return false;
    // Yerel değişken bildiriminden ÖNCE kullanılamaz; parametre gövdenin
    // her yerinde görünür.
    if (s->kind == SymbolKind::Variable && s->definitionLoc.offset > offset) return false;
    return true;
}

Symbol* resolveNameAt(const AnalysisView& v, const ScopeIndex& si,
                      const std::string& name, int offset) {
    Symbol* bestLocal  = nullptr;
    long    bestSpan   = 0;
    Symbol* bestGlobal = nullptr;
    for (Symbol* s : v.table->allSymbols()) {
        if (s->kind == SymbolKind::Field) continue;
        if (s->name != name && displayName(s->name) != name) continue;
        if (!symbolVisibleAt(v, si, s, offset)) continue;
        if (isGlobalSymbol(v, s)) {
            // Birden çok global aday (ör. import + yerleşik): bu dosyada
            // tanımlı olan ya da import edilen, yerleşikten önce gelir.
            if (!bestGlobal || (!s->isBuiltin && bestGlobal->isBuiltin)) bestGlobal = s;
            continue;
        }
        ScopeIndex::Pair r = localScopeRange(v, si, s);
        long span = static_cast<long>(r.close) - r.open;
        if (!bestLocal || span < bestSpan ||
            (span == bestSpan && s->definitionLoc.offset > bestLocal->definitionLoc.offset)) {
            bestLocal = s;
            bestSpan  = span;
        }
    }
    return bestLocal ? bestLocal : bestGlobal;
}

// ─────────────────────────────────────────────────────────────────────────────
// Yerleşik metot dönüş tipi
// ─────────────────────────────────────────────────────────────────────────────

Type methodReturnType(const Type& recv, const std::string& method) {
    if (recv.isPool()) {
        if (method == "pop") return recv.elementType ? *recv.elementType : Type::error();
        if (method == "length") return Type::Int();
        return Type::Void();
    }
    if (recv.isList()) {
        if (method == "get") return recv.elementType ? *recv.elementType : Type::error();
        if (method == "length") return Type::Int();
        return Type::Void();
    }
    if (recv.isThread()) {
        if (method == "running") return Type::Bool();
        return Type::Void();
    }
    const std::string left = recv.isStruct() ? recv.structName : recv.toString();
    const DataMethod* m = dataLookupMethod(left, method, recv.isStruct(), recv.isArray());
    if (!m) return Type::error();
    switch (m->ret.kind) {
        case DataReturnKind::Fixed:     return m->ret.fixedType;
        case DataReturnKind::ElemType:
            return (recv.isArray() && recv.elementType) ? *recv.elementType : Type::error();
        case DataReturnKind::ElemArray: return recv;
    }
    return Type::error();
}

// ─────────────────────────────────────────────────────────────────────────────
// Alıcı tipi — tokens[endIdx] ile biten ifade
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct RecvWalker {
    const AnalysisView& v;
    const RootResolver& resolveRoot;
    int depth = 0;

    const std::vector<Token*>& toks() const { return *v.tokens; }

    // tokens[i] kapanış ise ( ) / [ ] eşinin indeksi; yoksa -1.
    int matchingOpen(int i, const char* open, const char* close) const {
        int d = 0;
        for (int k = i; k >= 0; --k) {
            const std::string& s = toks()[static_cast<size_t>(k)]->token;
            if (s == close) ++d;
            else if (s == open && --d == 0) return k;
        }
        return -1;
    }

    ReceiverType fromType(const Type& t) {
        ReceiverType r;
        if (t.isError() || t.isVoid()) return r;
        r.ok = true;
        r.type = t;
        return r;
    }

    ReceiverType walk(int j) {
        ReceiverType fail;
        if (j < 0 || ++depth > 32) return fail;
        Token* t = toks()[static_cast<size_t>(j)];

        // ── Tanımlayıcı: kök ya da alan erişimi (a.b) ─────────────────────
        if (t->gettype() == "identifier") {
            // `yap().⏎    ns[0].` — satır sonunda kalan '.' ile alt satırın
            // başındaki ad birleştirilmez: kullanıcı yarım bir ifadeyi bırakıp
            // yeni deyime geçmiştir (parser ikisini tek zincir okur). Satır
            // başındaki `.bar()` zincir biçimi etkilenmez ('.' ile ad aynı satırda).
            bool dotOnPrevLine = false;
            if (j >= 1 && toks()[static_cast<size_t>(j - 1)]->token == ".") {
                Token* dot = toks()[static_cast<size_t>(j - 1)];
                const std::string& c = *v.content;
                for (int q = dot->end; q < t->start && q < static_cast<int>(c.size()); ++q)
                    if (c[static_cast<size_t>(q)] == '\n') { dotOnPrevLine = true; break; }
            }
            if (!dotOnPrevLine && j >= 2 && toks()[static_cast<size_t>(j - 1)]->token == ".") {
                ReceiverType base = walk(j - 2);
                if (!base.ok) return fail;
                if (!base.enumName.empty())                 // Color.Red → Color
                    return fromType(Type::enumType(base.enumName));
                if (!base.type.isStruct()) return fail;
                return fromType(v.table->getFieldType(base.type.structName, t->token));
            }
            Symbol* s = resolveRoot(t->token, t->start);
            if (!s) return fail;
            if (s->kind == SymbolKind::Enum) {
                ReceiverType r;
                r.ok = true;
                r.enumName = displayName(s->name);
                if (!v.table->isEnumName(r.enumName)) r.enumName = s->name;
                return r;
            }
            if (s->kind == SymbolKind::Function || s->kind == SymbolKind::Struct) return fail;
            return fromType(s->type);
        }

        // ── Çağrı: f(...) ya da x.m(...) ──────────────────────────────────
        if (t->token == ")") {
            int open = matchingOpen(j, "(", ")");
            if (open < 1) return fail;
            Token* callee = toks()[static_cast<size_t>(open - 1)];
            if (callee->gettype() != "identifier") return fail;   // gruplama: (a + b).
            if (open >= 3 && toks()[static_cast<size_t>(open - 2)]->token == ".") {
                ReceiverType base = walk(open - 3);
                if (!base.ok || !base.enumName.empty()) return fail;
                return fromType(methodReturnType(base.type, callee->token));
            }
            Symbol* s = resolveRoot(callee->token, callee->start);
            if (!s || s->kind != SymbolKind::Function || !s->type.returnType) return fail;
            return fromType(*s->type.returnType);
        }

        // ── İndeks: a[i] ──────────────────────────────────────────────────
        if (t->token == "]") {
            int open = matchingOpen(j, "[", "]");
            if (open < 1) return fail;
            ReceiverType base = walk(open - 1);
            if (!base.ok || !base.enumName.empty()) return fail;
            if (base.type.isArray() && base.type.elementType)
                return fromType(*base.type.elementType);
            if (base.type.isString()) return fromType(Type::String());
            return fail;
        }

        // ── Literal alıcı: "abc". ─────────────────────────────────────────
        if (t->gettype() == "string") return fromType(Type::String());
        return fail;
    }
};

}  // namespace

ReceiverType receiverTypeEndingAt(const AnalysisView& v, int endIdx,
                                  const RootResolver& resolveRoot) {
    if (!v.valid() || endIdx < 0 || endIdx >= static_cast<int>(v.tokens->size()))
        return {};
    RecvWalker w{v, resolveRoot};
    return w.walk(endIdx);
}

// ─────────────────────────────────────────────────────────────────────────────
// Üye tamamlama öğeleri
// ─────────────────────────────────────────────────────────────────────────────

nlohmann::json builtinMethodItem(const DataMethod* m) {
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

nlohmann::json builtinMethodsForType(const Type& receiverType, const std::string& typeName) {
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

    for (const DataMethod& method : dataAllMethods()) {
        const DataMethod* m = &method;
        bool include = false;
        switch (m->category) {
            case DataMethodCategory::Array:
                include = isReceiverArray;
                // `toString` yalnız byte[]'da geçerli (UTF-8 çözme); tip
                // denetleyici diğer dizilerde reddeder.
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

// ADR-045: Pool/List/Thread metotları (TypeChecker::checkThreadIntrinsic ile
// aynı liste; BuiltinMethodRegistry'de değiller). T, gerçek eleman tipiyle
// değiştirilir ("void push(int value)").
nlohmann::json threadMethodsForType(const Type& t) {
    struct M { const char* name; const char* ret; const char* params;
               const char* doc; const char* snippet; };
    static const M kPool[] = {
        {"push",   "void", "T value", "Kuyruğa ekler; kuyruk doluysa (setMax) yer açılana kadar bekler.", "push(${1:value})"},
        {"pop",    "T",    "",        "Kuyruktan alır; kuyruk boşsa eleman gelene kadar bekler.",        "pop()"},
        {"setMax", "void", "int n",   "Kuyruk kapasitesini sınırlar (push bu sınırda bekler).",         "setMax(${1:n})"},
        {"length", "int",  "",        "Kuyruktaki eleman sayısı.",                                       "length()"},
    };
    static const M kList[] = {
        {"append", "void", "T value",   "Listenin sonuna ekler (ekle-yalnız, thread'ler arası).", "append(${1:value})"},
        {"get",    "T",    "int index", "index'teki elemanı döndürür.",                           "get(${1:index})"},
        {"length", "int",  "",          "Listedeki eleman sayısı.",                               "length()"},
    };
    static const M kThread[] = {
        {"join",    "void", "", "Thread bitene kadar bekler.",           "join()"},
        {"stop",    "void", "", "Thread'in durmasını ister; beklemez.",  "stop()"},
        {"running", "bool", "", "Thread hâlâ çalışıyorsa true.",         "running()"},
    };
    const std::string elem = t.elementType ? t.elementType->toString() : "T";
    // Tek başına duran "T" kelimesini eleman tipiyle değiştir.
    auto subst = [&](const std::string& s) {
        std::string out;
        for (size_t p = 0; p < s.size(); ++p) {
            bool wordStart = p == 0 || !std::isalnum(static_cast<unsigned char>(s[p - 1]));
            bool wordEnd   = p + 1 >= s.size() || !std::isalnum(static_cast<unsigned char>(s[p + 1]));
            if (s[p] == 'T' && wordStart && wordEnd) out += elem;
            else out += s[p];
        }
        return out;
    };
    nlohmann::json items = nlohmann::json::array();
    auto add = [&](const M* b, const M* e) {
        for (const M* m = b; m != e; ++m)
            items.push_back({{"label", m->name}, {"kind", 2},  // Method
                             {"detail", subst(std::string(m->ret) + " " + m->name + "(" + m->params + ")")},
                             {"documentation", m->doc},
                             {"insertText", m->snippet}, {"insertTextFormat", 2}});
    };
    if (t.isPool())        add(std::begin(kPool),   std::end(kPool));
    else if (t.isList())   add(std::begin(kList),   std::end(kList));
    else if (t.isThread()) add(std::begin(kThread), std::end(kThread));
    return items;
}

nlohmann::json memberCompletionItems(const ReceiverType& r, SymbolTable& table,
                                     const std::string& prefix) {
    nlohmann::json items = nlohmann::json::array();
    if (!r.ok) return items;

    if (!r.enumName.empty()) {
        auto it = table.enumLayouts.find(r.enumName);
        if (it != table.enumLayouts.end())
            for (auto& [member, value] : it->second)
                items.push_back({{"label", member}, {"kind", 20},   // EnumMember
                                 {"detail", displayName(r.enumName) + "." + member + " = " +
                                            std::to_string(value)}});
    } else {
        const Type& t = r.type;
        if (t.isPool() || t.isList() || t.isThread()) {
            items = threadMethodsForType(t);
        } else if (t.isArray() || t.isString()) {
            items = builtinMethodsForType(t, "");
        } else if (t.isStruct()) {
            auto it = table.structLayouts.find(t.structName);
            std::vector<std::string> fieldNames;
            if (it != table.structLayouts.end()) {
                for (auto& [fieldName, fieldType] : it->second) {
                    fieldNames.push_back(fieldName);
                    items.push_back({{"label", fieldName}, {"kind", 5},   // Field
                                     {"detail", fieldType.toString()}});
                }
            }
            // Alan gölgeleme (ADR-033): aynı adlı alan varsa metodu önerme.
            for (auto& m : builtinMethodsForType(t, t.structName))
                if (std::find(fieldNames.begin(), fieldNames.end(),
                              m["label"].get<std::string>()) == fieldNames.end())
                    items.push_back(m);
        }
    }

    if (prefix.empty()) return items;
    nlohmann::json filtered = nlohmann::json::array();
    for (auto& it : items)
        if (it["label"].get<std::string>().rfind(prefix, 0) == 0) filtered.push_back(it);
    return filtered;
}

// ─────────────────────────────────────────────────────────────────────────────
// Anahtar kelime / tip adı listeleri
// ─────────────────────────────────────────────────────────────────────────────

const std::vector<std::string>& lspKeywords() {
    static const std::vector<std::string> kw = {
        "int","float","bool","string","void",
        "if","else","while","for","return",
        "true","false","null",
        "struct","enum","import","export",
        "break","continue","throw","try","catch",
        "switch","case","default","as",
        // ADR-045 threading
        "shared","thread","lock","unlock","wait","Pool","List","Thread"
    };
    return kw;
}

const std::vector<std::string>& lspBuiltinTypeNames() {
    static const std::vector<std::string> names = {
        "int","longint","float","double","decimal","byte","date","char",
        "string","bool","void","Pool","List","Thread"
    };
    return names;
}

// ─────────────────────────────────────────────────────────────────────────────
// Görünen ad ve imza
// ─────────────────────────────────────────────────────────────────────────────

std::string displayName(const std::string& internalName) {
    size_t at = internalName.find('@');
    return at == std::string::npos ? internalName : internalName.substr(0, at);
}

std::string symbolSignature(const Symbol* s) {
    const std::string name = displayName(s->name);
    switch (s->kind) {
        case SymbolKind::Function: {
            if (!s->type.isFunction()) return name + "()";
            std::string ret = s->type.returnType ? s->type.returnType->toString() : "void";
            std::string sig = ret + " " + name + "(";
            for (size_t i = 0; i < s->type.paramTypes.size(); ++i) {
                if (i > 0) sig += ", ";
                sig += s->type.paramTypes[i].toString();
                if (i < s->paramNames.size()) sig += " " + s->paramNames[i];
            }
            return sig + ")";
        }
        case SymbolKind::Struct:    return "struct " + name;
        case SymbolKind::Enum:      return "enum " + name;
        case SymbolKind::EnumValue: return s->type.toString() + " " + name;
        default:
            return std::string(s->isShared ? "shared " : "") + s->type.toString() + " " + name;
    }
}
