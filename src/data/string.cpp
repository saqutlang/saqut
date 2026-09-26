// ============================================================================
// saQut — string veri tipi
// ============================================================================
//
// DİZİN:   src/data/string.cpp
// KATMAN:  data — string'in çalışma zamanındaki TEK sahibi
//
// BURADA NE VAR (#223):
//   string'in bellek temsili (string.hpp), tüm metodlarının imzaları VE
//   gövdeleri, ve backend'lerin çağırdığı arayüz. String davranışını
//   değiştirmek için açılacak tek dosya burasıdır.
//
// ÖNCESİNDE: imzalar builtin/builtin_methods.hpp'de, gövdeler
//   vm/interpreter.cpp'deki 398 satırlık switch'te, aralarında elle korunan
//   bir runtimeId sıra sözleşmesi vardı. Artık id/imza/gövde aynı kayıtta.
//
// SEMANTİK (ADR-024): string IMMUTABLE bir değer tipidir. Hiçbir metod
//   receiver'ı değiştirmez; hepsi yeni string üretir (mutating == false).
//   ADR-023 istisnası: eşitlik İÇERİK karşılaştırmasıdır, kimlik değil.
//
// BELLEK: VM string'i Value::stringValue içinde inline tutar; sınır temsili
//   (HostSlot) ve JIT register'ı StringObject* taşır. Bu ayrım bilinçlidir
//   (ADR-037) ve host_abi.hpp'de belgelenmiştir.
//
// ============================================================================

#include "data/string.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include "core/utf8.hpp"
#include "ffi/host_bridge.hpp"
#include "gc/gc_object.hpp"
#include "gc/gc_heap.hpp"

namespace {

// ── Ortak argüman denetimi ───────────────────────────────────────────────────
//
// Eski gövdeler her case'de aynı kontrolü elle tekrarlıyordu
// ("expected string" × 13). Tek yerde toplandı: mesaj biçimi de böylece
// kendiliğinden tutarlı kalır.
bool wantStr(HostCallFrame* f, int idx, const char* method) {
    // Arite denetimi — args[idx] sınır dışıysa okumak bellek hatasıdır.
    // Tip denetleyici normalde engeller; ABI sözleşmesi backend'lere de açık
    // olduğu için burada da savunma var (sessiz bozulma yerine açık hata).
    if (idx >= f->argc || !f->args) {
        f->err.set(std::string("string::") + method + " — eksik argüman", "E_HOST");
        return false;
    }
    if (f->args[idx].kind == HostKind::Str) return true;
    f->err.set(std::string("string::") + method + " — expected string", "E_HOST");
    return false;
}

// ── Metod gövdeleri ──────────────────────────────────────────────────────────

int str_length(HostCallFrame* f) {
    if (!wantStr(f, 0, "length")) return 1;
    f->ret = HostSlot::fromInt((int)utf8::codePointCount(hostAsString(f->args[0])));
    return 0;
}

int str_upper(HostCallFrame* f) {
    if (!wantStr(f, 0, "upper")) return 1;
    hostSetRetString(*f, utf8::upper(hostAsString(f->args[0])));
    return 0;
}

int str_lower(HostCallFrame* f) {
    if (!wantStr(f, 0, "lower")) return 1;
    hostSetRetString(*f, utf8::lower(hostAsString(f->args[0])));
    return 0;
}

int str_trim(HostCallFrame* f) {
    if (!wantStr(f, 0, "trim")) return 1;
    const std::string& src = hostAsString(f->args[0]);
    size_t start = src.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) { hostSetRetString(*f, ""); return 0; }
    size_t end = src.find_last_not_of(" \t\n\r");
    hostSetRetString(*f, src.substr(start, end - start + 1));
    return 0;
}

int str_split(HostCallFrame* f) {
    if (!wantStr(f, 0, "split") || !wantStr(f, 1, "split")) return 1;
    if (!f->env || !f->env->heap) {
        f->err.set("string::split — heap yok", "E_HOST");
        return 1;
    }
    // Kopya ZORUNLU: aşağıda receiver'dan parça üretirken heap tahsisi
    // yapılıyor ve tahsis GC'yi tetikleyip kaynağı hareket ettirebilir.
    const std::string src = hostAsString(f->args[0]);
    const std::string sep = hostAsString(f->args[1]);

    auto* arr = f->env->heap->allocArray();
    if (sep.empty()) {
        // Boş ayraç: her kod noktası ayrı eleman. UTF-8'de çok baytlı
        // karakterleri byte düzeyinde kırmamak için codePointBytes ile
        // ilerleriz (ADI-024 karakter indeksiyle tutarlı).
        for (size_t offset = 0; offset < src.size();) {
            const size_t cpLen = utf8::codePointBytes(src, offset);
            arr->elements.push_back(Value::fromString(src.substr(offset, cpLen)));
            offset += cpLen;
        }
    } else {
        size_t pos = 0, found;
        while ((found = src.find(sep, pos)) != std::string::npos) {
            arr->elements.push_back(Value::fromString(src.substr(pos, found - pos)));
            pos = found + sep.size();
        }
        arr->elements.push_back(Value::fromString(src.substr(pos)));
    }
    f->ret = HostSlot::fromRef(arr);
    return 0;
}

int str_substring(HostCallFrame* f) {
    if (!wantStr(f, 0, "substring")) return 1;
    const std::string& s = hostAsString(f->args[0]);
    int from = (int)hostAsI64(f->args[1]);
    int len  = (int)hostAsI64(f->args[2]);
    if (from < 0 || from > (int)utf8::codePointCount(s)) {
        f->err.set("string::substring — index out of bounds", "E_HOST");
        return 1;
    }
    if (len < 0) len = 0;
    hostSetRetString(*f, utf8::substring(s, (size_t)from, (size_t)len));
    return 0;
}

int str_replace(HostCallFrame* f) {
    if (!wantStr(f, 0, "replace") || !wantStr(f, 1, "replace") || !wantStr(f, 2, "replace"))
        return 1;
    std::string s          = hostAsString(f->args[0]);
    const std::string from = hostAsString(f->args[1]);
    const std::string to   = hostAsString(f->args[2]);
    // Boş `from` sonsuz döngü olurdu — eski davranışta olduğu gibi atlanır.
    if (!from.empty()) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();  // yeni metnin içinde tekrar aramamak için
        }
    }
    hostSetRetString(*f, std::move(s));
    return 0;
}

int str_repeat(HostCallFrame* f) {
    if (!wantStr(f, 0, "repeat")) return 1;
    const std::string s = hostAsString(f->args[0]);
    int n = (int)hostAsI64(f->args[1]);
    if (n < 0) n = 0;
    std::string result;
    result.reserve(s.size() * (size_t)n);
    for (int i = 0; i < n; ++i) result += s;
    hostSetRetString(*f, std::move(result));
    return 0;
}

int str_charAt(HostCallFrame* f) {
    if (!wantStr(f, 0, "charAt")) return 1;
    const std::string& s = hostAsString(f->args[0]);
    // 64-bit okunur: s[i] longint indeks de geçebilir; (int) kesme büyük bir
    // indeksi sınır içindeki yanlış bir karaktere çevirirdi.
    const int64_t idx = hostAsI64(f->args[1]);
    if (idx < 0 || idx >= (int64_t)utf8::codePointCount(s)) {
        f->err.set("string::charAt — index out of bounds", "E_HOST");
        return 1;
    }
    hostSetRetString(*f, utf8::charAt(s, (size_t)idx));
    return 0;
}

int str_indexOf(HostCallFrame* f) {
    if (!wantStr(f, 0, "indexOf") || !wantStr(f, 1, "indexOf")) return 1;
    // Code-point index döner (ADR-024): substring/charAt ile aynı uzayda
    // olduğundan sonucu doğrudan onlara beslemek tutarlıdır.
    size_t pos = utf8::indexOf(hostAsString(f->args[0]), hostAsString(f->args[1]));
    // Bulunamazsa null döner (dönüş tipi `int?`) — HATA DEĞİL, ADR-021.
    if (pos == std::string::npos) f->ret = HostSlot::null();
    else                          f->ret = HostSlot::fromInt((int)pos);
    return 0;
}

int str_contains(HostCallFrame* f) {
    if (!wantStr(f, 0, "contains") || !wantStr(f, 1, "contains")) return 1;
    bool found = hostAsString(f->args[0]).find(hostAsString(f->args[1])) != std::string::npos;
    f->ret = HostSlot::fromInt(found ? 1 : 0);
    return 0;
}

int str_startsWith(HostCallFrame* f) {
    if (!wantStr(f, 0, "startsWith") || !wantStr(f, 1, "startsWith")) return 1;
    const std::string& s = hostAsString(f->args[0]);
    const std::string& p = hostAsString(f->args[1]);
    bool ok = s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
    f->ret = HostSlot::fromInt(ok ? 1 : 0);
    return 0;
}

int str_endsWith(HostCallFrame* f) {
    if (!wantStr(f, 0, "endsWith") || !wantStr(f, 1, "endsWith")) return 1;
    const std::string& s = hostAsString(f->args[0]);
    const std::string& p = hostAsString(f->args[1]);
    bool ok = s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
    f->ret = HostSlot::fromInt(ok ? 1 : 0);
    return 0;
}

int str_toBuffer(HostCallFrame* f) {
    if (!wantStr(f, 0, "toBuffer")) return 1;
    if (!f->env || !f->env->heap) {
        f->err.set("string::toBuffer — heap yok", "E_HOST");
        return 1;
    }
    const std::string& text = hostAsString(f->args[0]);
    auto* arr = f->env->heap->allocArray((int)text.size(), ArrayElemKind::Byte);
    arr->bytes.assign(text.begin(), text.end());
    f->ret = HostSlot::fromRef(arr);
    return 0;
}

}  // namespace

// ── Metod tablosu ────────────────────────────────────────────────────────────
//
// Sıra ÖNEMSİZDİR: id artık gövdeye tablo pozisyonundan değil, kaydın
// kendisinden bağlıdır. Yeni metod eklemek = bu listeye bir satır eklemek;
// başka hiçbir dosyaya dokunulmaz.
//
// Hepsi mutating == false (ADR-024: string immutable).
const std::vector<DataMethod>& dataStringMethods() {
    static const std::vector<DataMethod> methods = {
        {"length",     DataMethodCategory::StringVal, {dpString()},
         drFixed(Type::Int()),    false, HostKind::Int, HOST_PURE, str_length},
        {"upper",      DataMethodCategory::StringVal, {dpString()},
         drFixed(Type::String()), false, HostKind::Str, HOST_PURE, str_upper},
        {"lower",      DataMethodCategory::StringVal, {dpString()},
         drFixed(Type::String()), false, HostKind::Str, HOST_PURE, str_lower},
        {"trim",       DataMethodCategory::StringVal, {dpString()},
         drFixed(Type::String()), false, HostKind::Str, HOST_PURE, str_trim},
        // split heap'te array üretir → GC tetikleyebilir
        {"split",      DataMethodCategory::StringVal, {dpString(), dpFixed(Type::String())},
         drFixed(Type::array(Type::String())), false, HostKind::Ref,
         HOST_NEEDS_HEAP, str_split},
        {"substring",  DataMethodCategory::StringVal,
         {dpString(), dpFixed(Type::Int()), dpFixed(Type::Int())},
         drFixed(Type::String()), false, HostKind::Str, HOST_CAN_FAIL, str_substring},
        {"replace",    DataMethodCategory::StringVal,
         {dpString(), dpFixed(Type::String()), dpFixed(Type::String())},
         drFixed(Type::String()), false, HostKind::Str, HOST_PURE, str_replace},
        {"repeat",     DataMethodCategory::StringVal, {dpString(), dpFixed(Type::Int())},
         drFixed(Type::String()), false, HostKind::Str, HOST_PURE, str_repeat},
        {"charAt",     DataMethodCategory::StringVal, {dpString(), dpFixed(Type::Int())},
         drFixed(Type::String()), false, HostKind::Str, HOST_CAN_FAIL, str_charAt},
        // indexOf bulunamazsa null döner (int?) — hata değil
        {"indexOf",    DataMethodCategory::StringVal, {dpString(), dpFixed(Type::String())},
         drFixed(Type::Int().asNullable()), false, HostKind::Int, HOST_PURE, str_indexOf},
        {"contains",   DataMethodCategory::StringVal, {dpString(), dpFixed(Type::String())},
         drFixed(Type::Bool()), false, HostKind::Int, HOST_PURE, str_contains},
        {"startsWith", DataMethodCategory::StringVal, {dpString(), dpFixed(Type::String())},
         drFixed(Type::Bool()), false, HostKind::Int, HOST_PURE, str_startsWith},
        {"endsWith",   DataMethodCategory::StringVal, {dpString(), dpFixed(Type::String())},
         drFixed(Type::Bool()), false, HostKind::Int, HOST_PURE, str_endsWith},
        {"toBuffer",   DataMethodCategory::StringVal, {dpString()},
         drFixed(Type::array(Type::Byte())), false, HostKind::Ref, HOST_NEEDS_HEAP, str_toBuffer},
    };
    return methods;
}
