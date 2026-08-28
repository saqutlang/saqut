// ============================================================================
// saQut MIR JIT — Fast-mode Codegen Gövdesi (Dilim 1.5, #80, MIRPLAN.md §3/§4/§10)
//
// TEK dosya bu projede <mir.h>/<mir-gen.h> include eder (MIRPLAN.md §1).
// Programın TAMAMI (her fonksiyon) desteklenen opcode kümesinde değilse
// hiçbir şey derlenmez/çalıştırılmaz — kısmi JIT / sessiz VM'e düşme YOK
// (bkz. mir_backend.hpp başlık yorumu).
//
// KAPSAM (Dilim 1.5): int-skaler (Dilim 1) + FLOAT skaler. Register tipi
// IRFunction::slotTypes'tan seçilir (Float → MIR_T_D, diğerleri → MIR_T_I64,
// MIRPLAN §3). Slot türü Int/Float DIŞINDA bir şeyse (Ref/Str/Decimal/Date)
// program reddedilir — bu türler sonraki dilimlerde (kutulama + shadow stack).
// ============================================================================

#include "mir/mir_backend.hpp"

#include <cstring>
#include <cstddef>  // offsetof — JIT direct-memory view alanları (Aşama 0 spike)
#include <chrono>
#include <algorithm>
#include <set>

#include "ffi/host_bridge.hpp"
#include "ffi/host_registry.hpp"
#include "gc/shadow_stack.hpp"
#include "ir/ir_liveness.hpp"
#include "data/array.hpp"

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// Merkezi exit kodu sınıfları (0/64/65/70). Bağımlılıksız saf sabit başlığı —
// JIT runtime hatalarının VM ile aynı exit sözleşmesine uyması için gerekli.
#include "cli/exit_codes.hpp"

#include "mir/vendor/mir-gen.h"
#include "mir/vendor/mir.h"
#include "vm/value.hpp"   // Value tam tanımı — object.hpp'nin vector<Value> üyeleri için
#include "gc/gc_object.hpp"
#include "core/float_format.hpp"  // StringObject — JIT string kutulama (ADR-037)

namespace mir_backend {

namespace {

// ── JIT çalışma bağlamı (runtime context) ───────────────────────────────────
//
// JIT'in ÇALIŞMA ZAMANI durumunun tamamı bu tek yapıda toplanır (refactor,
// davranış değişikliği yok). Öncesinde bu alanlar dosyaya dağınık
// g_jit* global'leri olarak yayılmıştı — her biri ayrı sahiplik noktası
// olduğundan çok-thread'li çalışmanın önünde engeldi.
//
// Bugün süreç-ömrü TEK örnek vardır ve rt() ile erişilir. Thread desteği
// geldiğinde tek değişiklik rt()'nin deposunu thread_local yapmak olur;
// çağıran taraf değişmez. (MIRPLAN §9: MIR_context paylaşımı ve shadow
// stack ile aynı model.)
//
// Derleme-zamanı tablolar (intern edilmiş string havuzu, shadow-stack
// kodgen kararları) burada DEĞİLDİR — bunlar MIR context'ine ve üretilen
// koda gömülüdür; çalışma zamanında mutasyonları yoktur.

// ADR-025 deterministik stacktrace çerçevesi (yalnızca try'lı fonksiyonlar).
struct JitTraceFrame {
    std::string name;
    std::string file;
    int         line = 0;
    int         col  = 0;
};

// STRUCT_NEW metadata'sı: VM fieldNames + ADR-021 nullable zero-init
// maskesi. Derleme sırasında doldurulur, çalışmada salt okunur.
struct JitStructMeta {
    std::shared_ptr<std::vector<std::string>> names;
    std::vector<bool>                         nullableMask;
};

struct JitRuntime {
    // GC: JIT ve VM AYNI Heap'i paylaşır (jitSetHeap ile bağlanır). Toplama
    // eşiği/politikası Heap'in kendisindedir — backend'ler yalnızca
    // safepoint'lerinde collectIfNeeded() çağırır.
    Heap* heap = nullptr;

    // Hata yayılımı (#110): VM'in pendingThrow_ karşılığı — hata tek
    // bayrakta durur, kodgen her hata-üretebilen talimattan sonra kontrol
    // eder. errorLine/Col: jitSetError defaults için son hata konumu.
    StructObject* pendingError = nullptr;
    int64_t       errorLine    = 0;
    int64_t       errorCol     = 0;

    // Global slot'ların JIT tarafı görünümü (VM globalSlots_ ile aynı
    // değerler, ham register temsillerinde) + nullable çağrı kanalı.
    std::vector<int64_t> globalI;
    std::vector<double>   globalD;
    std::vector<void*>    globalP;
    int64_t               callNullArgs[64]{};
    int64_t               callRetNull = 0;

    // Bench profil sayaçları — nullptr ise sayaç artırılmaz (sıfır ek yük).
    JitCallCounters* benchCounters = nullptr;

    // Deterministik iz yığını (ADR-025) — yalnızca try'lı fonksiyonlar.
    std::vector<JitTraceFrame> traceStack;

    // STRUCT_NEW talimat başına bir kayıt (derleme sırasında dolar).
    std::vector<JitStructMeta> structMeta;

    // Host çağrı ABI'si (#222): çağrılar arasında yeniden kullanılan
    // scratch/owner — çağrı başına tahsis yapmamanın yolu. Argümanlar
    // MIR'den tek tek geçirilemez (değişken arite), bu yüzden sabit bir
    // tampona yazılır (tek iş parçacığı varsayımı, MIRPLAN §9).
    static constexpr int kMaxHostArgs = 8;
    HostSlot      hostArgs[JitRuntime::kMaxHostArgs];
    HostRetOwner  hostRetOwner;
    HostCallFrame hostFrame;
    HostEnv*      hostEnv = nullptr;

    // Fallible cast null kanalı: cast_begin nullable-mod bayrağını tutar,
    // null sonucu cast_null_check'e taşır.
    bool    castNullable = false;
    int64_t castNull     = 0;

    // jitNewString/jitBoxDecimal'in heap bağlı değilken (test/izole
    // kullanım) sızdırmadan çalışması için yedek havuzlar — normal yol
    // heap->allocString/allocDecimal'dir.
    std::vector<std::unique_ptr<StringObject>>  stringFallback;
    std::vector<std::unique_ptr<DecimalObject>> decimalFallback;
};

// Tek erişim noktası. THREAD NOTU: bugün süreç-ömrü tek örnek; thread
// desteğinde `static` → `thread_local` yapılır (MIRPLAN §9 modeli).
JitRuntime& rt() {
    static JitRuntime instance;
    return instance;
}

StringObject*  jitNewString(std::string v);
DecimalObject* jitBoxDecimal(const DecimalValue& v);

// ── try/catch hata yayılımı (#110, MIRPLAN §7) ──────────────────────────────
//
// Mekanizma KARARI: yakalanabilir hata, ilk taslaktaki (MIRPLAN §7 aday 1)
// setjmp/longjmp köprüsüyle değil, "her hata-üretebilen instruction sonrası
// pending kontrolü" (proje kendi 1367d88 tasarımı) ile yayılır. Gerekçe:
// setjmp, DÖNEN bir fonksiyon çerçevesinde kurulamaz (C11 7.13.2.1) — try
// gövdesi JIT kodunda çalışırken rt_try_push çoktan dönmüştür; longjmp çöp
// kareye döner. Bu, uzun-atlama yeniden-giriş (resume trampolini + JMPI)
// denemesiyle de SEGFAULT üretti (catch gövdesi fonksiyon prolog'u olmadan
// ortadan girilip RETURN'de bozuk kareye döner). Ölçülen kanıt geridedir.
//
// rt().pendingError VM'in pendingThrow_ karşılığıdır: hata tek bayrakta
// durur. JIT codegen her hata-üretebilen / CALL instruction'ından SONRA
// rt_jit_error_pending kontrol eder; doluysa ya yerel handler'a (catch) JMP
// eder ya da propagateLabel üzerinden çağırana döner (hata bayrakta kalır).
// Catch girişi rt_jit_error_take ile hatayı error slot'una bağlar.
//
// İz (trace) yığını ADR-025 deterministik stacktrace içindir; yalnızca try
// içeren fonksiyonlarda tutulur (MIRPLAN §7.1: try'sız yollar sıfır ek yük).
// Depolama: rt().traceStack (JitRuntime).

// En içten dışa (VM Interpreter::buildTrace ile aynı sıra ve biçim).
std::string jitBuildTrace() {
    std::string result;
    for (int i = (int)rt().traceStack.size() - 1; i >= 0; --i) {
        const auto& f = rt().traceStack[i];
        if (f.line > 0) {
            result += f.name + " (" + f.file + ":" + std::to_string(f.line) +
                      ":" + std::to_string(f.col) + ")\n";
        } else {
            result += f.name + " (?)\n";
        }
    }
    return result;
}
}  // namespace

// Trampolin/runtime yardımcı bölgesi — yukarıdaki bağlam tanımlarının
// (JitRuntime/rt) kullanıldığı iç-bağlantım (anonymous) bloğu.
namespace {

extern "C" void rt_jit_trace_enter(const char* name, const char* file) {
    rt().traceStack.push_back(JitTraceFrame{ name ? name : "", file ? file : "", 0, 0 });
}
extern "C" void rt_jit_trace_leave() {
    if (!rt().traceStack.empty()) rt().traceStack.pop_back();
}
extern "C" void rt_jit_trace_line(int64_t line, int64_t col) {
    if (!rt().traceStack.empty()) {
        rt().traceStack.back().line = (int)line;
        rt().traceStack.back().col  = (int)col;
    }
}

static StructObject* jitMakeError(std::string message, std::string code,
                                  int64_t line, int64_t col) {
    if (!rt().heap) return nullptr;
    auto* err = rt().heap->allocStruct(5);
    err->fields[0] = Value::fromInt((int)line);
    err->fields[1] = Value::fromInt((int)col);
    err->fields[2] = Value::fromString(std::move(message));
    err->fields[3] = Value::fromString("");
    err->fields[4] = Value::fromString(std::move(code));
    return err;
}

// Hatayı rt().pendingError'a bağlar (ilk hata kazanır — VM pendingThrow_
// üzerine yazmaz) ve trace alanını doldurur. Yakalama/catch codegen'in
// per-instruction kontrolünde gerçekleşir; burada longjmp YOKTUR.
static void jitSetError(std::string message, std::string code,
                        int64_t line = 0, int64_t col = 0) {
    if (line == 0) line = rt().errorLine;
    if (col == 0) col = rt().errorCol;
    if (!rt().pendingError) {
        auto* err = jitMakeError(std::move(message), std::move(code), line, col);
        if (err)
            err->fields[3] = Value::fromString(jitBuildTrace());
        rt().pendingError = err;
    }
}

extern "C" void rt_jit_error_location(int64_t line, int64_t col) {
    rt().errorLine = line;
    rt().errorCol = col;
}

extern "C" int64_t rt_jit_error_pending() {
    return rt().pendingError ? 1 : 0;
}

extern "C" int64_t rt_jit_error_take() {
    auto* err = rt().pendingError;
    rt().pendingError = nullptr;
    return reinterpret_cast<int64_t>(err);
}

extern "C" void rt_jit_throw_p(void* value, int64_t kind,
                                int64_t line, int64_t col) {
    if ((SlotType)kind == SlotType::Ref) {
        // Kullanıcı Error struct'ı throw etti — VM'deki gibi trace alanı
        // (fields[3]) doldurulur; message/code aynen korunur (ADR-025).
        auto* err = static_cast<StructObject*>(value);
        if (err && err->fields.size() >= 4)
            err->fields[3] = Value::fromString(jitBuildTrace());
        rt().pendingError = err;
        return;
    }
    if ((SlotType)kind == SlotType::Str) {
        auto* s = static_cast<StringObject*>(value);
        jitSetError(s ? s->data : std::string{}, "", line, col);
    } else {
        jitSetError("throw", "", line, col);
    }
}

extern "C" void rt_jit_call_arg_null_set(int64_t index, int64_t value) {
    if (index >= 0 && index < 64) rt().callNullArgs[index] = value;
}

extern "C" int64_t rt_jit_call_arg_null_get(int64_t index) {
    return index >= 0 && index < 64 ? rt().callNullArgs[index] : 0;
}

extern "C" void rt_jit_call_ret_null_set(int64_t value) {
    rt().callRetNull = value;
}

extern "C" int64_t rt_jit_call_ret_null_get() {
    return rt().callRetNull;
}

extern "C" int64_t rt_jit_global_load_i(int64_t index) {
    if (index < 0 || index >= (int64_t)rt().globalI.size()) return 0;
    return rt().globalI[(size_t)index];
}

extern "C" void rt_jit_global_store_i(int64_t index, int64_t value) {
    if (index >= 0 && index < (int64_t)rt().globalI.size())
        rt().globalI[(size_t)index] = value;
}

extern "C" double rt_jit_global_load_d(int64_t index) {
    if (index < 0 || index >= (int64_t)rt().globalD.size()) return 0.0;
    return rt().globalD[(size_t)index];
}

extern "C" void rt_jit_global_store_d(int64_t index, double value) {
    if (index >= 0 && index < (int64_t)rt().globalD.size())
        rt().globalD[(size_t)index] = value;
}

extern "C" int64_t rt_jit_global_load_p(int64_t index) {
    if (index < 0 || index >= (int64_t)rt().globalP.size()) return 0;
    return reinterpret_cast<int64_t>(rt().globalP[(size_t)index]);
}

extern "C" void rt_jit_global_store_p(int64_t index, int64_t value) {
    if (index >= 0 && index < (int64_t)rt().globalP.size())
        rt().globalP[(size_t)index] = reinterpret_cast<void*>(value);
}

// ─────────────────────────────────────────────────────────────────────────────
// JIT'in GC kök sağlayıcısı ve safepoint'i
// ─────────────────────────────────────────────────────────────────────────────
//
// JIT çalışırken VM'in çağrı yığını BOŞTUR — değerler MIR register'larındadır
// ve GC oraya bakamaz. Shadow stack (gc/shadow_stack.hpp) o register'ların
// GC'ye görünen yansımasıdır; burada bir RootSource olarak Heap'e kaydedilir.
//
// Kökler: shadow stack + global pointer slot'ları + uçuştaki hata nesnesi.
struct JitRootSource : RootSource {
    void collectRoots(RootSink& sink) override {
        for (Object* object : jitShadowStack().slots) sink.acceptObject(object);
        for (void* global : rt().globalP)
            sink.acceptObject(static_cast<Object*>(global));
        sink.acceptObject(rt().pendingError);

        // Host çağrısı için HAZIRLANMAKTA olan argüman tamponu. Argümanlar
        // teker teker yazılır (rt_jit_host_arg_*) ve çağrı en sonda yapılır;
        // bu aralıkta bir tahsis toplama tetiklerse, yazılmış ama henüz
        // kullanılmamış string/ref argümanı başka hiçbir kökten görünmez.
        //
        // Somut hata: print(x as float32) — cast trampolini yeni string
        // tahsis eder, o tahsis toplamayı tetikler, tamponda bekleyen string
        // süpürülür ve host thunk'ı serbest bırakılmış belleği okur.
        for (const HostSlot& argument : rt().hostArgs)
            if (argument.kind == HostKind::Str ||
                argument.kind == HostKind::Decimal ||
                argument.kind == HostKind::Ref)
                sink.acceptObject(static_cast<Object*>(argument.p));

        // Çağrıdan dönen ama henüz register'a/shadow stack'e yazılmamış değer.
        if (rt().hostFrame.ret.kind == HostKind::Str ||
            rt().hostFrame.ret.kind == HostKind::Decimal ||
            rt().hostFrame.ret.kind == HostKind::Ref)
            sink.acceptObject(static_cast<Object*>(rt().hostFrame.ret.p));
    }
};

JitRootSource& jitRootSource() {
    static thread_local JitRootSource source;
    return source;
}

// JIT safepoint'i. VM'de toplama talimat döngüsünde denenir; JIT'te o döngü
// olmadığı için TAHSİS noktalarında denenir — tahsisten HEMEN ÖNCE, çünkü
// yeni nesne henüz shadow stack'e yazılmamıştır ve toplanırdı.
void jitMaybeCollect() {
    if (rt().heap) rt().heap->collectIfNeeded();
}

// Koşu heap'i koşu sonunda yıkıldığı için sayaçları hayatta tutan depo.
GcStats& lastRunGcStatsStorage() {
    static GcStats stats;
    return stats;
}

// --gc-threshold'ün JIT karşılığı. 0 = ayarlanmadı (Heap varsayılanı).
int& gcThresholdForNextRunStorage() {
    static int bytes = 0;
    return bytes;
}

// ── #228: Ref opcode trampolinleri ──────────────────────────────────────────
//
// array/struct nesneleri JIT heap'inde tahsis edilir ve shadow stack üzerinden
// GC'ye görünür kılınır. Semantik VM ile birebir (interpreter.cpp ARRAY_*/
// FIELD_* dalları); eleman erişimi src/data/array.cpp'deki tek kaynaktan gelir.

extern "C" void rt_jit_shadow_set(int64_t base, int64_t slot, void* obj) {
    // Çerçeve-göreli indeks: base bu fonksiyonun girişinde alınan taban
    // (rt_jit_shadow_enter). Düz mutlak indeks yazımı recursive çağrıda ÜST
    // çerçevenin köklerini eziyordu — canlı nesne süpürülüp use-after-free
    // ("invalid struct field index" / segfault) üretiyordu.
    jitShadowStack().set((int)base + (int)slot, static_cast<Object*>(obj));
}

extern "C" int64_t rt_jit_shadow_enter() { return jitShadowStack().enter(); }

extern "C" void rt_jit_shadow_leave(int64_t base) { jitShadowStack().leave((int)base); }

extern "C" void* rt_jit_array_new(int64_t capacity, int64_t elemKind) {
    if (!rt().heap) return nullptr;
    jitMaybeCollect();
    auto ek  = (ArrayElemKind)elemKind;
    auto* arr = rt().heap->allocArray((int)capacity, ek);
    switch (ek) {
        case ArrayElemKind::Ref:     arr->elements.resize((size_t)capacity, Value::fromInt(0)); break;
        case ArrayElemKind::Byte:    arr->bytes.resize((size_t)capacity, 0);    break;
        case ArrayElemKind::Int:     arr->ints.resize((size_t)capacity, 0);     break;
        case ArrayElemKind::LongInt: arr->longs.resize((size_t)capacity, 0);    break;
        case ArrayElemKind::Float32: arr->f32s.resize((size_t)capacity, 0.0f);  break;
        case ArrayElemKind::Float64: arr->f64s.resize((size_t)capacity, 0.0);   break;
        case ArrayElemKind::Decimal: arr->decimals.resize((size_t)capacity);    break;
    }
    // resize sonrası view senkronu (plan: allocArray → resize → syncJitView).
    arr->syncJitView();
    return arr;
}

extern "C" int64_t rt_jit_array_len(void* a) {
    if (!a) return 0;
    return dataArraySize(static_cast<ArrayObject*>(a));
}

// Sınır dışı erişim VM'de yakalanabilir bir Error'dur; JIT'te try/catch yok
// (ENTER_TRY desteklenmiyor) → uncaught throw ile aynı: program durur.
static void jitBoundsFail(const char* what, int64_t idx, int64_t len) {
    jitSetError(std::string(what) + " index out of bounds (index=" +
                std::to_string(idx) + ", length=" + std::to_string(len) + ")",
                "E_OOB");
}

// Doğrudan MIR bounds-check'in cold path'i (spike: Int ARRAY_GET/ARRAY_SET).
// MIR'da sınır dışı branch bu C sarmalayıcıyı çağırır; mesajı üretir ve
// pending error'u set eder. Akış plan §6: bounds fail → error location →
// pending error → uncaught exit (JIT'te try/catch yok).
extern "C" void rt_jit_array_bounds_fail(int64_t idx, int64_t len) {
    jitBoundsFail("array", idx, len);
}

extern "C" int64_t rt_jit_array_get_i(void* a, int64_t idx) {
    auto* arr = static_cast<ArrayObject*>(a);
    if (!arr) { jitBoundsFail("array", idx, 0); return 0; }
    int64_t len = dataArraySize(arr);
    if (idx < 0 || idx >= len) { jitBoundsFail("array", idx, len); return 0; }
    return dataArrayElemAt(arr, (int)idx).asI64();
}

extern "C" double rt_jit_array_get_d(void* a, int64_t idx) {
    auto* arr = static_cast<ArrayObject*>(a);
    if (!arr) { jitBoundsFail("array", idx, 0); return 0.0; }
    int64_t len = dataArraySize(arr);
    if (idx < 0 || idx >= len) { jitBoundsFail("array", idx, len); return 0.0; }
    return dataArrayElemAt(arr, (int)idx).asDouble();
}

extern "C" void* rt_jit_array_get_p(void* a, int64_t idx) {
    auto* arr = static_cast<ArrayObject*>(a);
    if (!arr) { jitBoundsFail("array", idx, 0); return nullptr; }
    int64_t len = dataArraySize(arr);
    if (idx < 0 || idx >= len) { jitBoundsFail("array", idx, len); return nullptr; }
    Value v = dataArrayElemAt(arr, (int)idx);
    // VM string'i Value içinde INLINE tutar; JIT sınırında pointer gerekir.
    // Kutulama GC-yönetimli heap'e yapılır (shadow stack ile görünür).
    if (v.kind == ValueKind::String)  return jitNewString(v.stringValue());
    if (v.kind == ValueKind::Decimal) return jitBoxDecimal(v.decimalValue());
    return v.ref();
}

static void jitArraySet(ArrayObject* arr, int64_t idx, const Value& v) {
    if (!arr) { jitBoundsFail("array", idx, 0); return; }
    int64_t len = dataArraySize(arr);
    if (idx < 0 || idx >= len) { jitBoundsFail("array", idx, len); return; }
    size_t i = (size_t)idx;
    switch (arr->elemKind) {
        case ArrayElemKind::Ref:     arr->elements[i] = v; break;
        case ArrayElemKind::Byte:    arr->bytes[i]    = (uint8_t)v.asI64(); break;
        case ArrayElemKind::Int:     arr->ints[i]     = (int32_t)v.asI64(); break;
        case ArrayElemKind::LongInt: arr->longs[i]    = v.asI64(); break;
        case ArrayElemKind::Float32: arr->f32s[i]     = (float)v.asDouble(); break;
        case ArrayElemKind::Float64: arr->f64s[i]     = v.asDouble(); break;
        case ArrayElemKind::Decimal: arr->decimals[i] = v.decimalValue(); break;
    }
}

extern "C" void rt_jit_array_set_i(void* a, int64_t idx, int64_t v) {
    jitArraySet(static_cast<ArrayObject*>(a), idx, Value::fromLongInt(v));
}
extern "C" void rt_jit_array_set_d(void* a, int64_t idx, double v) {
    jitArraySet(static_cast<ArrayObject*>(a), idx, Value::fromFloat(v));
}
// Pointer yazarken kutulu string/decimal VM temsiline geri çevrilir; array
// eleman tipi bunu belirler (ADR-024: string değer-tipi, inline saklanır).
static Value jitUnboxForSlot(ArrayElemKind ek, void* p) {
    auto* o = static_cast<Object*>(p);
    if (o && ek == ArrayElemKind::Ref) {
        if (o->type == ObjectType::String)
            return Value::fromString(static_cast<StringObject*>(o)->data);
        if (o->type == ObjectType::Decimal)
            return Value::fromDecimal(static_cast<DecimalObject*>(o)->val);
    }
    return Value::fromRef(o);
}

extern "C" void rt_jit_array_set_p(void* a, int64_t idx, void* v) {
    auto* arr = static_cast<ArrayObject*>(a);
    if (!arr) { jitBoundsFail("array", idx, 0); return; }
    jitArraySet(arr, idx, jitUnboxForSlot(arr->elemKind, v));
}

// STRUCT_NEW metadata'sı: VM fieldNames'i ve ADR-021 nullable zero-init
// maskesini IRFunction'dan okur. JIT'te talimat başına bir kayıt indeksi
// geçirilir; tablo derleme sırasında doldurulur. Depolama: rt().structMeta.

extern "C" void* rt_jit_struct_new(int64_t fieldCount, int64_t metaId) {
    if (!rt().heap) return nullptr;
    jitMaybeCollect();
    auto* obj = rt().heap->allocStruct((int)fieldCount);
    if (metaId >= 0 && metaId < (int64_t)rt().structMeta.size()) {
        const auto& m = rt().structMeta[(size_t)metaId];
        obj->fieldNames = m.names;
        size_t n = std::min(m.nullableMask.size(), obj->fields.size());
        for (size_t i = 0; i < n; ++i)
            if (m.nullableMask[i]) obj->fields[i] = Value::null();
    }
    return obj;
}

static Value* jitFieldAt(void* o, int64_t idx) {
    auto* obj = static_cast<StructObject*>(o);
    if (!obj || idx < 0 || idx >= (int64_t)obj->fields.size()) {
        std::cerr << "runtime error: invalid struct field index " << idx << std::endl;
        std::exit(70);
    }
    return &obj->fields[(size_t)idx];
}

extern "C" int64_t rt_jit_field_get_i(void* o, int64_t idx) { return jitFieldAt(o, idx)->asI64(); }
extern "C" double  rt_jit_field_get_d(void* o, int64_t idx) { return jitFieldAt(o, idx)->asDouble(); }
extern "C" void*   rt_jit_field_get_p(void* o, int64_t idx) {
    Value* v = jitFieldAt(o, idx);
    if (v->kind == ValueKind::String)  return jitNewString(v->stringValue());
    if (v->kind == ValueKind::Decimal) return jitBoxDecimal(v->decimalValue());
    return v->ref();
}
extern "C" int64_t rt_jit_field_is_null(void* o, int64_t idx) {
    return jitFieldAt(o, idx)->kind == ValueKind::Null ? 1 : 0;
}

extern "C" void rt_jit_field_set_i(void* o, int64_t idx, int64_t v) {
    *jitFieldAt(o, idx) = Value::fromLongInt(v);
}
extern "C" void rt_jit_field_set_d(void* o, int64_t idx, double v) {
    *jitFieldAt(o, idx) = Value::fromFloat(v);
}
extern "C" void rt_jit_field_set_p(void* o, int64_t idx, void* v) {
    *jitFieldAt(o, idx) = jitUnboxForSlot(ArrayElemKind::Ref, v);
}

// ── #227: birleşik host çağrı trampolini ────────────────────────────────────
//
// JIT'in host çağrıları için bilmesi gereken TEK köprü. Öncesinde yalnızca
// print destekleniyordu; host fonksiyonları + built-in metodları açmak 70
// ayrı trampolin demekti (kesin sayılar değişkendir — yoruma yazılmaz).
// Artık yeni host fonksiyonu eklemek JIT'e hiç dokunmaz (#229: kayıt
// birliği — hostRegistry tek tablo).
// Argüman tamponu rt().hostArgs'tadır (JitRuntime); JitRuntime::kMaxHostArgs sabiti de
// oradadır. Tek iş parçacığı varsayımı (MIRPLAN §9), string trampolinleriyle
// aynı kısıt.
void jitSetHostEnv(HostEnv* env) { rt().hostEnv = env; }

extern "C" void rt_jit_host_arg_i(int64_t idx, int64_t kind, int64_t v) {
    if (idx < 0 || idx >= JitRuntime::kMaxHostArgs) return;
    rt().hostArgs[idx].kind = (HostKind)kind;
    rt().hostArgs[idx].i    = v;
}

extern "C" void rt_jit_host_arg_d(int64_t idx, int64_t kind, double v) {
    if (idx < 0 || idx >= JitRuntime::kMaxHostArgs) return;
    rt().hostArgs[idx].kind = (HostKind)kind;
    rt().hostArgs[idx].d    = v;
}

extern "C" void rt_jit_host_arg_nullable_i(int64_t idx, int64_t kind,
                                             int64_t v, int64_t isNull) {
    rt_jit_host_arg_i(idx, isNull ? (int64_t)HostKind::Null : kind, v);
}

extern "C" void rt_jit_host_arg_nullable_d(int64_t idx, int64_t kind,
                                             double v, int64_t isNull) {
    if (isNull) rt_jit_host_arg_i(idx, (int64_t)HostKind::Null, 0);
    else rt_jit_host_arg_d(idx, kind, v);
}

extern "C" int64_t rt_jit_host_call(int64_t entryId, int64_t argc) {
    // Bench profil sayaçları (nullptr ise sıfır ek yük — yalnızca bir karşılaştırma).
    // entryId bloklarına göre FFI/builtin ayrımı (bkz. host_registry.hpp):
    //   [0..256)    FFI        → ffi++
    //   [256..512)  Builtin    → builtin++
    //   [512..)     Çekirdek   → (ffi/builtin dışı)
    if (rt().benchCounters) {
        if (rt().benchCounters->callhost) ++(*rt().benchCounters->callhost);
        if (entryId < kBuiltinBase) {
            if (rt().benchCounters->ffi) ++(*rt().benchCounters->ffi);
        } else if (entryId < kCoreBase) {
            if (rt().benchCounters->builtin) ++(*rt().benchCounters->builtin);
        }
    }
    rt().hostFrame.reset();
    rt().hostFrame.args     = rt().hostArgs;
    rt().hostFrame.argc     = (int32_t)argc;
    rt().hostFrame.env      = rt().hostEnv;
    rt().hostFrame.retOwner = &rt().hostRetOwner;
    if (rt_host_call((int32_t)entryId, &rt().hostFrame) != 0) {
        jitSetError(rt().hostFrame.err.message,
                    rt().hostFrame.err.code.empty() ? "E_HOST" : rt().hostFrame.err.code);
        rt().hostFrame.ret = HostSlot::null();
        return 0;
    }
    if (rt().hostFrame.ret.kind == HostKind::Str) {
        auto* s = static_cast<StringObject*>(rt().hostFrame.ret.p);
        rt().hostFrame.ret = HostSlot::fromStr(jitNewString(s ? s->data : std::string{}));
    } else if (rt().hostFrame.ret.kind == HostKind::Decimal) {
        auto* d = static_cast<DecimalObject*>(rt().hostFrame.ret.p);
        rt().hostFrame.ret = HostSlot::fromDecimal(
            jitBoxDecimal(d ? d->val : DecimalValue{}));
    } else if (rt().hostFrame.ret.kind == HostKind::Ref) {
        // Ref (array/byte[] vb.) host dönüşü: host gövdesi nesneyi zaten bu
        // koşunun heap'inde (jitEnv.heap) tahsis etti — pointer register'a
        // olduğu gibi iletilir. Köklenmesi kodgen'in işidir: CALLHOST'tan
        // sonra dest slot'u için emitShadowSet yayılır, nesne oradan görünür.
        rt().hostFrame.ret = HostSlot::fromRef(rt().hostFrame.ret.p);
    }
    return rt().hostFrame.ret.i;
}

extern "C" double rt_jit_host_call_d(int64_t entryId, int64_t argc) {
    (void)rt_jit_host_call(entryId, argc);
    return rt().hostFrame.ret.d;
}

extern "C" int64_t rt_jit_host_ret_is_null() {
    return rt().hostFrame.ret.kind == HostKind::Null ? 1 : 0;
}

// ── print(int) trampoline'i — VM'in Value::toString()'iyle birebir (ADR-024).
// #120: VM (9ac66d5) trailing "\n" eklemeyi bıraktı (console:: FFI hazırlığı,
// #115) — JIT aynı hizaya getirildi, yalnızca flush eklendi.
extern "C" void rt_jit_print_int(int64_t v) {
    std::cout << v << std::flush;
}

// ── print(float) trampoline'i — VM'in Value::toString() Float dalıyla BİREBİR
// aynı biçim (setprecision(10) + nokta yoksa ".0"). Diferansiyel testin stdout
// eşleşmesi buna bağlı (value.hpp:94-102). ──────────────────────────────────
extern "C" void rt_jit_print_float(double v) {
    // #114: biçim tek kaynaktan (core/float_format.hpp) — VM senkronu
    std::cout << formatDoublePrint(v) << std::flush;
}

// ── print(float32) trampoline'i — VM'in Value::toString() Float32 dalıyla BİREBİR
// (setprecision(9) + nokta yoksa ".0"). Argüman JIT'te MIR_T_F register olduğundan
// çağrı öncesi F2D ile double'a genişletilip buraya double gelir. ─────────────
extern "C" void rt_jit_print_float32(double v) {
    // #114: biçim tek kaynaktan (core/float_format.hpp) — VM senkronu
    std::cout << formatFloat32Print(v) << std::flush;
}

// ── print(string) trampoline'i (Dilim 3, ADR-037). Argüman, JIT register'ında
// pointer olarak taşınan bir StringObject*'tir (kutulanmış string). VM'in
// Value::toString() String dalı ham içeriği döndürür (value.hpp:104); #120
// öncesi print host'u "\n" ekliyordu, artık eklemiyor (VM ile birebir). ──────
extern "C" void rt_jit_print_str(void* strObj) {
    std::cout << static_cast<StringObject*>(strObj)->data << std::flush;
}

// ── Runtime string havuzu (Dilim 3). LOAD_STRING sabitleri derleme zamanı
// intern edilir (fonksiyon-local stringPool); CONCAT gibi ÇALIŞMA zamanı üretilen
// stringler burada tutulur. GC henüz JIT tarafını taramadığından (Dilim 2/§8)
// bunlar program-ömrü boyunca birikir ve tryCompileAndRunProgram sonunda toplu
// silinir (leak değil, ama döngüde çok concat = çok nesne — GC gelince çözülür).
// Tek-iş-parçacıklı varsayım (MIRPLAN §9: ileride thread-local). ──────────────
// #228: JIT nesneleri artık VM heap'inde ve GC'ye görünür (shadow stack).
// Öncesinde unique_ptr havuzunda süresiz birikiyorlardı — 200k concat'te
// JIT 21,8 MB / VM 6,8 MB (ölçüldü).
StringObject* jitNewString(std::string v) {
    if (rt().heap) { jitMaybeCollect(); return rt().heap->allocString(std::move(v)); }
    // Heap bağlı değilse (test/izole kullanım) sızdırmadan çalış.
    auto& fallback = rt().stringFallback;
    fallback.push_back(std::make_unique<StringObject>(std::move(v)));
    return fallback.back().get();
}

// Heap'i bağlar ve JIT'in kök sağlayıcısını ona kaydeder. nullptr bağlamak
// kaydı kaldırır — heap'siz koşuda (test/izole kullanım) GC devrede değildir.
void jitSetHeap(Heap* h) {
    if (rt().heap) rt().heap->removeRootSource(&jitRootSource());
    rt().heap = h;
    if (h) h->addRootSource(&jitRootSource());
}

// ── STRING_CONCAT trampolini — yeni (immutable, ADR-024) string üretir. ──────
extern "C" void* rt_jit_string_concat(void* a, void* b) {
    const std::string& sa = static_cast<StringObject*>(a)->data;
    const std::string& sb = static_cast<StringObject*>(b)->data;
    return jitNewString(sa + sb);
}

// ── string ==/!= trampolini — ADR-023 istisnası: string eşitliği İÇERİK.
// VM'in EQUAL_EQUAL String dalıyla birebir (interpreter.cpp: stringValue==). ──
extern "C" int64_t rt_jit_string_eq(void* a, void* b) {
    return static_cast<StringObject*>(a)->data == static_cast<StringObject*>(b)->data ? 1 : 0;
}

// JIT runtime hatalarının çıkış kodu. VM yakalanmamış runtime hatasında
// kSoftwareError (70) döndürür; JIT aynı sözleşmeye uymalıdır (VM normatif).
// Daha önce burada hard-coded 1 vardı — merkezi 0/64/65/70 sınıfı dışıydı.
constexpr int kJitRuntimeErrorExit = saqut::exit_code::kSoftwareError;

extern "C" void rt_jit_cast_begin(int64_t nullableMode) {
    rt().castNullable = nullableMode != 0;
    rt().castNull = 0;
}

extern "C" int64_t rt_jit_cast_ret_is_null() {
    int64_t result = rt().castNull;
    rt().castNullable = false;
    return result;
}

// ── Cast trampolinleri (Dilim 3). Hepsi non-nullable hedef; başarısızlık
// uncaught (try/catch JIT'te yok) → rt_jit_cast_error, VM'in uncaught-throw
// mesaj gövdesiyle birebir (interpreter.cpp CAST_* dalları). ──────────────────
extern "C" void rt_jit_cast_error(const char* what) {
    if (rt().castNullable) {
        rt().castNull = 1;
        return;
    }
    jitSetError(what, "E_CAST");
}
extern "C" void* rt_jit_int_to_str(int64_t v) {
    return jitNewString(std::to_string(v));
}
extern "C" void* rt_jit_float_to_str(double v) {
    // #114: biçim tek kaynaktan (core/float_format.hpp) — VM cast sözleşmesi
    return jitNewString(formatDoubleCast(v));
}
extern "C" void* rt_jit_bool_to_str(int64_t v) {
    return jitNewString(v ? "true" : "false");
}
// ADR-040: longint (int64) → string. VM CAST_LONG_TO_STR ile birebir (to_string).
extern "C" void* rt_jit_long_to_str(int64_t v) {
    return jitNewString(std::to_string(v));
}
// ADR-040: float32 → string. VM CAST_FLOAT32_TO_STR ile birebir (setprecision 9).
// Argüman gerçek single (MIR_T_F) — F2D genişletmesi olmadan doğrudan.
extern "C" void* rt_jit_float32_to_str(float v) {
    // #114: biçim tek kaynaktan (core/float_format.hpp) — VM cast sözleşmesi
    return jitNewString(formatFloat32Cast(v));
}
extern "C" int64_t rt_jit_str_to_int(void* s) {
    const std::string& str = static_cast<StringObject*>(s)->data;
    try {
        size_t pos;
        long long v = std::stoll(str, &pos);
        if (pos != str.size()) throw std::invalid_argument("incomplete");
        if (v < INT_MIN || v > INT_MAX) throw std::out_of_range("overflow");
        return static_cast<int64_t>(static_cast<int>(v));
    } catch (...) {
        rt_jit_cast_error(("'" + str + "' cannot convert to int").c_str());
        return 0;  // ulaşılmaz (exit)
    }
}
extern "C" double rt_jit_str_to_float(void* s) {
    const std::string& str = static_cast<StringObject*>(s)->data;
    try {
        size_t pos;
        double v = std::stod(str, &pos);
        if (pos != str.size()) throw std::invalid_argument("incomplete");
        return v;
    } catch (...) {
        rt_jit_cast_error(("'" + str + "' cannot convert to float").c_str());
        return 0.0;  // ulaşılmaz
    }
}
extern "C" int64_t rt_jit_float_to_int_checked(double fv) {
    if (!std::isfinite(fv) || fv < static_cast<double>(INT_MIN) || fv > static_cast<double>(INT_MAX))
        rt_jit_cast_error("float value out of int range or NaN/Inf");
    return static_cast<int64_t>(static_cast<int>(fv));  // sıfıra kırp
}
extern "C" int64_t rt_jit_int_to_byte_checked(int64_t iv) {
    if (iv < 0 || iv > 255)
        rt_jit_cast_error(("integer value " + std::to_string(iv) +
                           " out of byte range (0-255)").c_str());
    return iv;
}

// ADR-040: string→longint / string→float32 / longint→int / float→longint.
// VM'in CAST_STR_TO_LONG / CAST_STR_TO_FLOAT32 / LONG_TO_INT_CHECKED /
// CAST_FLOAT_TO_LONG_CHECKED dallarıyla birebir (interpreter.cpp).
extern "C" int64_t rt_jit_str_to_long(void* s) {
    const std::string& str = static_cast<StringObject*>(s)->data;
    try {
        size_t pos;
        long long v = std::stoll(str, &pos);
        if (pos != str.size()) throw std::invalid_argument("incomplete");
        return static_cast<int64_t>(v);
    } catch (...) {
        rt_jit_cast_error(("'" + str + "' cannot convert to longint").c_str());
        return 0;  // ulaşılmaz (exit)
    }
}
extern "C" float rt_jit_str_to_float32(void* s) {
    const std::string& str = static_cast<StringObject*>(s)->data;
    try {
        size_t pos;
        float v = std::stof(str, &pos);
        if (pos != str.size()) throw std::invalid_argument("incomplete");
        return v;
    } catch (...) {
        rt_jit_cast_error(("'" + str + "' cannot convert to float").c_str());
        return 0.0f;  // ulaşılmaz
    }
}
extern "C" int64_t rt_jit_long_to_int_checked(int64_t lv) {
    if (lv < INT_MIN || lv > INT_MAX)
        rt_jit_cast_error(("longint value " + std::to_string(lv) +
                           " out of int range").c_str());
    return static_cast<int64_t>(static_cast<int>(lv));
}
// Argüman her zaman double (float32 kaynak call-site'ta F2D ile genişletilir,
// rt_jit_print_float32 desenindeki gibi — MIR call'da MIR_T_F/MIR_T_D karışımı
// operand tip uyuşmazlığına düşer, bkz. opcodeSupported/codegen notu).
extern "C" int64_t rt_jit_float_to_long_checked(double fv) {
    if (!std::isfinite(fv) || fv < -9223372036854775808.0 || fv >= 9223372036854775808.0)
        rt_jit_cast_error("float value out of longint range or NaN/Inf");
    return static_cast<int64_t>(fv);  // sıfıra kırp
}

// ── Decimal trampolinleri (Dilim 3, ADR-037: decimal her zaman kutulu). Değerler
// VM heap'inde (rt().heap->allocDecimal; shadow stack ile GC'ye görünür). Hata
// mesajları VM'in D* / CAST_*_DECIMAL dallarıyla birebir. ────────────────────
DecimalValue& jitDV(void* p) { return static_cast<DecimalObject*>(p)->val; }
DecimalObject* jitBoxDecimal(const DecimalValue& v) {
    if (rt().heap) return rt().heap->allocDecimal(v);
    auto& fallback = rt().decimalFallback;
    fallback.push_back(std::make_unique<DecimalObject>(v));
    return fallback.back().get();
}
extern "C" void* rt_jit_decimal_add(void* a, void* b) {
    auto r = DecimalValue::add(jitDV(a), jitDV(b));
    if (r.isOverflow()) rt_jit_cast_error("decimal overflow");
    return jitBoxDecimal(r);
}
extern "C" void* rt_jit_decimal_sub(void* a, void* b) {
    auto r = DecimalValue::sub(jitDV(a), jitDV(b));
    if (r.isOverflow()) rt_jit_cast_error("decimal overflow");
    return jitBoxDecimal(r);
}
extern "C" void* rt_jit_decimal_mul(void* a, void* b) {
    auto r = DecimalValue::mul(jitDV(a), jitDV(b));
    if (r.isOverflow()) rt_jit_cast_error("decimal overflow");
    return jitBoxDecimal(r);
}
extern "C" void* rt_jit_decimal_div(void* a, void* b) {
    if (jitDV(b).coeff == 0) rt_jit_cast_error("decimal division by zero");
    return jitBoxDecimal(DecimalValue::div(jitDV(a), jitDV(b)));
}

// ── decimal karşılaştırma trampolini ────────────────────────────────────────
// Decimal JIT'te KUTULU pointer olarak taşınır (ADR-037). Native MIR_EQ/MIR_LT
// bu pointer'ları karşılaştırır — yani ADRES eşitliği. `0.1+0.2 == 0.3` iki
// ayrı kutu ürettiği için sessizce 0 döner; VM ise DecimalValue::compare ile
// DEĞER karşılaştırıp 1 döndürür (VM≡JIT parity ihlali, sessiz yanlış değer).
// String eşitliğinde (rt_jit_string_eq) aynı hata sınıfı kapatılmıştı, decimal
// atlanmıştı.
//
// Tek trampolin bütün aileyi besler: -1/0/1 döner, çağrı sitesi sonucu 0 ile
// karşılaştırıp ==, !=, <, <=, >, >= üretir — VM'in DecimalValue::compare
// tabanlı karşılaştırma dallarıyla birebir.
extern "C" int64_t rt_jit_decimal_cmp(void* a, void* b) {
    return static_cast<int64_t>(DecimalValue::compare(jitDV(a), jitDV(b)));
}
extern "C" void* rt_jit_decimal_mod(void* a, void* b) {
    if (jitDV(b).coeff == 0) rt_jit_cast_error("decimal modulo by zero");
    return jitBoxDecimal(DecimalValue::mod(jitDV(a), jitDV(b)));
}
extern "C" void* rt_jit_decimal_neg(void* a) { return jitBoxDecimal(DecimalValue::neg(jitDV(a))); }
extern "C" void* rt_jit_int_to_decimal(int64_t v)   { return jitBoxDecimal(DecimalValue::fromInt(v)); }
extern "C" void* rt_jit_float_to_decimal(double v)  { return jitBoxDecimal(DecimalValue::fromDouble(v)); }
extern "C" void* rt_jit_decimal_to_str(void* d) {
    return jitNewString(jitDV(d).toString());
}
extern "C" int64_t rt_jit_decimal_to_int(void* d) {
    DecimalValue t = DecimalValue::truncate(jitDV(d));
    if (t.coeff < INT_MIN || t.coeff > INT_MAX)
        rt_jit_cast_error("decimal value out of int range");
    return static_cast<int64_t>(static_cast<int>(t.coeff));
}
extern "C" double rt_jit_decimal_to_float(void* d) { return jitDV(d).toDouble(); }
extern "C" void* rt_jit_str_to_decimal(void* s) {
    const std::string& str = static_cast<StringObject*>(s)->data;
    try {
        return jitBoxDecimal(DecimalValue::fromString(str));
    } catch (...) {
        rt_jit_cast_error(("'" + str + "' cannot convert to decimal").c_str());
        return nullptr;  // ulaşılmaz
    }
}
extern "C" void rt_jit_print_decimal(void* d) { std::cout << jitDV(d).toString() << std::flush; }

// Sıfıra bölme — bu Dilim'de try/catch (ENTER_TRY/THROW) reddedildiğinden
// yakalanamaz; VM'de de aynı program uncaught throw ile sonlanırdı.
//
// Mesaj ve çıkış kodu VM ile BİREBİR eşleşmelidir (VM normatif, AGENTS.md §8:
// "exit code, stdout ve stderr ayrı sözleşmelerdir"). Ölçülen VM davranışı:
//     a/b  → "runtime error: division by zero"        exit 70
//     a%b  → "runtime error: sıfıra bölme (mod)"      exit 70
//     f/f  → "runtime error: float division by zero"  exit 70
// Önceki gerçekleme mod mesajını ASCII'ye düşürüyor ("sifira bolme") ve
// üçünde de exit 1 döndürüyordu — merkezi kSoftwareError (70) sınıfı ihlali.
// Sıfıra bölme hataları. Konum PARAMETRE olarak gelir; eskiden her bölme
// talimatından ÖNCE koşulsuz bir rt_jit_error_location çağrısı emit ediliyordu
// ve hata olmasa bile tur başına bir çağrı ödeniyordu. Bölme kodgen'i zaten
// inline sıfır kontrolü yapıyor — konumu yalnız hata DALINDA yazmak yeterli.
extern "C" void rt_jit_div_zero(int64_t line, int64_t col) {
    jitSetError("division by zero", "E_DIVZERO", line, col);
}

extern "C" void rt_jit_mod_zero(int64_t line, int64_t col) {
    jitSetError("sıfıra bölme (mod)", "E_DIVZERO", line, col);
}

extern "C" void rt_jit_fdiv_zero(int64_t line, int64_t col) {
    jitSetError("float division by zero", "E_DIVZERO", line, col);
}

// SlotType → MIR register tipi (MIRPLAN §3; ADR-040 genişletmesi).
//   Float   → MIR_T_D (64-bit double)
//   Float32 → MIR_T_F (32-bit single — gerçek precision, VM ile birebir)
//   LongInt → MIR_T_I64 (int gibi ama EXT32 yok, tam 64-bit)
//   diğer (Int/Str/Decimal/…) → MIR_T_I64
MIR_type_t mirType(SlotType t) {
    if (t == SlotType::Float)   return MIR_T_D;
    if (t == SlotType::Float32) return MIR_T_F;
    return MIR_T_I64;
}

// Bir fonksiyonun dönüş türü — ilk RETURN'ün src slot türünden (tip denetleyici
// tüm RETURN'lerin aynı türde olduğunu garanti eder; Dilim 1 void RETURN'ü
// zaten reddediyor).
SlotType retKind(const IRFunction& fn) {
    for (const auto& ins : fn.instructions)
        if (ins.opcode == Opcode::RETURN && ins.src >= 0 &&
            ins.src < static_cast<int>(fn.slotTypes.size()))
            return fn.slotTypes[static_cast<size_t>(ins.src)];
    return SlotType::Int;
}

// Slot türünü güvenli oku (tablo eksikse/aralık dışıysa Int).
SlotType slotKindOf(const IRFunction& fn, int slot) {
    if (slot >= 0 && slot < static_cast<int>(fn.slotTypes.size()))
        return fn.slotTypes[static_cast<size_t>(slot)];
    return SlotType::Int;
}

bool isSupportedCallhost(const Instruction& instr, const std::vector<bool>&) {
    if ((int)instr.argSlots.size() > JitRuntime::kMaxHostArgs) return false;
    const HostEntry* he = hostEntryAt(instr.intValue);
    if (!he || !he->thunk) return false;
    // Capability (HOST_NEEDS_CAPS) host çağrılarını JIT'te ARTIK reddetmeyiz:
    // JIT kendi HostEnv'i (jitCaps/jitHeap) üzerinden host gövdesini çalıştırır.
    // Capability modeli ayrı bir karar alanıdır (Adım 3); burada fs/sys host'ları
    // JIT köprüsüne açılır.
    // Dönüş türü ELEMAN TİPİNE bağlı olan metodlar reddedilir: registry'nin
    // retKind'i statik bir değerdir (Int), oysa gerçek tür receiver'ın eleman
    // tipidir. Trampolin ham 64-bit taşıdığı için float/string elemanlı bir
    // array'de pop() yanlış yorumlanır.
    //
    // Ölçüldü: array_float VM "33.52" / JIT "302",
    //          array_string VM "12worldworld1" / JIT "12091740481".
    // Eleman-tipli dönüşler (pop/remove) artık valueType üzerinden çözülüyor;
    // Unknown kalırsa tür bilinmiyor demektir.
    static const char* kElemTypedReturns[] = {"pop", "remove"};
    for (const char* n : kElemTypedReturns)
        if (std::strcmp(he->symbolicId, n) == 0 && instr.valueType == SlotType::Unknown)
            return false;
    return true;
}

bool opcodeSupported(const Instruction& instr, const std::vector<bool>& fnNullable) {
    // Temel destek OPCODE_LIST spec tablosundan türetilir (opcodeJitBaseSupported,
    // #132). Talimata bağlı ek koşullar — nullable hedef (instr.left==1 →
    // başarısızlıkta null; null'un register temsili JIT'te ayrı tasarım turu,
    // yalnızca non-nullable hedef desteklenir: başarısızlık = uncaught throw,
    // VM ile aynı), print-only CALLHOST, void RETURN — burada uygulanır.
    if (!opcodeJitBaseSupported(instr.opcode))
        return false;
    switch (instr.opcode) {
        // Fallible cast'ler — nullable hedef → reddet
        case Opcode::CAST_STR_TO_INT:
        case Opcode::CAST_STR_TO_FLOAT:
        case Opcode::CAST_FLOAT_TO_INT_CHECKED:
        case Opcode::CAST_INT_TO_BYTE_CHECKED:
        case Opcode::CAST_STR_TO_LONG:
        case Opcode::CAST_STR_TO_FLOAT32:
        case Opcode::CAST_FLOAT_TO_LONG_CHECKED:
        case Opcode::LONG_TO_INT_CHECKED:
        case Opcode::CAST_DECIMAL_TO_INT:
        case Opcode::CAST_STR_TO_DECIMAL:
            return true;
        case Opcode::RETURN:
            return instr.src >= 0;  // void RETURN (src=-1) bu dilimde yok
        case Opcode::ARRAY_GET:
        case Opcode::FIELD_GET:
            // valueType (ADR-039) eleman/alan türünü taşır. Unknown ise tür
            // IR'de kaybolmuştur ve JIT hangi register genişliğini kullanacağını
            // bilemez — ham 64-bit okumak string/float elemanlarda yanlış
            // sonuç verir (ölçüldü: string[] elemanı "3abc" yerine "3000").
            //
            // finalizeSlotTypes bu opcode'ların dest türünü çözmüyor (kodda
            // TODO). Çözülene dek yalnızca türü kesin olanlar JIT'te.
            return instr.valueType != SlotType::Unknown;
        case Opcode::CALLHOST:
            return isSupportedCallhost(instr, fnNullable);
        default:
            return true;
    }
}

// Programın TAMAMINI tarar: (a) her opcode desteklenmeli, (b) her slot türü
// Int/Float olmalı (Ref/Str/Decimal/Date register'a sığmaz — sonraki dilimler).
bool wholeProgramSupported(IRProgram& program, UnsupportedReason& outReason) {
    for (auto& name : program.functionOrder) {
        IRFunction& fn = program.functions.at(name);
        for (auto& instr : fn.instructions) {
            if (!opcodeSupported(instr, fn.slotNullable)) {
                outReason.functionName = name;
                outReason.opcodeName   = opcodeName(instr.opcode);
                return false;
            }
            if (instr.opcode == Opcode::LOAD_GLOBAL || instr.opcode == Opcode::STORE_GLOBAL) {
                int slot = instr.opcode == Opcode::LOAD_GLOBAL ? instr.dest : instr.src;
                SlotType t = slotKindOf(fn, slot);
                if (t != SlotType::Int && t != SlotType::LongInt &&
                    t != SlotType::Float && t != SlotType::Float32 &&
                    t != SlotType::Str && t != SlotType::Decimal &&
                    t != SlotType::Ref) {
                    outReason.functionName = name;
                    outReason.opcodeName = std::string(opcodeName(instr.opcode)) +
                                           " <boxed global>";
                    return false;
                }
            }
            // String operandlı SIRALAMA (</<=/>/>=) JIT'te desteklenmez —
            // zaten frontend'de reddedilir (E003: "for string use only == and
            // !="), bu yalnızca savunmacı bir kalkan. Eşitlik (==/!=) İÇERİK
            // karşılaştırmasıdır (ADR-023 istisnası) ve codegen'de rt_jit_string_eq
            // runtime call'a çevrilir (native MIR_EQ pointer eşitliği YANLIŞ olurdu).
            switch (instr.opcode) {
                case Opcode::LESS:    case Opcode::LESS_EQUAL:
                case Opcode::GREATER: case Opcode::GREATER_EQUAL:
                    if (slotKindOf(fn, instr.left) == SlotType::Str ||
                        slotKindOf(fn, instr.right) == SlotType::Str) {
                        outReason.functionName = name;
                        outReason.opcodeName =
                            std::string(opcodeName(instr.opcode)) + " <string operand>";
                        return false;
                    }
                    break;
                default:
                    break;
            }
        }
        // #221: nullable slot'lar yalnızca null-farkındalıklı opcode'larda
        // tüketilebilir. Bu dilimde yalnız EQUAL_EQUAL/NOT_EQUAL (ve slot'a
        // yazan LOAD_NULL/LOAD_SLOT) null'u doğru işler.
        //
        // NEDEN REDDEDİYORUZ: nullable bir slot'u ör. ADD'e verirsek, JIT
        // null'u 0 gibi toplar ve SESSİZCE yanlış sonuç üretir — VM ise
        // narrowing sayesinde oraya hiç gelmez. Eksik kapsam kabul edilebilir,
        // yanlış cevap değil (ADR-037: VM normatif).
        //
        // BİLİNEN EKSİK (narrowing): tip denetleyici `if (a != null) { int x = a; }`
        // içinde a'nın non-null olduğunu KANITLAR (ADR-021 akış analizi), ama bu
        // bilgi IR'ye inmez — slotNullable hâlâ true kalır ve buradaki kalkan
        // gereksiz yere reddeder. Etkilenen: golden/null/narrowing.sqt,
        // and_narrowing.sqt. Çözüm, narrowing sonucunu IR'de taşımak (ayrı iş);
        // o zamana kadar bu iki fixture VM'de kalır — yanlış cevap değil, eksik
        // kapsam.
        for (const auto& instr : fn.instructions) {
            auto usesNullable = [&](int slot) {
                return slot >= 0 && slot < (int)fn.slotNullable.size() &&
                       fn.slotNullable[static_cast<size_t>(slot)];
            };
            switch (instr.opcode) {
                case Opcode::EQUAL_EQUAL:
                case Opcode::NOT_EQUAL:
                case Opcode::LESS:
                case Opcode::LESS_EQUAL:
                case Opcode::GREATER:
                case Opcode::GREATER_EQUAL:
                case Opcode::RETURN:
                case Opcode::FIELD_GET:
                case Opcode::ARRAY_GET:
                case Opcode::ARRAY_SET:
                case Opcode::CAST_STR_TO_INT:
                case Opcode::CAST_STR_TO_FLOAT:
                case Opcode::CAST_FLOAT_TO_INT_CHECKED:
                case Opcode::CAST_INT_TO_BYTE_CHECKED:
                case Opcode::CAST_STR_TO_LONG:
                case Opcode::CAST_STR_TO_FLOAT32:
                case Opcode::CAST_FLOAT_TO_LONG_CHECKED:
                case Opcode::LONG_TO_INT_CHECKED:
                case Opcode::CAST_DECIMAL_TO_INT:
                case Opcode::CAST_STR_TO_DECIMAL:
                case Opcode::CAST_INT_TO_STR:
                case Opcode::CAST_FLOAT_TO_STR:
                case Opcode::CAST_FLOAT32_TO_STR:
                case Opcode::CAST_LONG_TO_STR:
                case Opcode::CAST_BOOL_TO_STR:
                case Opcode::CAST_DECIMAL_TO_STR:
                case Opcode::LOAD_NULL:
                case Opcode::LOAD_SLOT:
                    break;  // null-farkındalıklı
                default:
                    if (usesNullable(instr.left) || usesNullable(instr.right) ||
                        usesNullable(instr.src)) {
                        outReason.functionName = name;
                        outReason.opcodeName =
                            std::string(opcodeName(instr.opcode)) + " <nullable operand>";
                        return false;
                    }
                    break;
            }
        }

        for (SlotType st : fn.slotTypes) {
            // Int/LongInt/Float/Float32 register-skaler; Str/Decimal kutulu
            // pointer (I64, ADR-037). Ref hâlâ sonraki dilimde (shadow stack).
            // #228: Ref artık destekleniyor — shadow stack GC görünürlüğünü
            // sağlıyor. Date hâlâ dışarıda (register temsili tanımlı değil).
            if (st != SlotType::Int && st != SlotType::LongInt &&
                st != SlotType::Float && st != SlotType::Float32 &&
                st != SlotType::Str && st != SlotType::Decimal &&
                st != SlotType::Ref && st != SlotType::Date) {
                outReason.functionName = name;
                outReason.opcodeName =
                    std::string("<desteklenmeyen slot turu: ") + slotTypeName(st) + ">";
                return false;
            }
        }
    }
    return true;
}

struct FuncEntry {
    MIR_item_t callRef;    // CALL hedefi (önce forward, gövde açılınca gerçek func)
    MIR_item_t protoItem;
    int        paramCount;
};

}  // namespace

bool tryCompileAndRunProgram(IRProgram& program, int& outExitCode,
                              UnsupportedReason& outReason,
                              const std::vector<std::string>& programArgs,
                              profiling::StageTimer* profiler,
                              JitCallCounters* counters,
                              int executionRuns,
                              std::vector<long long>* executionSamplesUs,
                              const std::function<void(int, int)>& executionProgress) {
    if (!wholeProgramSupported(program, outReason)) return false;

    // Bench sayaçlarını aktif et (profiler ile bağımsız — sayaçlar profil
    // çalışmasında bile istenebilir; nullptr ise trampoline'lar atlar).
    struct BenchCountersGuard {
        JitCallCounters* prev;
        ~BenchCountersGuard() { rt().benchCounters = prev; }
    } benchGuard{ rt().benchCounters };
    rt().benchCounters = counters;

    // Bu koşunun heap'i. Ömrü KOŞUYA bağlıdır (süreç ömrüne değil): koşu
    // bitince yıkılır ve tahsis ettiği her nesne serbest kalır. Aynı süreçte
    // arka arkaya program çalıştıran gömülü kullanım için bu şarttır.
    Heap runHeap;
    if (const int gcThreshold = gcThresholdForNextRunStorage(); gcThreshold != 0) {
        if (gcThreshold > 0) runHeap.setMinCollectBytes(gcThreshold);
        else                 runHeap.setCollectionEnabled(false);
    }

    // Bağlanan her şey koşu sonunda çözülmelidir — heap yığında olduğundan
    // ona işaret eden global bağlar (kök sağlayıcı kaydı, string kancası,
    // host ortamı) heap'ten uzun yaşarsa serbest bırakılmış belleğe bakar.
    struct RunHeapBinding {
        Heap* heap = nullptr;
        ~RunHeapBinding() {
            if (heap) lastRunGcStatsStorage() = heap->stats();
            jitSetHeap(nullptr);
            setValueStringHeap(nullptr);
            jitShadowStack().clear();
            rt().pendingError = nullptr;
            rt().globalP.clear();
        }
    } runHeapBinding{ &runHeap };

    static HostEnv jitEnv;
    jitEnv.programArgs = &programArgs;
    jitEnv.heap        = &runHeap;
    jitSetHostEnv(&jitEnv);
    jitSetHeap(&runHeap);
    // Tek string modeli: Value::fromString de aynı heap'e tahsis etsin —
    // VM ve JIT aynı string dünyasını paylaşır.
    setValueStringHeap(&runHeap);
    rt().globalI.assign((size_t)program.globalCount, 0);
    rt().globalD.assign((size_t)program.globalCount, 0.0);
    rt().globalP.assign((size_t)program.globalCount, nullptr);
    std::fill(std::begin(rt().callNullArgs), std::end(rt().callNullArgs), 0);
    rt().callRetNull = 0;
    rt().castNullable = false;
    rt().castNull = 0;
    rt().pendingError = nullptr;
    rt().errorLine = 0;
    rt().errorCol = 0;
    jitShadowStack().clear();
    rt().structMeta.clear();
    rt().traceStack.clear();

    if (program.findFunction("main") == nullptr) {
        outReason.functionName = "main";
        outReason.opcodeName   = "(fonksiyon bulunamadi)";
        return false;
    }

    // "jit-warmup" — IR->MIR çeviri + gerçek native derleme (MIR_gen dahil).
    // compiled() çağrısı bu kapsamın DIŞINDA ("jit-exec"); RAII kapsamı
    // compiled()'dan hemen önce reset() ile kapatılır.
    std::optional<profiling::StageTimer::ScopedStage> profWarmup;
    profWarmup.emplace(profiler, "jit-warmup");

    MIR_context_t ctx = MIR_init();
    MIR_module_t  mod = MIR_new_module(ctx, "saqut_jit_dilim1");

    // ── print/fatal-hata trampolinleri (dış C fonksiyonları) ────────────
    MIR_item_t printProto      = MIR_new_proto(ctx, "print_proto", 0, nullptr, 1, MIR_T_I64, "v");
    MIR_item_t printImport     = MIR_new_import(ctx, "rt_jit_print_int");
    MIR_item_t printFProto     = MIR_new_proto(ctx, "print_f_proto", 0, nullptr, 1, MIR_T_D, "v");
    MIR_item_t printFImport    = MIR_new_import(ctx, "rt_jit_print_float");
    // ADR-040: float32 print (arg F2D ile double'a genişletilir → MIR_T_D)
    MIR_item_t printF32Proto   = MIR_new_proto(ctx, "print_f32_proto", 0, nullptr, 1, MIR_T_D, "v");
    MIR_item_t printF32Import  = MIR_new_import(ctx, "rt_jit_print_float32");
    MIR_item_t printSProto     = MIR_new_proto(ctx, "print_s_proto", 0, nullptr, 1, MIR_T_I64, "v");
    MIR_item_t printSImport    = MIR_new_import(ctx, "rt_jit_print_str");
    // STRING_CONCAT / string ==,!= runtime call'ları (ret I64 pointer/bool, 2×I64 arg)
    MIR_type_t i64Ret          = MIR_T_I64;
    MIR_var_t  strConcatArgs[2] = {{MIR_T_I64, "a", 0}, {MIR_T_I64, "b", 0}};
    MIR_item_t concatProto     = MIR_new_proto_arr(ctx, "str_concat_proto", 1, &i64Ret, 2, strConcatArgs);
    MIR_item_t concatImport    = MIR_new_import(ctx, "rt_jit_string_concat");
    MIR_var_t  strEqArgs[2]     = {{MIR_T_I64, "a", 0}, {MIR_T_I64, "b", 0}};
    MIR_item_t strEqProto      = MIR_new_proto_arr(ctx, "str_eq_proto", 1, &i64Ret, 2, strEqArgs);
    MIR_item_t strEqImport     = MIR_new_import(ctx, "rt_jit_string_eq");
    MIR_var_t  decCmpArgs[2]    = {{MIR_T_I64, "a", 0}, {MIR_T_I64, "b", 0}};
    MIR_item_t decCmpProto     = MIR_new_proto_arr(ctx, "dec_cmp_proto", 1, &i64Ret, 2, decCmpArgs);
    MIR_item_t decCmpImport    = MIR_new_import(ctx, "rt_jit_decimal_cmp");
    // Cast trampolinleri (Dilim 3). ret I64 (pointer/int) veya D; arg I64/D.
    MIR_type_t dRet            = MIR_T_D;
    MIR_item_t castI2SProto    = MIR_new_proto(ctx, "cast_i2s_proto", 1, &i64Ret, 1, MIR_T_I64, "v");
    MIR_item_t castI2SImport   = MIR_new_import(ctx, "rt_jit_int_to_str");
    MIR_item_t castF2SProto    = MIR_new_proto(ctx, "cast_f2s_proto", 1, &i64Ret, 1, MIR_T_D, "v");
    MIR_item_t castF2SImport   = MIR_new_import(ctx, "rt_jit_float_to_str");
    MIR_item_t castB2SProto    = MIR_new_proto(ctx, "cast_b2s_proto", 1, &i64Ret, 1, MIR_T_I64, "v");
    MIR_item_t castB2SImport   = MIR_new_import(ctx, "rt_jit_bool_to_str");
    // ADR-040: longint→str (arg I64), float32→str (arg MIR_T_F). ret I64 pointer.
    MIR_item_t castL2SProto    = MIR_new_proto(ctx, "cast_l2s_proto", 1, &i64Ret, 1, MIR_T_I64, "v");
    MIR_item_t castL2SImport   = MIR_new_import(ctx, "rt_jit_long_to_str");
    MIR_item_t castF322SProto  = MIR_new_proto(ctx, "cast_f322s_proto", 1, &i64Ret, 1, MIR_T_F, "v");
    MIR_item_t castF322SImport = MIR_new_import(ctx, "rt_jit_float32_to_str");
    MIR_item_t castS2IProto    = MIR_new_proto(ctx, "cast_s2i_proto", 1, &i64Ret, 1, MIR_T_I64, "v");
    MIR_item_t castS2IImport   = MIR_new_import(ctx, "rt_jit_str_to_int");
    MIR_item_t castS2FProto    = MIR_new_proto(ctx, "cast_s2f_proto", 1, &dRet, 1, MIR_T_I64, "v");
    MIR_item_t castS2FImport   = MIR_new_import(ctx, "rt_jit_str_to_float");
    MIR_item_t castF2IProto    = MIR_new_proto(ctx, "cast_f2i_proto", 1, &i64Ret, 1, MIR_T_D, "v");
    MIR_item_t castF2IImport   = MIR_new_import(ctx, "rt_jit_float_to_int_checked");
    MIR_item_t castI2BProto    = MIR_new_proto(ctx, "cast_i2b_proto", 1, &i64Ret, 1, MIR_T_I64, "v");
    MIR_item_t castI2BImport   = MIR_new_import(ctx, "rt_jit_int_to_byte_checked");
    // ADR-040: str→longint, str→float32 (ret F), longint→int, float→longint
    // (arg her zaman D — float32 kaynak call-site'ta F2D ile genişletilir).
    MIR_type_t fRet            = MIR_T_F;
    MIR_item_t castS2LProto    = MIR_new_proto(ctx, "cast_s2l_proto", 1, &i64Ret, 1, MIR_T_I64, "v");
    MIR_item_t castS2LImport   = MIR_new_import(ctx, "rt_jit_str_to_long");
    MIR_item_t castS2F32Proto  = MIR_new_proto(ctx, "cast_s2f32_proto", 1, &fRet, 1, MIR_T_I64, "v");
    MIR_item_t castS2F32Import = MIR_new_import(ctx, "rt_jit_str_to_float32");
    MIR_item_t castL2IProto    = MIR_new_proto(ctx, "cast_l2i_proto", 1, &i64Ret, 1, MIR_T_I64, "v");
    MIR_item_t castL2IImport   = MIR_new_import(ctx, "rt_jit_long_to_int_checked");
    MIR_item_t castF2LProto    = MIR_new_proto(ctx, "cast_f2l_proto", 1, &i64Ret, 1, MIR_T_D, "v");
    MIR_item_t castF2LImport   = MIR_new_import(ctx, "rt_jit_float_to_long_checked");
    MIR_item_t castBeginProto  = MIR_new_proto(ctx, "cast_begin_proto", 0, nullptr, 1, MIR_T_I64, "n");
    MIR_item_t castBeginImport = MIR_new_import(ctx, "rt_jit_cast_begin");
    MIR_item_t castNullProto   = MIR_new_proto_arr(ctx, "cast_null_proto", 1, &i64Ret, 0, nullptr);
    MIR_item_t castNullImport  = MIR_new_import(ctx, "rt_jit_cast_ret_is_null");
    // Decimal trampolinleri (Dilim 3). Kutulu → I64 pointer. Binary I64,I64→I64;
    // unary I64→I64; float→dec D→I64; dec→float I64→D.
    MIR_var_t  decBinArgs[2]   = {{MIR_T_I64, "a", 0}, {MIR_T_I64, "b", 0}};
    MIR_item_t decBinProto     = MIR_new_proto_arr(ctx, "dec_bin_proto", 1, &i64Ret, 2, decBinArgs);
    MIR_item_t decAddImport    = MIR_new_import(ctx, "rt_jit_decimal_add");
    MIR_item_t decSubImport    = MIR_new_import(ctx, "rt_jit_decimal_sub");
    MIR_item_t decMulImport    = MIR_new_import(ctx, "rt_jit_decimal_mul");
    MIR_item_t decDivImport    = MIR_new_import(ctx, "rt_jit_decimal_div");
    MIR_item_t decModImport    = MIR_new_import(ctx, "rt_jit_decimal_mod");
    MIR_item_t decUnIProto     = MIR_new_proto(ctx, "dec_uni_proto", 1, &i64Ret, 1, MIR_T_I64, "v");
    MIR_item_t decNegImport    = MIR_new_import(ctx, "rt_jit_decimal_neg");
    MIR_item_t decI2DImport    = MIR_new_import(ctx, "rt_jit_int_to_decimal");
    MIR_item_t decToStrImport  = MIR_new_import(ctx, "rt_jit_decimal_to_str");
    MIR_item_t decToIntImport  = MIR_new_import(ctx, "rt_jit_decimal_to_int");
    MIR_item_t decS2DImport    = MIR_new_import(ctx, "rt_jit_str_to_decimal");
    MIR_item_t decFromFProto   = MIR_new_proto(ctx, "dec_fromf_proto", 1, &i64Ret, 1, MIR_T_D, "v");
    MIR_item_t decF2DImport    = MIR_new_import(ctx, "rt_jit_float_to_decimal");
    MIR_item_t decToFProto     = MIR_new_proto(ctx, "dec_tof_proto", 1, &dRet, 1, MIR_T_I64, "v");
    MIR_item_t decToFImport    = MIR_new_import(ctx, "rt_jit_decimal_to_float");
        MIR_var_t  ssVars[3]       = {{MIR_T_I64, "b", 0}, {MIR_T_I64, "i", 0}, {MIR_T_I64, "o", 0}};
        MIR_item_t ssProto         = MIR_new_proto_arr(ctx, "ss_proto", 0, nullptr, 3, ssVars);
        MIR_item_t ssImport        = MIR_new_import(ctx, "rt_jit_shadow_set");
        MIR_item_t ssEnterProto    = MIR_new_proto(ctx, "ss_enter_proto", 1, &i64Ret, 0);
        MIR_item_t ssEnterImport   = MIR_new_import(ctx, "rt_jit_shadow_enter");
        MIR_item_t ssLeaveProto    = MIR_new_proto(ctx, "ss_leave_proto", 0, nullptr, 1, MIR_T_I64, "b");
        MIR_item_t ssLeaveImport   = MIR_new_import(ctx, "rt_jit_shadow_leave");
    MIR_var_t  anewVars[2]     = {{MIR_T_I64, "c", 0}, {MIR_T_I64, "k", 0}};
    MIR_item_t anewProto       = MIR_new_proto_arr(ctx, "anew_proto", 1, &i64Ret, 2, anewVars);
    MIR_item_t anewImport      = MIR_new_import(ctx, "rt_jit_array_new");
    MIR_item_t alenProto       = MIR_new_proto(ctx, "alen_proto", 1, &i64Ret, 1, MIR_T_I64, "a");
    MIR_item_t alenImport      = MIR_new_import(ctx, "rt_jit_array_len");
    MIR_var_t  abndVars[2]     = {{MIR_T_I64, "i", 0}, {MIR_T_I64, "l", 0}};
    MIR_item_t abndProto       = MIR_new_proto_arr(ctx, "abnd_proto", 0, nullptr, 2, abndVars);
    MIR_item_t abndImport      = MIR_new_import(ctx, "rt_jit_array_bounds_fail");
    MIR_var_t  agetVars[2]     = {{MIR_T_I64, "a", 0}, {MIR_T_I64, "i", 0}};
    MIR_item_t agetIProto      = MIR_new_proto_arr(ctx, "agi_proto", 1, &i64Ret, 2, agetVars);
    MIR_item_t agetIImport     = MIR_new_import(ctx, "rt_jit_array_get_i");
    MIR_item_t agetDProto      = MIR_new_proto_arr(ctx, "agd_proto", 1, &dRet, 2, agetVars);
    MIR_item_t agetDImport     = MIR_new_import(ctx, "rt_jit_array_get_d");
    MIR_item_t agetPProto      = MIR_new_proto_arr(ctx, "agp_proto", 1, &i64Ret, 2, agetVars);
    MIR_item_t agetPImport     = MIR_new_import(ctx, "rt_jit_array_get_p");
    MIR_var_t  asetIVars[3]    = {{MIR_T_I64, "a", 0}, {MIR_T_I64, "i", 0}, {MIR_T_I64, "v", 0}};
    MIR_item_t asetIProto      = MIR_new_proto_arr(ctx, "asi_proto", 0, nullptr, 3, asetIVars);
    MIR_item_t asetIImport     = MIR_new_import(ctx, "rt_jit_array_set_i");
    MIR_var_t  asetDVars[3]    = {{MIR_T_I64, "a", 0}, {MIR_T_I64, "i", 0}, {MIR_T_D, "v", 0}};
    MIR_item_t asetDProto      = MIR_new_proto_arr(ctx, "asd_proto", 0, nullptr, 3, asetDVars);
    MIR_item_t asetDImport     = MIR_new_import(ctx, "rt_jit_array_set_d");
    MIR_item_t asetPProto      = MIR_new_proto_arr(ctx, "asp_proto", 0, nullptr, 3, asetIVars);
    MIR_item_t asetPImport     = MIR_new_import(ctx, "rt_jit_array_set_p");
    MIR_var_t  snewVars[2]     = {{MIR_T_I64, "n", 0}, {MIR_T_I64, "m", 0}};
    MIR_item_t snewProto       = MIR_new_proto_arr(ctx, "snew_proto", 1, &i64Ret, 2, snewVars);
    MIR_item_t snewImport      = MIR_new_import(ctx, "rt_jit_struct_new");
    MIR_var_t  fgetVars[2]     = {{MIR_T_I64, "o", 0}, {MIR_T_I64, "i", 0}};
    MIR_item_t fgetIProto      = MIR_new_proto_arr(ctx, "fgi_proto", 1, &i64Ret, 2, fgetVars);
    MIR_item_t fgetIImport     = MIR_new_import(ctx, "rt_jit_field_get_i");
    MIR_item_t fgetDProto      = MIR_new_proto_arr(ctx, "fgd_proto", 1, &dRet, 2, fgetVars);
    MIR_item_t fgetDImport     = MIR_new_import(ctx, "rt_jit_field_get_d");
    MIR_item_t fgetPProto      = MIR_new_proto_arr(ctx, "fgp_proto", 1, &i64Ret, 2, fgetVars);
    MIR_item_t fgetPImport     = MIR_new_import(ctx, "rt_jit_field_get_p");
    MIR_item_t fgetNullProto   = MIR_new_proto_arr(ctx, "fgn_proto", 1, &i64Ret, 2, fgetVars);
    MIR_item_t fgetNullImport  = MIR_new_import(ctx, "rt_jit_field_is_null");
    MIR_var_t  fsetIVars[3]    = {{MIR_T_I64, "o", 0}, {MIR_T_I64, "i", 0}, {MIR_T_I64, "v", 0}};
    MIR_item_t fsetIProto      = MIR_new_proto_arr(ctx, "fsi_proto", 0, nullptr, 3, fsetIVars);
    MIR_item_t fsetIImport     = MIR_new_import(ctx, "rt_jit_field_set_i");
    MIR_var_t  fsetDVars[3]    = {{MIR_T_I64, "o", 0}, {MIR_T_I64, "i", 0}, {MIR_T_D, "v", 0}};
    MIR_item_t fsetDProto      = MIR_new_proto_arr(ctx, "fsd_proto", 0, nullptr, 3, fsetDVars);
    MIR_item_t fsetDImport     = MIR_new_import(ctx, "rt_jit_field_set_d");
    MIR_item_t fsetPProto      = MIR_new_proto_arr(ctx, "fsp_proto", 0, nullptr, 3, fsetIVars);
    MIR_item_t fsetPImport     = MIR_new_import(ctx, "rt_jit_field_set_p");

    MIR_var_t  hostArgIVars[3] = {{MIR_T_I64, "i", 0}, {MIR_T_I64, "k", 0}, {MIR_T_I64, "v", 0}};
    MIR_item_t hostArgIProto   = MIR_new_proto_arr(ctx, "host_arg_i_proto", 0, nullptr, 3, hostArgIVars);
    MIR_item_t hostArgIImport  = MIR_new_import(ctx, "rt_jit_host_arg_i");
    MIR_var_t  hostArgDVars[3] = {{MIR_T_I64, "i", 0}, {MIR_T_I64, "k", 0}, {MIR_T_D, "v", 0}};
    MIR_item_t hostArgDProto   = MIR_new_proto_arr(ctx, "host_arg_d_proto", 0, nullptr, 3, hostArgDVars);
    MIR_item_t hostArgDImport  = MIR_new_import(ctx, "rt_jit_host_arg_d");
    MIR_var_t hostArgNIVars[4] = {{MIR_T_I64, "i", 0}, {MIR_T_I64, "k", 0},
                                  {MIR_T_I64, "v", 0}, {MIR_T_I64, "n", 0}};
    MIR_item_t hostArgNIProto = MIR_new_proto_arr(ctx, "host_arg_ni_proto", 0, nullptr, 4, hostArgNIVars);
    MIR_item_t hostArgNIImport = MIR_new_import(ctx, "rt_jit_host_arg_nullable_i");
    MIR_var_t hostArgNDVars[4] = {{MIR_T_I64, "i", 0}, {MIR_T_I64, "k", 0},
                                  {MIR_T_D, "v", 0}, {MIR_T_I64, "n", 0}};
    MIR_item_t hostArgNDProto = MIR_new_proto_arr(ctx, "host_arg_nd_proto", 0, nullptr, 4, hostArgNDVars);
    MIR_item_t hostArgNDImport = MIR_new_import(ctx, "rt_jit_host_arg_nullable_d");
    MIR_var_t  hostCallVars[2] = {{MIR_T_I64, "e", 0}, {MIR_T_I64, "n", 0}};
    MIR_item_t hostCallProto   = MIR_new_proto_arr(ctx, "host_call_proto", 1, &i64Ret, 2, hostCallVars);
    MIR_item_t hostCallImport  = MIR_new_import(ctx, "rt_jit_host_call");
    MIR_item_t hostCallDProto  = MIR_new_proto_arr(ctx, "host_call_d_proto", 1, &dRet, 2, hostCallVars);
    MIR_item_t hostCallDImport = MIR_new_import(ctx, "rt_jit_host_call_d");
    MIR_item_t hostRetNullProto = MIR_new_proto_arr(ctx, "host_ret_null_proto", 1, &i64Ret, 0, nullptr);
    MIR_item_t hostRetNullImport = MIR_new_import(ctx, "rt_jit_host_ret_is_null");
    MIR_var_t callNullSetVars[2] = {{MIR_T_I64, "i", 0}, {MIR_T_I64, "n", 0}};
    MIR_item_t callArgNullSetProto = MIR_new_proto_arr(ctx, "call_arg_null_set_proto", 0, nullptr, 2, callNullSetVars);
    MIR_item_t callArgNullSetImport = MIR_new_import(ctx, "rt_jit_call_arg_null_set");
    MIR_var_t callNullGetVars[1] = {{MIR_T_I64, "i", 0}};
    MIR_item_t callArgNullGetProto = MIR_new_proto_arr(ctx, "call_arg_null_get_proto", 1, &i64Ret, 1, callNullGetVars);
    MIR_item_t callArgNullGetImport = MIR_new_import(ctx, "rt_jit_call_arg_null_get");
    MIR_item_t callRetNullSetProto = MIR_new_proto_arr(ctx, "call_ret_null_set_proto", 0, nullptr, 1, callNullGetVars);
    MIR_item_t callRetNullSetImport = MIR_new_import(ctx, "rt_jit_call_ret_null_set");
    MIR_item_t callRetNullGetProto = MIR_new_proto_arr(ctx, "call_ret_null_get_proto", 1, &i64Ret, 0, nullptr);
    MIR_item_t callRetNullGetImport = MIR_new_import(ctx, "rt_jit_call_ret_null_get");
    MIR_var_t globalIVars[2] = {{MIR_T_I64, "i", 0}, {MIR_T_I64, "v", 0}};
    MIR_item_t globalLoadIProto = MIR_new_proto_arr(ctx, "global_load_i_proto", 1, &i64Ret, 1, globalIVars);
    MIR_item_t globalLoadIImport = MIR_new_import(ctx, "rt_jit_global_load_i");
    MIR_item_t globalStoreIProto = MIR_new_proto_arr(ctx, "global_store_i_proto", 0, nullptr, 2, globalIVars);
    MIR_item_t globalStoreIImport = MIR_new_import(ctx, "rt_jit_global_store_i");
    MIR_var_t globalDVars[2] = {{MIR_T_I64, "i", 0}, {MIR_T_D, "v", 0}};
    MIR_item_t globalLoadDProto = MIR_new_proto_arr(ctx, "global_load_d_proto", 1, &dRet, 1, globalDVars);
    MIR_item_t globalLoadDImport = MIR_new_import(ctx, "rt_jit_global_load_d");
    MIR_item_t globalStoreDProto = MIR_new_proto_arr(ctx, "global_store_d_proto", 0, nullptr, 2, globalDVars);
    MIR_item_t globalStoreDImport = MIR_new_import(ctx, "rt_jit_global_store_d");
    MIR_item_t globalLoadPProto = MIR_new_proto_arr(ctx, "global_load_p_proto", 1, &i64Ret, 1, globalIVars);
    MIR_item_t globalLoadPImport = MIR_new_import(ctx, "rt_jit_global_load_p");
    MIR_item_t globalStorePProto = MIR_new_proto_arr(ctx, "global_store_p_proto", 0, nullptr, 2, globalIVars);
    MIR_item_t globalStorePImport = MIR_new_import(ctx, "rt_jit_global_store_p");

    MIR_item_t printDProto     = MIR_new_proto(ctx, "print_d_proto", 0, nullptr, 1, MIR_T_I64, "v");
    MIR_item_t printDImport    = MIR_new_import(ctx, "rt_jit_print_decimal");
    MIR_var_t  zeroErrVars[2]  = {{MIR_T_I64, "line", 0}, {MIR_T_I64, "col", 0}};
    MIR_item_t divZeroProto    = MIR_new_proto_arr(ctx, "divzero_proto", 0, nullptr, 2, zeroErrVars);
    MIR_item_t divZeroImport   = MIR_new_import(ctx, "rt_jit_div_zero");
    MIR_item_t modZeroProto    = MIR_new_proto_arr(ctx, "modzero_proto", 0, nullptr, 2, zeroErrVars);
    MIR_item_t modZeroImport   = MIR_new_import(ctx, "rt_jit_mod_zero");
    MIR_item_t fdivZeroProto   = MIR_new_proto_arr(ctx, "fdivzero_proto", 0, nullptr, 2, zeroErrVars);
    MIR_item_t fdivZeroImport  = MIR_new_import(ctx, "rt_jit_fdiv_zero");
    MIR_item_t errPendingProto = MIR_new_proto_arr(ctx, "err_pending_proto", 1, &i64Ret, 0, nullptr);
    MIR_item_t errPendingImport = MIR_new_import(ctx, "rt_jit_error_pending");
    MIR_var_t errorLocationVars[2] = {{MIR_T_I64, "l", 0}, {MIR_T_I64, "c", 0}};
    MIR_item_t errorLocationProto = MIR_new_proto_arr(ctx, "err_location_proto", 0, nullptr, 2, errorLocationVars);
    MIR_item_t errorLocationImport = MIR_new_import(ctx, "rt_jit_error_location");
    MIR_item_t errTakeProto = MIR_new_proto_arr(ctx, "err_take_proto", 1, &i64Ret, 0, nullptr);
    MIR_item_t errTakeImport = MIR_new_import(ctx, "rt_jit_error_take");
    MIR_var_t throwPVars[4] = {{MIR_T_I64, "v", 0}, {MIR_T_I64, "k", 0},
                               {MIR_T_I64, "l", 0}, {MIR_T_I64, "c", 0}};
    MIR_item_t throwPProto = MIR_new_proto_arr(ctx, "throw_p_proto", 0, nullptr, 4, throwPVars);
    MIR_item_t throwPImport = MIR_new_import(ctx, "rt_jit_throw_p");
    // İz (trace) çağrı trampolinleri: yalnızca try içeren fonksiyonlarda
    // (MIRPLAN §7.1) fonksiyon giriş/çıkışında çağrılır — deterministik
    // stacktrace'in fonksiyon zinciri (ADR-025).
    MIR_var_t  traceEnterVars[2] = {{MIR_T_I64, "n", 0}, {MIR_T_I64, "f", 0}};
    MIR_item_t traceEnterProto = MIR_new_proto_arr(ctx, "trace_enter_proto", 0, nullptr, 2, traceEnterVars);
    MIR_item_t traceEnterImport = MIR_new_import(ctx, "rt_jit_trace_enter");
    MIR_item_t traceLeaveProto = MIR_new_proto(ctx, "trace_leave_proto", 0, nullptr, 0);
    MIR_item_t traceLeaveImport = MIR_new_import(ctx, "rt_jit_trace_leave");

    // ── String sabit havuzu (Dilim 3, ADR-037). LOAD_STRING derleme zamanında
    // string'i kutular; StringObject* pointer'ı native koda int sabiti olarak
    // gömülür (JIT in-process, pointer geçerli). intern tablosu aynı içeriği
    // tek nesneye indirger → döngüde tekrar kutulama/leak yok. Nesneler bu
    // fonksiyon kapsamı boyunca (native compiled() çağrısı dahil) yaşar.
    // NOT: AOT (#81) bu yolu runtime call'a (rt_intern_string + string_data)
    // çevirmeli — farklı process'te derleme-zamanı host pointer'ı gömülemez.
    std::vector<std::unique_ptr<StringObject>>     stringPool;
    std::unordered_map<std::string, StringObject*> internTable;
    auto internString = [&](const std::string& s) -> StringObject* {
        auto it = internTable.find(s);
        if (it != internTable.end()) return it->second;
        stringPool.push_back(std::make_unique<StringObject>(s));
        StringObject* obj = stringPool.back().get();
        internTable.emplace(s, obj);
        return obj;
    };

    // ── Aşama 1: TÜM fonksiyonlar için proto + forward (ileri-referanslı
    // CALL çözümü, MIRPLAN §2). Tip-imzalar slotTypes'tan (MIRPLAN §3). ──
    std::unordered_map<std::string, FuncEntry> funcMap;
    for (auto& name : program.functionOrder) {
        IRFunction& fn = program.functions.at(name);

        std::vector<MIR_var_t>   argVars(static_cast<size_t>(fn.paramCount));
        std::vector<std::string> argNames(static_cast<size_t>(fn.paramCount));
        for (int i = 0; i < fn.paramCount; i++) {
            argNames[static_cast<size_t>(i)] = "arg" + std::to_string(i);
            argVars[static_cast<size_t>(i)]  =
                {mirType(slotKindOf(fn, i)), argNames[static_cast<size_t>(i)].c_str(), 0};
        }

        MIR_type_t ret = mirType(retKind(fn));
        MIR_item_t proto = MIR_new_proto_arr(ctx, (name + "_proto").c_str(), 1, &ret,
                                              static_cast<size_t>(fn.paramCount), argVars.data());
        MIR_item_t forward = MIR_new_forward(ctx, name.c_str());

        funcMap[name] = FuncEntry{forward, proto, fn.paramCount};
    }

    // ── Aşama 1.5: "canRaise" sabit-nokta analizi (#110, §7.1 sıfır-maliyet).
    // Bir fonksiyon ya doğrudan hata üretebilen bir talimat içerir ya da böyle
    // bir fonksiyonu çağırır → bu durumda hata onun ÇERÇEVESİNDEN de taşar ve
    // per-instruction pending kontrolü gerekir. Salt-skaler sıcak yol
    // (fibonacci gibi: yalnız ADD/SUB/CMP/CALL(kendine)/RETURN) canRaise=false
    // kalır → MIRPLAN §7.1 gereği ZERO ek talimat taşır.
    std::unordered_map<std::string, bool> canRaise;
    // Bir opcode ÇALIŞMA ZAMANINDA hata yayabilir mi (rt().pendingError'a
    // yazabilir mi)? Liste, hata üreten trampolinlerin çağrıldığı opcode'lardan
    // türetilmiştir; EKSİK bırakmak hatanın sessizce yutulması demektir
    // (yayılım kontrolü emit edilmez, hata bir sonraki safepoint'e kadar
    // görülmez). Fazla eklemek yalnız gereksiz kontrol maliyetidir.
    //
    // Kaynaklar (mir_backend.cpp):
    //   div/mod/fdiv sıfır       → rt_jit_div_zero / mod_zero / fdiv_zero
    //   dizi sınırı              → jitBoundsFail (array_get_*, array_set_*)
    //   host çağrısı             → rt_jit_host_call (thunk hatası)
    //   throw                    → rt_jit_throw_*
    //   decimal taşma/sıfır      → rt_jit_decimal_add/sub/mul/div/mod
    //                              ("decimal overflow" — DADD/DSUB/DMUL de
    //                              hata yayar; eskiden listede YOKTU)
    auto errorCapable = [](Opcode op) {
        switch (op) {
            case Opcode::DIV: case Opcode::MOD: case Opcode::FDIV:
            case Opcode::LDIV: case Opcode::LMOD: case Opcode::F32DIV:
            case Opcode::DADD: case Opcode::DSUB: case Opcode::DMUL:
            case Opcode::DDIV: case Opcode::DMOD:
            case Opcode::ARRAY_GET: case Opcode::ARRAY_SET:
            case Opcode::CALLHOST: case Opcode::THROW:
                return true;
            default:
                return false;
        }
    };
    auto fallibleCastOp = [](Opcode op) {
        switch (op) {
            case Opcode::CAST_STR_TO_INT: case Opcode::CAST_STR_TO_FLOAT:
            case Opcode::CAST_FLOAT_TO_INT_CHECKED: case Opcode::CAST_INT_TO_BYTE_CHECKED:
            case Opcode::CAST_STR_TO_LONG: case Opcode::CAST_STR_TO_FLOAT32:
            case Opcode::CAST_FLOAT_TO_LONG_CHECKED: case Opcode::LONG_TO_INT_CHECKED:
            case Opcode::CAST_DECIMAL_TO_INT: case Opcode::CAST_STR_TO_DECIMAL:
                return true;
            default:
                return false;
        }
    };
    for (auto& name : program.functionOrder)
        canRaise[name] = false;
    for (auto& name : program.functionOrder) {
        const IRFunction& fn = program.functions.at(name);
        for (const auto& in : fn.instructions) {
            if (errorCapable(in.opcode) || fallibleCastOp(in.opcode)) {
                canRaise[name] = true;
                break;
            }
        }
    }
    // Sabit nokta: çağrılan fonksiyon canRaise ise çağıran da canRaise.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& name : program.functionOrder) {
            if (canRaise[name]) continue;
            const IRFunction& fn = program.functions.at(name);
            for (const auto& in : fn.instructions) {
                if (in.opcode == Opcode::CALL) {
                    auto it = canRaise.find(in.functionName);
                    if (it != canRaise.end() && it->second) {
                        canRaise[name] = true;
                        changed = true;
                        break;
                    }
                }
            }
        }
    }

    // ── Aşama 2: her fonksiyonun gerçek gövdesini aç/doldur/kapat ────────
    for (auto& name : program.functionOrder) {
        IRFunction& fn = program.functions.at(name);

        std::vector<MIR_var_t>   argVars(static_cast<size_t>(fn.paramCount));
        std::vector<std::string> argNames(static_cast<size_t>(fn.paramCount));
        for (int i = 0; i < fn.paramCount; i++) {
            argNames[static_cast<size_t>(i)] = "arg" + std::to_string(i);
            argVars[static_cast<size_t>(i)]  =
                {mirType(slotKindOf(fn, i)), argNames[static_cast<size_t>(i)].c_str(), 0};
        }
        MIR_type_t ret = mirType(retKind(fn));
        MIR_item_t func = MIR_new_func_arr(ctx, name.c_str(), 1, &ret,
                                            static_cast<size_t>(fn.paramCount), argVars.data());
        funcMap.at(name).callRef = func;  // gövde açıldı — forward yerine gerçek item

        size_t instrN = fn.instructions.size();
        std::vector<int> handlerTarget(instrN, -1);
        std::vector<int> handlerErrorSlot(instrN, -1);
        std::vector<std::pair<int, int>> handlerStack;
        for (size_t hi = 0; hi < instrN; ++hi) {
            if (!handlerStack.empty()) {
                handlerTarget[hi] = handlerStack.back().first;
                handlerErrorSlot[hi] = handlerStack.back().second;
            }
            const Instruction& hin = fn.instructions[hi];
            if (hin.opcode == Opcode::ENTER_TRY)
                handlerStack.emplace_back(hin.jumpTarget, hin.dest);
            else if (hin.opcode == Opcode::LEAVE_TRY && !handlerStack.empty())
                handlerStack.pop_back();
        }

        // saQut slot'u -> MIR register'ı. Parametreler biçimsel argümanlar
        // (MIR_reg); geri kalan yerel register. Tip slotTypes'tan (MIRPLAN §3).
        std::vector<MIR_reg_t> regs(static_cast<size_t>(fn.slotCount));
        for (int i = 0; i < fn.paramCount; i++) {
            std::string argName = "arg" + std::to_string(i);
            regs[static_cast<size_t>(i)] = MIR_reg(ctx, argName.c_str(), func->u.func);
        }
        for (int i = fn.paramCount; i < fn.slotCount; i++) {
            std::string regName = "slot" + std::to_string(i);
            regs[static_cast<size_t>(i)] =
                MIR_new_func_reg(ctx, func->u.func, mirType(slotKindOf(fn, i)), regName.c_str());
        }

        // #221: nullable slot'lar için gizli "isNull" yandaş register'ı.
        // Bir Int register'ı 0 ile null'u ayıramaz (64 bitin tamamı geçerli
        // değer), bu yüzden null'luk AYRI bir bitte taşınır. VM'de bu sorun
        // yok — Value ayrıca `kind` taşır. Yalnızca nullable slot'lar için
        // ayrılır; diğerleri hiç etkilenmez (register baskısı yok).
        std::vector<MIR_reg_t> nullFlagRegs(static_cast<size_t>(fn.slotCount), 0);
        auto isNullableSlot = [&](int slot) -> bool {
            return slot >= 0 && slot < (int)fn.slotNullable.size() &&
                   fn.slotNullable[static_cast<size_t>(slot)];
        };
        for (int i = 0; i < fn.slotCount; i++) {
            if (!isNullableSlot(i)) continue;
            std::string flagName = "isnull" + std::to_string(i);
            nullFlagRegs[static_cast<size_t>(i)] =
                MIR_new_func_reg(ctx, func->u.func, MIR_T_I64, flagName.c_str());
        }

        // #165: instrN+1 — son etiket "fonksiyon sonu" (past-the-end) sentinel'idir.
        // IR generator, tüm yolları return eden switch'ten sonra JMP → instrN
        // emit eder (hiç yürütülmez ama hedef geçerli olmalı). labelAt[instrN]
        // bu hedefi karşılar; boyut instrN olursa out-of-bounds → segfault.
        std::vector<MIR_label_t> labelAt(instrN + 1);
        for (size_t i = 0; i <= instrN; i++) labelAt[i] = MIR_new_label(ctx);
        MIR_label_t propagateLabel = MIR_new_label(ctx);

        // Shadow-stack çerçevesi (shadow_stack.hpp enter/leave protokolü):
        // fonksiyon girişinde taban alınır, her çıkışta geri sarılır. Yalnızca
        // ref taşıyabilen slot'u (Ref/Str/Decimal) olan fonksiyonlarda —
        // salt-skaler fonksiyonlar (fibonacci gibi sıcak yollar) sıfır ek
        // yükle çalışır. Düz mutlak indeksli eski model recursive çağrıda
        // üst çerçevenin GC köklerini eziyordu (use-after-free).
        bool needsShadowFrame = false;
        for (int i = 0; i < fn.slotCount; i++) {
            SlotType st = slotKindOf(fn, i);
            if (st == SlotType::Ref || st == SlotType::Str || st == SlotType::Decimal) {
                needsShadowFrame = true;
                break;
            }
        }
        // Slot canlılık analizi (ir_liveness) — shadow-stack kök daraltma:
        // bir talimattan SONRA bir daha okunmayacak slot'un değeri köklenmez;
        // nesneyi başka hiçbir yol tutmuyorsa toplanması doğrudur. VM tarafının
        // (maybeCollect) aynı bilgidir — iki backend aynı analizi paylaşır.
        // try içeren fonksiyonlarda analiz muhafazakârdır (exact=false) ve
        // tüm emitler eskisi gibi yapılır.
        const SlotLiveness slotLive = computeSlotLiveness(fn);
        MIR_reg_t ssBaseReg = 0;
        if (needsShadowFrame) {
            ssBaseReg = MIR_new_func_reg(ctx, func->u.func, MIR_T_I64, "ssbase");
            MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 3,
                MIR_new_ref_op(ctx, ssEnterProto), MIR_new_ref_op(ctx, ssEnterImport),
                MIR_new_reg_op(ctx, ssBaseReg)));
        }

        // #110 iz (trace) çerçevesi: deterministik stacktrace (§4) için çağrı
        // zinciri. §7.1 gereği YALNIZCA try (ENTER_TRY) içeren fonksiyonlara
        // eklenir — try'sız sıcak yollar (fibonacci vb.) sıfır ek yük taşır.
        // İz yığını ayrı bir bookkeeping'dir (GC shadow stack'ten bağımsız,
        // MIRPLAN §7 kararı); satır bilgisi THROW/error sitesinde doldurulur.
        bool fnHasTry = false;
        for (const auto& ins : fn.instructions)
            if (ins.opcode == Opcode::ENTER_TRY) { fnHasTry = true; break; }
        if (fnHasTry) {
            const std::string& file =
                program.moduleRegistry.filePath(fn.moduleId);
            MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4,
                MIR_new_ref_op(ctx, traceEnterProto), MIR_new_ref_op(ctx, traceEnterImport),
                MIR_new_int_op(ctx, reinterpret_cast<int64_t>(fn.name.c_str())),
                MIR_new_int_op(ctx, reinterpret_cast<int64_t>(file.c_str()))));
        }

        auto R = [&](int slot) { return MIR_new_reg_op(ctx, regs[static_cast<size_t>(slot)]); };

        // #221: nullable slot'un isNull bayrağını ayarla. `v` sabiti 0 (değer
        // var) veya 1 (null). Slot nullable değilse hiçbir şey yapmaz.
        //
        // DİKKAT: değer üreten HER opcode bunu 0'a çekmek zorundadır, yoksa
        // önceki null'luk sızar: `int? a; a = 5;` sonrası `a == null` yanlışlıkla
        // true kalırdı. setNullFlag çağrısı unutulan bir opcode = sessiz yanlış
        // cevap, bu yüzden emitInstr sonunda TOPLU olarak uygulanır (aşağıya bak).
        auto setNullFlag = [&](int slot, int v) {
            if (!isNullableSlot(slot)) return;
            MIR_append_insn(ctx, func,
                MIR_new_insn(ctx, MIR_MOV,
                    MIR_new_reg_op(ctx, nullFlagRegs[static_cast<size_t>(slot)]),
                    MIR_new_int_op(ctx, v)));
        };
        // ADR-040: D (double) argüman bekleyen bir runtime call'a float32
        // kaynak slot geçilecekse önce F2D ile genişlet (MIR call'da MIR_T_F/
        // MIR_T_D operand tip uyuşmazlığına düşmemek için — rt_jit_print_float32
        // çağrı-sitesindeki desenle aynı). CAST_FLOAT_TO_INT_CHECKED /
        // CAST_FLOAT_TO_LONG_CHECKED her ikisi de kaynak float32 YA DA double
        // olabilir (srcIsFloat = srcIsFloat32||srcIsDouble, ir_generator.cpp).
        int f32ToDCounter = 0;
        auto asDoubleOperand = [&](int slot) {
            if (slotKindOf(fn, slot) == SlotType::Float32) {
                std::string tmpName = "f32tod" + std::to_string(f32ToDCounter++);
                MIR_reg_t tmp = MIR_new_func_reg(ctx, func->u.func, MIR_T_D, tmpName.c_str());
                MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_F2D, MIR_new_reg_op(ctx, tmp), R(slot)));
                return MIR_new_reg_op(ctx, tmp);
            }
            return R(slot);
        };
        // Bir karşılaştırma/aritmetik talimatının FLOAT operand mı aldığını
        // (varyant seçimi için) statik slot türünden anla.
        auto floatOperands = [&](const Instruction& in) {
            return slotKindOf(fn, in.left) == SlotType::Float ||
                   slotKindOf(fn, in.right) == SlotType::Float;
        };
        // ADR-040: float32 operand mı (MIR single karşılaştırma varyantı için).
        auto float32Operands = [&](const Instruction& in) {
            return slotKindOf(fn, in.left) == SlotType::Float32 ||
                   slotKindOf(fn, in.right) == SlotType::Float32;
        };
        // Karşılaştırma MIR op'unu operand türüne göre seç: int/longint (I),
        // double (D), float32 (F). i=int, d=double, f=single opu.
        auto cmpOp = [&](const Instruction& in, MIR_insn_code_t iOp,
                         MIR_insn_code_t dOp, MIR_insn_code_t fOp) {
            if (floatOperands(in))   return dOp;
            if (float32Operands(in)) return fOp;
            return iOp;
        };
        // Eşitlik karşılaştırması string operand mı alıyor (içerik karşılaştırması
        // → rt_jit_string_eq runtime call, ADR-023). Tip denetleyici iki operandın
        // da string olmasını garanti eder (karışık yasak).
        auto stringOperands = [&](const Instruction& in) {
            return slotKindOf(fn, in.left) == SlotType::Str ||
                   slotKindOf(fn, in.right) == SlotType::Str;
        };
        // Decimal operandlı karşılaştırma: kutulu pointer olduğu için native
        // MIR_EQ/MIR_LT ADRES karşılaştırır — DEĞER için trampolin şart.
        auto decimalOperands = [&](const Instruction& in) {
            return slotKindOf(fn, in.left) == SlotType::Decimal ||
                   slotKindOf(fn, in.right) == SlotType::Decimal;
        };
        // dest = rt_jit_decimal_cmp(left, right) <op> 0
        // cmp -1/0/1 döndürür; istenen ilişki sonucun 0 ile karşılaştırılmasıdır.
        int decCmpCounter = 0;
        auto emitDecimalCompare = [&](const Instruction& in, MIR_insn_code_t rel) {
            std::string tmpName = "deccmp" + std::to_string(decCmpCounter++);
            MIR_reg_t tmp = MIR_new_func_reg(ctx, func->u.func, MIR_T_I64, tmpName.c_str());
            MIR_append_insn(ctx, func,
                MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, decCmpProto),
                    MIR_new_ref_op(ctx, decCmpImport),
                    MIR_new_reg_op(ctx, tmp), R(in.left), R(in.right)));
            MIR_append_insn(ctx, func,
                MIR_new_insn(ctx, rel, R(in.dest),
                    MIR_new_reg_op(ctx, tmp), MIR_new_int_op(ctx, 0)));
        };

        // #221: null-farkındalıklı eşitlik. VM semantiği (interpreter.cpp
        // EQUAL_EQUAL/NOT_EQUAL) NULL-ÖNCELİKLİDİR ve birebir eşlenir:
        //   ikisi de null      → eşit
        //   yalnız biri null   → eşit DEĞİL
        //   hiçbiri null değil → normal değer karşılaştırması
        //
        // Üretilen kod (eq için; ne'de sonuç XOR 1 ile terslenir):
        //     bothNull = lnull & rnull
        //     anyNull  = lnull | rnull
        //     valEq    = <normal karşılaştırma>
        //     dest     = bothNull | (valEq & ~anyNull)
        //
        // Dallanmasız — JIT sıcak yolunda tahmin edilemeyen branch üretmez.
        // Operandlardan hiçbiri nullable değilse çağıran bu yolu hiç kullanmaz.
        int nullTmpCounter = 0;
        auto newTmp = [&](const char* prefix) {
            std::string n = std::string(prefix) + std::to_string(nullTmpCounter++);
            return MIR_new_func_reg(ctx, func->u.func, MIR_T_I64, n.c_str());
        };
        // Bir operandın "null mu?" bitini veren operand. Slot nullable değilse
        // sabit 0 — MIR sabit katlaması bunu bedavaya indirir.
        auto nullBitOf = [&](int slot) -> MIR_op_t {
            if (!isNullableSlot(slot)) return MIR_new_int_op(ctx, 0);
            return MIR_new_reg_op(ctx, nullFlagRegs[static_cast<size_t>(slot)]);
        };
        auto eitherNullable = [&](const Instruction& in) {
            return isNullableSlot(in.left) || isNullableSlot(in.right);
        };
        // valEqReg: normal (null'suz) karşılaştırmanın sonucunu tutan register.
        // Bu fonksiyon onu null kurallarıyla düzeltip dest'e yazar.
        auto applyNullEquality = [&](const Instruction& in, MIR_reg_t valEqReg, bool negate) {
            MIR_reg_t bothNull = newTmp("bothnull");
            MIR_reg_t anyNull  = newTmp("anynull");
            MIR_reg_t masked   = newTmp("nullmask");
            MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_AND,
                MIR_new_reg_op(ctx, bothNull), nullBitOf(in.left), nullBitOf(in.right)));
            MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_OR,
                MIR_new_reg_op(ctx, anyNull), nullBitOf(in.left), nullBitOf(in.right)));
            // masked = anyNull ^ 1   (yani "hiçbiri null değil")
            MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_XOR,
                MIR_new_reg_op(ctx, masked), MIR_new_reg_op(ctx, anyNull),
                MIR_new_int_op(ctx, 1)));
            // masked = valEq & masked
            MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_AND,
                MIR_new_reg_op(ctx, masked), MIR_new_reg_op(ctx, valEqReg),
                MIR_new_reg_op(ctx, masked)));
            // dest = bothNull | masked   → EQUAL_EQUAL sonucu
            MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_OR,
                R(in.dest), MIR_new_reg_op(ctx, bothNull), MIR_new_reg_op(ctx, masked)));
            if (negate)  // NOT_EQUAL: eşitliğin tersi
                MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_XOR,
                    R(in.dest), R(in.dest), MIR_new_int_op(ctx, 1)));
        };

        // #228: bir slot'taki referansı GC'ye görünür kıl. İndeks TABAN+slot
        // olarak yazılır — düz slot indeksi recursive çağrıda çerçeveler
        // arası çakışır, üst çerçevenin kökleri ezilir (use-after-free).
        // nextIp: sıradaki talimatın indeksi (i+1). Slot orada artık canlı
        // değilse kök yazımı atlanır (liveness daraltması) — değeri bir daha
        // okunmayacak nesnenin köklenmesi yalnızca gereksiz canlılık üretir.
        // Eski kök girdisi yerinde kalırsa sonuç muhafazakârdır (eski nesne
        // fazla yaşamak dışında zarar vermez).
        auto emitShadowSet = [&](int slot, int nextIp) {
            if (slot < 0 || !needsShadowFrame) return;
            if (nextIp >= 0 && slotLive.exact &&
                !slotLive.isLiveBefore(nextIp, slot))
                return;
            MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 5,
                MIR_new_ref_op(ctx, ssProto), MIR_new_ref_op(ctx, ssImport),
                MIR_new_reg_op(ctx, ssBaseReg), MIR_new_int_op(ctx, (int64_t)slot), R(slot)));
        };
        auto emitCastBegin = [&](const Instruction& in) {
            MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 3,
                MIR_new_ref_op(ctx, castBeginProto), MIR_new_ref_op(ctx, castBeginImport),
                MIR_new_int_op(ctx, in.left == 1 ? 1 : 0)));
        };
        auto emitCastNull = [&](const Instruction& in) {
            if (!isNullableSlot(in.dest)) return;
            MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 3,
                MIR_new_ref_op(ctx, castNullProto), MIR_new_ref_op(ctx, castNullImport),
                MIR_new_reg_op(ctx, nullFlagRegs[static_cast<size_t>(in.dest)])));
        };
        auto isFallibleCast = [](Opcode op) {
            switch (op) {
                case Opcode::CAST_STR_TO_INT:
                case Opcode::CAST_STR_TO_FLOAT:
                case Opcode::CAST_FLOAT_TO_INT_CHECKED:
                case Opcode::CAST_INT_TO_BYTE_CHECKED:
                case Opcode::CAST_STR_TO_LONG:
                case Opcode::CAST_STR_TO_FLOAT32:
                case Opcode::CAST_FLOAT_TO_LONG_CHECKED:
                case Opcode::LONG_TO_INT_CHECKED:
                case Opcode::CAST_DECIMAL_TO_INT:
                case Opcode::CAST_STR_TO_DECIMAL:
                    return true;
                default:
                    return false;
            }
        };

        for (int pi = 0; pi < fn.paramCount; ++pi) {
            if (!isNullableSlot(pi)) continue;
            MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4,
                MIR_new_ref_op(ctx, callArgNullGetProto),
                MIR_new_ref_op(ctx, callArgNullGetImport),
                MIR_new_reg_op(ctx, nullFlagRegs[static_cast<size_t>(pi)]),
                MIR_new_int_op(ctx, pi)));
        }

        for (size_t i = 0; i < instrN; i++) {
            MIR_append_insn(ctx, func, labelAt[i]);
            const Instruction& instr = fn.instructions[i];

            // Bölme opcode'larının hata dalı için: hatayı bayrağa yazdıktan
            // sonra genel yayılım kontrolünü BEKLEMEDEN doğrudan hedefe atla.
            //
            // Neden: bölme kodgen'i sıfırı zaten inline kontrol ediyor ve o
            // dalda hatanın oluştuğunu KESİN biliyor. Genel yayılım bloğunun
            // yaptığı iş (rt_jit_error_pending çağırıp bayrağı sorgulamak)
            // burada gereksizdir — cevabı önceden biliyoruz. Ölçülen fark:
            // bölme yollarında 11x, aynı döngü bölmesiz 53x.
            auto emitJumpToErrorTarget = [&]() {
                if (handlerTarget[i] >= 0) {
                    int errorSlot = handlerErrorSlot[i];
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 3,
                        MIR_new_ref_op(ctx, errTakeProto),
                        MIR_new_ref_op(ctx, errTakeImport), R(errorSlot)));
                    emitShadowSet(errorSlot, (int)i + 1);
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_JMP,
                        MIR_new_label_op(ctx, labelAt[static_cast<size_t>(handlerTarget[i])])));
                } else {
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_JMP,
                        MIR_new_label_op(ctx, propagateLabel)));
                }
            };

            // Hata konumu: yakalanabilir hata üretebilen opcode'dan ÖNCE
            // rt().errorLine/Col'u doldur — jitSetError defaults olarak bu
            // değerleri kullanır (VM'in instr.sourceLine/sourceCol kullanımıyla
            // birebir, ADR-025). Bir önceki instruction'ın konumu sızmasın.
            switch (instr.opcode) {
                // NOT: int/long/float sıfıra bölme (DIV/MOD/FDIV/LDIV/LMOD/
                // F32DIV) bu listede DEĞİLDİR. Onların kodgen'i sıfır
                // kontrolünü inline yapar ve konumu hata DALINDA, trampoline
                // parametre olarak geçirir. Buraya koymak, hata olmasa bile
                // tur başına bir rt_jit_error_location çağrısı demekti —
                // sıcak bölme döngülerinde ölçülebilir maliyet.
                //
                // DDIV/DMOD (decimal) listede KALIR: onların hata yolu
                // trampolinin içindedir (rt_jit_decimal_div), konumu dışarıdan
                // geçirilemez.
                case Opcode::DDIV:
                case Opcode::DMOD:
                case Opcode::ARRAY_GET:
                case Opcode::ARRAY_SET:
                case Opcode::CALLHOST:
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4,
                        MIR_new_ref_op(ctx, errorLocationProto),
                        MIR_new_ref_op(ctx, errorLocationImport),
                        MIR_new_int_op(ctx, instr.sourceLine),
                        MIR_new_int_op(ctx, instr.sourceCol)));
                    break;
                default:
                    if (isFallibleCast(instr.opcode)) {
                        MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4,
                            MIR_new_ref_op(ctx, errorLocationProto),
                            MIR_new_ref_op(ctx, errorLocationImport),
                            MIR_new_int_op(ctx, instr.sourceLine),
                            MIR_new_int_op(ctx, instr.sourceCol)));
                    }
                    break;
            }

            switch (instr.opcode) {
                case Opcode::LOAD_CONST:
                    MIR_append_insn(ctx, func,
                        MIR_new_insn(ctx, MIR_MOV, R(instr.dest), MIR_new_int_op(ctx, instr.intValue)));
                    break;
                case Opcode::LOAD_FLOAT:
                    MIR_append_insn(ctx, func,
                        MIR_new_insn(ctx, MIR_DMOV, R(instr.dest), MIR_new_double_op(ctx, instr.floatValue)));
                    break;
                case Opcode::LOAD_STRING: {
                    // Sabit string'i derleme zamanı kutula, pointer'ını int
                    // sabiti olarak register'a taşı (ADR-037: Str = I64 pointer).
                    StringObject* obj = internString(instr.stringValue);
                    MIR_append_insn(ctx, func,
                        MIR_new_insn(ctx, MIR_MOV, R(instr.dest),
                            MIR_new_int_op(ctx, reinterpret_cast<int64_t>(obj))));
                    break;
                }
                case Opcode::STRING_CONCAT:
                    // dest = rt_jit_string_concat(left, right) — yeni string kutusu.
                    MIR_append_insn(ctx, func,
                        MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, concatProto),
                            MIR_new_ref_op(ctx, concatImport),
                            R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                // ── Cast (Dilim 3) — hepsi dest = rt_jit_<cast>(src) runtime call ──
                case Opcode::CAST_INT_TO_STR:
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, castI2SProto), MIR_new_ref_op(ctx, castI2SImport), R(instr.dest), R(instr.src)));
                    break;
                case Opcode::CAST_FLOAT_TO_STR:
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, castF2SProto), MIR_new_ref_op(ctx, castF2SImport), R(instr.dest), R(instr.src)));
                    break;
                case Opcode::CAST_BOOL_TO_STR:
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, castB2SProto), MIR_new_ref_op(ctx, castB2SImport), R(instr.dest), R(instr.src)));
                    break;
                case Opcode::CAST_LONG_TO_STR:
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, castL2SProto), MIR_new_ref_op(ctx, castL2SImport), R(instr.dest), R(instr.src)));
                    break;
                case Opcode::CAST_FLOAT32_TO_STR:
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, castF322SProto), MIR_new_ref_op(ctx, castF322SImport), R(instr.dest), R(instr.src)));
                    break;
                case Opcode::CAST_STR_TO_INT:
                    emitCastBegin(instr);
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, castS2IProto), MIR_new_ref_op(ctx, castS2IImport), R(instr.dest), R(instr.src)));
                    emitCastNull(instr);
                    break;
                case Opcode::CAST_STR_TO_FLOAT:
                    emitCastBegin(instr);
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, castS2FProto), MIR_new_ref_op(ctx, castS2FImport), R(instr.dest), R(instr.src)));
                    emitCastNull(instr);
                    break;
                case Opcode::CAST_FLOAT_TO_INT_CHECKED:
                    emitCastBegin(instr);
                    // srcType float32 OLABİLİR (ir_generator.cpp: srcIsFloat =
                    // srcIsFloat32||srcIsDouble) — call D bekliyor, F2D şart.
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, castF2IProto), MIR_new_ref_op(ctx, castF2IImport), R(instr.dest), asDoubleOperand(instr.src)));
                    emitCastNull(instr);
                    break;
                case Opcode::CAST_INT_TO_BYTE_CHECKED: {
                    // INLINE aralık kontrolü — başarı yolunda çağrı YOK.
                    //
                    // Kontrolün kendisi iki karşılaştırmadır; onu bir native
                    // çağrının arkasına koymak (eski hal: cast_begin + cast +
                    // cast_ret_is_null) ölçülebilir maliyet üretiyordu:
                    // 5M turluk byte döngüsünde JIT 188ms → 254ms (%35).
                    // Fazladan opcode'un kendisi bedavaydı (188→189ms), yani
                    // maliyetin tamamı çağrı köprüsündendi.
                    //
                    // Şema: aralık içindeyse doğrudan kopyala; dışındaysa
                    // yalnız O DALDA trampoline sap (hata mesajı string kurar,
                    // nullable modunda null bayrağı yazar — ikisi de sıcak
                    // yolda değil).
                    MIR_label_t failLabel = MIR_new_label(ctx);
                    MIR_label_t doneLabel = MIR_new_label(ctx);
                    MIR_reg_t   src       = regs[static_cast<size_t>(instr.src)];

                    // src < 0 || src > 255 → fail
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_BLT,
                        MIR_new_label_op(ctx, failLabel), MIR_new_reg_op(ctx, src),
                        MIR_new_int_op(ctx, 0)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_BGT,
                        MIR_new_label_op(ctx, failLabel), MIR_new_reg_op(ctx, src),
                        MIR_new_int_op(ctx, 255)));
                    // Başarı: değeri taşı, null bayrağını temizle.
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MOV,
                        R(instr.dest), MIR_new_reg_op(ctx, src)));
                    if (isNullableSlot(instr.dest)) setNullFlag(instr.dest, 0);
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_JMP,
                        MIR_new_label_op(ctx, doneLabel)));
                    // Hata dalı: VM ile aynı mesaj/nullable davranışı için
                    // mevcut trampolin kullanılır (cast_begin nullable modunu
                    // kurar, cast_error ya hata yayar ya null bayrağı yazar).
                    MIR_append_insn(ctx, func, failLabel);
                    emitCastBegin(instr);
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4,
                        MIR_new_ref_op(ctx, castI2BProto), MIR_new_ref_op(ctx, castI2BImport),
                        R(instr.dest), MIR_new_reg_op(ctx, src)));
                    emitCastNull(instr);
                    MIR_append_insn(ctx, func, doneLabel);
                    break;
                }
                case Opcode::CAST_STR_TO_LONG:
                    emitCastBegin(instr);
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, castS2LProto), MIR_new_ref_op(ctx, castS2LImport), R(instr.dest), R(instr.src)));
                    emitCastNull(instr);
                    break;
                case Opcode::CAST_STR_TO_FLOAT32:
                    emitCastBegin(instr);
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, castS2F32Proto), MIR_new_ref_op(ctx, castS2F32Import), R(instr.dest), R(instr.src)));
                    emitCastNull(instr);
                    break;
                case Opcode::LONG_TO_INT_CHECKED: {
                    // INLINE aralık kontrolü — gerekçe CAST_INT_TO_BYTE_CHECKED'te.
                    MIR_label_t failLabel = MIR_new_label(ctx);
                    MIR_label_t doneLabel = MIR_new_label(ctx);
                    MIR_reg_t   src       = regs[static_cast<size_t>(instr.src)];

                    // src < INT_MIN || src > INT_MAX → fail
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_BLT,
                        MIR_new_label_op(ctx, failLabel), MIR_new_reg_op(ctx, src),
                        MIR_new_int_op(ctx, INT_MIN)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_BGT,
                        MIR_new_label_op(ctx, failLabel), MIR_new_reg_op(ctx, src),
                        MIR_new_int_op(ctx, INT_MAX)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MOV,
                        R(instr.dest), MIR_new_reg_op(ctx, src)));
                    if (isNullableSlot(instr.dest)) setNullFlag(instr.dest, 0);
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_JMP,
                        MIR_new_label_op(ctx, doneLabel)));
                    MIR_append_insn(ctx, func, failLabel);
                    emitCastBegin(instr);
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4,
                        MIR_new_ref_op(ctx, castL2IProto), MIR_new_ref_op(ctx, castL2IImport),
                        R(instr.dest), MIR_new_reg_op(ctx, src)));
                    emitCastNull(instr);
                    MIR_append_insn(ctx, func, doneLabel);
                    break;
                }
                case Opcode::CAST_FLOAT_TO_LONG_CHECKED:
                    emitCastBegin(instr);
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, castF2LProto), MIR_new_ref_op(ctx, castF2LImport), R(instr.dest), asDoubleOperand(instr.src)));
                    emitCastNull(instr);
                    break;
                // ── Decimal (Dilim 3) — kutulu; sabit derleme zamanı, aritmetik call ──
                case Opcode::LOAD_DECIMAL: {
                    DecimalObject* obj = jitBoxDecimal(instr.decimalValue);
                    MIR_append_insn(ctx, func,
                        MIR_new_insn(ctx, MIR_MOV, R(instr.dest),
                            MIR_new_int_op(ctx, reinterpret_cast<int64_t>(obj))));
                    break;
                }
                case Opcode::DADD:
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, decBinProto), MIR_new_ref_op(ctx, decAddImport), R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::DSUB:
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, decBinProto), MIR_new_ref_op(ctx, decSubImport), R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::DMUL:
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, decBinProto), MIR_new_ref_op(ctx, decMulImport), R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::DDIV:
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, decBinProto), MIR_new_ref_op(ctx, decDivImport), R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::DMOD:
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, decBinProto), MIR_new_ref_op(ctx, decModImport), R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::DNEG:
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, decUnIProto), MIR_new_ref_op(ctx, decNegImport), R(instr.dest), R(instr.src)));
                    break;
                case Opcode::INT_TO_DECIMAL:
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, decUnIProto), MIR_new_ref_op(ctx, decI2DImport), R(instr.dest), R(instr.src)));
                    break;
                case Opcode::FLOAT_TO_DECIMAL:
                    // decFromFProto parametreyi MIR_T_D alır; kaynak slot
                    // Float32 (MIR_T_F) olabilir — asDoubleOperand F2D ile
                    // genişletir. Genişletmeden geçilirse MIR call'ı
                    // "unexpected operand mode ... Got 'float', expected
                    // 'double'" ile reddeder ve program VM'e düşer.
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, decFromFProto), MIR_new_ref_op(ctx, decF2DImport), R(instr.dest), asDoubleOperand(instr.src)));
                    break;
                case Opcode::CAST_DECIMAL_TO_STR:
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, decUnIProto), MIR_new_ref_op(ctx, decToStrImport), R(instr.dest), R(instr.src)));
                    break;
                case Opcode::CAST_DECIMAL_TO_INT:
                    emitCastBegin(instr);
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, decUnIProto), MIR_new_ref_op(ctx, decToIntImport), R(instr.dest), R(instr.src)));
                    emitCastNull(instr);
                    break;
                case Opcode::CAST_DECIMAL_TO_FLOAT:
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, decToFProto), MIR_new_ref_op(ctx, decToFImport), R(instr.dest), R(instr.src)));
                    break;
                case Opcode::CAST_STR_TO_DECIMAL:
                    emitCastBegin(instr);
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, decUnIProto), MIR_new_ref_op(ctx, decS2DImport), R(instr.dest), R(instr.src)));
                    emitCastNull(instr);
                    break;
                case Opcode::LOAD_SLOT: {
                    // Float→DMOV, Float32→FMOV, diğerleri MOV (pointer/int/longint I64).
                    SlotType dk = slotKindOf(fn, instr.dest);
                    MIR_insn_code_t mv = dk == SlotType::Float   ? MIR_DMOV
                                       : dk == SlotType::Float32 ? MIR_FMOV
                                       : MIR_MOV;
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, mv, R(instr.dest), R(instr.src)));
                    break;
                }
                // ── int32 aritmetiği (#113/ADR-040) ──────────────────────
                // saQut `int` 32-bit; MIR "S"-op'ları alt 32-bit'te çalışır ama
                // sonucun üst yarısı TANIMSIZ (MIR.md §insns) → her sonucu EXT32
                // ile sign-extend edip register'ı normalize tutuyoruz. Böylece
                // taşma VM'in int32 wrap'iyle birebir eşleşir (ADR-032/038).
                case Opcode::ADD:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_ADDS, R(instr.dest), R(instr.left), R(instr.right)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_EXT32, R(instr.dest), R(instr.dest)));
                    break;
                case Opcode::SUB:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_SUBS, R(instr.dest), R(instr.left), R(instr.right)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_EXT32, R(instr.dest), R(instr.dest)));
                    break;
                case Opcode::MUL:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MULS, R(instr.dest), R(instr.left), R(instr.right)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_EXT32, R(instr.dest), R(instr.dest)));
                    break;
                case Opcode::DIV: {
                    MIR_label_t okLabel   = MIR_new_label(ctx);
                    MIR_label_t doDiv     = MIR_new_label(ctx);
                    MIR_label_t doneLabel = MIR_new_label(ctx);
                    // /0 → yakalanabilir hata (VM ile aynı)
                    MIR_append_insn(ctx, func,
                        MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, okLabel), R(instr.right), MIR_new_int_op(ctx, 0)));
                    MIR_append_insn(ctx, func,
                        MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, divZeroProto), MIR_new_ref_op(ctx, divZeroImport), MIR_new_int_op(ctx, instr.sourceLine), MIR_new_int_op(ctx, instr.sourceCol)));
                    emitJumpToErrorTarget();
                    MIR_append_insn(ctx, func, okLabel);
                    // INT_MIN / -1 donanımda tuzak (#DE) → 2's-complement sonucu INT_MIN
                    MIR_append_insn(ctx, func,
                        MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, doDiv), R(instr.right), MIR_new_int_op(ctx, -1)));
                    MIR_append_insn(ctx, func,
                        MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, doDiv), R(instr.left), MIR_new_int_op(ctx, INT_MIN)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MOV, R(instr.dest), MIR_new_int_op(ctx, INT_MIN)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, doneLabel)));
                    MIR_append_insn(ctx, func, doDiv);
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_DIVS, R(instr.dest), R(instr.left), R(instr.right)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_EXT32, R(instr.dest), R(instr.dest)));
                    MIR_append_insn(ctx, func, doneLabel);
                    break;
                }
                case Opcode::MOD: {
                    MIR_label_t okLabel   = MIR_new_label(ctx);
                    MIR_label_t doMod     = MIR_new_label(ctx);
                    MIR_label_t doneLabel = MIR_new_label(ctx);
                    MIR_append_insn(ctx, func,
                        MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, okLabel), R(instr.right), MIR_new_int_op(ctx, 0)));
                    MIR_append_insn(ctx, func,
                        MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, modZeroProto), MIR_new_ref_op(ctx, modZeroImport), MIR_new_int_op(ctx, instr.sourceLine), MIR_new_int_op(ctx, instr.sourceCol)));
                    emitJumpToErrorTarget();
                    MIR_append_insn(ctx, func, okLabel);
                    // INT_MIN % -1 → tuzak → 2's-complement sonucu 0
                    MIR_append_insn(ctx, func,
                        MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, doMod), R(instr.right), MIR_new_int_op(ctx, -1)));
                    MIR_append_insn(ctx, func,
                        MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, doMod), R(instr.left), MIR_new_int_op(ctx, INT_MIN)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MOV, R(instr.dest), MIR_new_int_op(ctx, 0)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, doneLabel)));
                    MIR_append_insn(ctx, func, doMod);
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MODS, R(instr.dest), R(instr.left), R(instr.right)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_EXT32, R(instr.dest), R(instr.dest)));
                    MIR_append_insn(ctx, func, doneLabel);
                    break;
                }
                // ── Float aritmetiği (Dilim 1.5) ────────────────────────
                case Opcode::FADD:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_DADD, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::FSUB:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_DSUB, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::FMUL:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_DMUL, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::FDIV: {
                    // VM float /0 → yakalanabilir hata; try/catch bu dilimde
                    // reddedildiğinden uncaught = fatal (VM'de de aynı).
                    MIR_label_t okLabel = MIR_new_label(ctx);
                    MIR_label_t doneLabel = MIR_new_label(ctx);
                    MIR_append_insn(ctx, func,
                        MIR_new_insn(ctx, MIR_DBNE, MIR_new_label_op(ctx, okLabel), R(instr.right), MIR_new_double_op(ctx, 0.0)));
                    MIR_append_insn(ctx, func,
                        MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, fdivZeroProto), MIR_new_ref_op(ctx, fdivZeroImport), MIR_new_int_op(ctx, instr.sourceLine), MIR_new_int_op(ctx, instr.sourceCol)));
                    emitJumpToErrorTarget();
                    MIR_append_insn(ctx, func, okLabel);
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_DDIV, R(instr.dest), R(instr.left), R(instr.right)));
                    MIR_append_insn(ctx, func, doneLabel);
                    break;
                }
                case Opcode::FNEG:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_DNEG, R(instr.dest), R(instr.src)));
                    break;
                case Opcode::INT_TO_FLOAT:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_I2D, R(instr.dest), R(instr.src)));
                    break;
                case Opcode::FLOAT_TO_INT:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_D2I, R(instr.dest), R(instr.src)));
                    break;
                // ── LongInt aritmetiği (ADR-040) — native 64-bit, EXT32 YOK ──
                case Opcode::LOAD_LONG:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MOV, R(instr.dest), MIR_new_int_op(ctx, instr.int64Value)));
                    break;
                case Opcode::LADD:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_ADD, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::LSUB:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_SUB, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::LMUL:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MUL, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::LDIV: {
                    MIR_label_t okLabel = MIR_new_label(ctx);
                    MIR_label_t doDiv = MIR_new_label(ctx);
                    MIR_label_t doneLabel = MIR_new_label(ctx);
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, okLabel), R(instr.right), MIR_new_int_op(ctx, 0)));
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, divZeroProto), MIR_new_ref_op(ctx, divZeroImport), MIR_new_int_op(ctx, instr.sourceLine), MIR_new_int_op(ctx, instr.sourceCol)));
                    emitJumpToErrorTarget();
                    MIR_append_insn(ctx, func, okLabel);
                    // INT64_MIN / -1 → tuzak → 2's-complement sonucu INT64_MIN
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, doDiv), R(instr.right), MIR_new_int_op(ctx, -1)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, doDiv), R(instr.left), MIR_new_int_op(ctx, INT64_MIN)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MOV, R(instr.dest), MIR_new_int_op(ctx, INT64_MIN)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, doneLabel)));
                    MIR_append_insn(ctx, func, doDiv);
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_DIV, R(instr.dest), R(instr.left), R(instr.right)));
                    MIR_append_insn(ctx, func, doneLabel);
                    break;
                }
                case Opcode::LMOD: {
                    MIR_label_t okLabel = MIR_new_label(ctx);
                    MIR_label_t doMod = MIR_new_label(ctx);
                    MIR_label_t doneLabel = MIR_new_label(ctx);
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, okLabel), R(instr.right), MIR_new_int_op(ctx, 0)));
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, modZeroProto), MIR_new_ref_op(ctx, modZeroImport), MIR_new_int_op(ctx, instr.sourceLine), MIR_new_int_op(ctx, instr.sourceCol)));
                    emitJumpToErrorTarget();
                    MIR_append_insn(ctx, func, okLabel);
                    // INT64_MIN % -1 → tuzak → 0
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, doMod), R(instr.right), MIR_new_int_op(ctx, -1)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, doMod), R(instr.left), MIR_new_int_op(ctx, INT64_MIN)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MOV, R(instr.dest), MIR_new_int_op(ctx, 0)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, doneLabel)));
                    MIR_append_insn(ctx, func, doMod);
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MOD, R(instr.dest), R(instr.left), R(instr.right)));
                    MIR_append_insn(ctx, func, doneLabel);
                    break;
                }
                case Opcode::LNEG:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_NEG, R(instr.dest), R(instr.src)));
                    break;
                case Opcode::LBAND:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_AND, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::LBOR:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_OR, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::LBXOR:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_XOR, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::LSHL:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_LSH, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::LSHR:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_RSH, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::LBNOT:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_XOR, R(instr.dest), R(instr.src), MIR_new_int_op(ctx, -1)));
                    break;
                case Opcode::INT_TO_LONG:
                    // int (I64 register, sign-extended) → longint: kimlik kopya.
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MOV, R(instr.dest), R(instr.src)));
                    break;
                // ── Float32 aritmetiği (ADR-040) — native single MIR_T_F op ──
                case Opcode::LOAD_FLOAT32:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_FMOV, R(instr.dest), MIR_new_float_op(ctx, (float)instr.floatValue)));
                    break;
                case Opcode::F32ADD:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_FADD, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::F32SUB:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_FSUB, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::F32MUL:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_FMUL, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::F32DIV: {
                    MIR_label_t okLabel = MIR_new_label(ctx);
                    MIR_label_t doneLabel = MIR_new_label(ctx);
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_FBNE, MIR_new_label_op(ctx, okLabel), R(instr.right), MIR_new_float_op(ctx, 0.0f)));
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, fdivZeroProto), MIR_new_ref_op(ctx, fdivZeroImport), MIR_new_int_op(ctx, instr.sourceLine), MIR_new_int_op(ctx, instr.sourceCol)));
                    emitJumpToErrorTarget();
                    MIR_append_insn(ctx, func, okLabel);
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_FDIV, R(instr.dest), R(instr.left), R(instr.right)));
                    MIR_append_insn(ctx, func, doneLabel);
                    break;
                }
                case Opcode::F32NEG:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_FNEG, R(instr.dest), R(instr.src)));
                    break;
                case Opcode::INT_TO_FLOAT32:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_I2F, R(instr.dest), R(instr.src)));
                    break;
                case Opcode::FLOAT32_TO_INT:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_F2I, R(instr.dest), R(instr.src)));
                    break;
                case Opcode::FLOAT_TO_FLOAT32:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_D2F, R(instr.dest), R(instr.src)));
                    break;
                case Opcode::FLOAT32_TO_FLOAT:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_F2D, R(instr.dest), R(instr.src)));
                    break;
                case Opcode::BAND:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_AND, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::BOR:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_OR, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::BXOR:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_XOR, R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::SHL:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_LSHS, R(instr.dest), R(instr.left), R(instr.right)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_EXT32, R(instr.dest), R(instr.dest)));
                    break;
                case Opcode::SHR:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_RSHS, R(instr.dest), R(instr.left), R(instr.right)));
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_EXT32, R(instr.dest), R(instr.dest)));
                    break;
                case Opcode::BNOT:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_XOR, R(instr.dest), R(instr.src), MIR_new_int_op(ctx, -1)));
                    break;
                // ── Karşılaştırmalar — float operand ise D-varyantı, decimal
                //    operand ise rt_jit_decimal_cmp trampolini ─────────────
                case Opcode::LESS:
                    if (decimalOperands(instr)) { emitDecimalCompare(instr, MIR_LT); break; }
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, cmpOp(instr, MIR_LT, MIR_DLT, MIR_FLT), R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::LESS_EQUAL:
                    if (decimalOperands(instr)) { emitDecimalCompare(instr, MIR_LE); break; }
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, cmpOp(instr, MIR_LE, MIR_DLE, MIR_FLE), R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::GREATER:
                    if (decimalOperands(instr)) { emitDecimalCompare(instr, MIR_GT); break; }
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, cmpOp(instr, MIR_GT, MIR_DGT, MIR_FGT), R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::GREATER_EQUAL:
                    if (decimalOperands(instr)) { emitDecimalCompare(instr, MIR_GE); break; }
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, cmpOp(instr, MIR_GE, MIR_DGE, MIR_FGE), R(instr.dest), R(instr.left), R(instr.right)));
                    break;
                case Opcode::EQUAL_EQUAL:
                    // #221: operandlardan biri nullable ise null-öncelikli yol.
                    // Değer karşılaştırması geçici bir register'a yapılır, sonra
                    // null kurallarıyla düzeltilir.
                    if (eitherNullable(instr)) {
                        MIR_reg_t valEq = newTmp("valeq");
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx,
                            cmpOp(instr, MIR_EQ, MIR_DEQ, MIR_FEQ),
                            MIR_new_reg_op(ctx, valEq), R(instr.left), R(instr.right)));
                        applyNullEquality(instr, valEq, /*negate=*/false);
                        break;
                    }
                    if (decimalOperands(instr)) { emitDecimalCompare(instr, MIR_EQ); break; }
                    if (stringOperands(instr)) {
                        // dest = rt_jit_string_eq(left, right)  (içerik, ADR-023)
                        MIR_append_insn(ctx, func,
                            MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, strEqProto),
                                MIR_new_ref_op(ctx, strEqImport),
                                R(instr.dest), R(instr.left), R(instr.right)));
                    } else {
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx, cmpOp(instr, MIR_EQ, MIR_DEQ, MIR_FEQ), R(instr.dest), R(instr.left), R(instr.right)));
                    }
                    break;
                case Opcode::NOT_EQUAL:
                    if (eitherNullable(instr)) {
                        MIR_reg_t valEq = newTmp("valeq");
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx,
                            cmpOp(instr, MIR_EQ, MIR_DEQ, MIR_FEQ),
                            MIR_new_reg_op(ctx, valEq), R(instr.left), R(instr.right)));
                        applyNullEquality(instr, valEq, /*negate=*/true);
                        break;
                    }
                    if (decimalOperands(instr)) { emitDecimalCompare(instr, MIR_NE); break; }
                    if (stringOperands(instr)) {
                        // dest = !rt_jit_string_eq(left, right) → eq sonra XOR 1
                        MIR_append_insn(ctx, func,
                            MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, strEqProto),
                                MIR_new_ref_op(ctx, strEqImport),
                                R(instr.dest), R(instr.left), R(instr.right)));
                        MIR_append_insn(ctx, func,
                            MIR_new_insn(ctx, MIR_XOR, R(instr.dest), R(instr.dest), MIR_new_int_op(ctx, 1)));
                    } else {
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx, cmpOp(instr, MIR_NE, MIR_DNE, MIR_FNE), R(instr.dest), R(instr.left), R(instr.right)));
                    }
                    break;
                case Opcode::JMP:
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, labelAt[static_cast<size_t>(instr.jumpTarget)])));
                    break;
                case Opcode::JIF_FALSE:
                    MIR_append_insn(ctx, func,
                        MIR_new_insn(ctx, MIR_BF, MIR_new_label_op(ctx, labelAt[static_cast<size_t>(instr.jumpTarget)]), R(instr.cond)));
                    break;
                case Opcode::JIF_TRUE:
                    MIR_append_insn(ctx, func,
                        MIR_new_insn(ctx, MIR_BT, MIR_new_label_op(ctx, labelAt[static_cast<size_t>(instr.jumpTarget)]), R(instr.cond)));
                    break;
                case Opcode::CALL: {
                    const FuncEntry& callee = funcMap.at(instr.functionName);
                    for (size_t ai = 0; ai < instr.argSlots.size(); ++ai) {
                        int as = instr.argSlots[ai];
                        MIR_op_t nullOp = isNullableSlot(as)
                            ? MIR_new_reg_op(ctx, nullFlagRegs[static_cast<size_t>(as)])
                            : MIR_new_int_op(ctx, 0);
                        MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4,
                            MIR_new_ref_op(ctx, callArgNullSetProto),
                            MIR_new_ref_op(ctx, callArgNullSetImport),
                            MIR_new_int_op(ctx, (int64_t)ai), nullOp));
                    }
                    std::vector<MIR_op_t> ops;
                    ops.push_back(MIR_new_ref_op(ctx, callee.protoItem));
                    ops.push_back(MIR_new_ref_op(ctx, callee.callRef));
                    ops.push_back(R(instr.dest));
                    for (int argSlot : instr.argSlots) ops.push_back(R(argSlot));
                    MIR_append_insn(ctx, func,
                        MIR_new_insn_arr(ctx, MIR_CALL, ops.size(), ops.data()));
                    if (isNullableSlot(instr.dest))
                        MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 3,
                            MIR_new_ref_op(ctx, callRetNullGetProto),
                            MIR_new_ref_op(ctx, callRetNullGetImport),
                            MIR_new_reg_op(ctx, nullFlagRegs[static_cast<size_t>(instr.dest)])));
                    SlotType rt = slotKindOf(fn, instr.dest);
                    if (rt == SlotType::Str || rt == SlotType::Decimal || rt == SlotType::Ref)
                        emitShadowSet(instr.dest, (int)i + 1);
                    break;
                }
                case Opcode::ARRAY_NEW: {
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 5,
                        MIR_new_ref_op(ctx, anewProto), MIR_new_ref_op(ctx, anewImport),
                        R(instr.dest),
                        MIR_new_int_op(ctx, instr.intValue),
                        MIR_new_int_op(ctx, (int64_t)instr.arrayElemKind)));
                    emitShadowSet(instr.dest, (int)i + 1);
                    break;
                }
                case Opcode::ARRAY_LEN:
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4,
                        MIR_new_ref_op(ctx, alenProto), MIR_new_ref_op(ctx, alenImport),
                        R(instr.dest), R(instr.src)));
                    break;
                case Opcode::LOAD_GLOBAL: {
                    SlotType gt = instr.valueType != SlotType::Unknown
                                      ? instr.valueType : slotKindOf(fn, instr.dest);
                    bool isD = gt == SlotType::Float || gt == SlotType::Float32;
                    bool isP = gt == SlotType::Str || gt == SlotType::Decimal || gt == SlotType::Ref;
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4,
                        MIR_new_ref_op(ctx, isD ? globalLoadDProto : (isP ? globalLoadPProto : globalLoadIProto)),
                        MIR_new_ref_op(ctx, isD ? globalLoadDImport : (isP ? globalLoadPImport : globalLoadIImport)),
                        R(instr.dest), MIR_new_int_op(ctx, instr.intValue)));
                    if (isP) emitShadowSet(instr.dest, (int)i + 1);
                    break;
                }
                case Opcode::STORE_GLOBAL: {
                    SlotType gt = slotKindOf(fn, instr.src);
                    bool isD = gt == SlotType::Float || gt == SlotType::Float32;
                    bool isP = gt == SlotType::Str || gt == SlotType::Decimal || gt == SlotType::Ref;
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4,
                        MIR_new_ref_op(ctx, isD ? globalStoreDProto : (isP ? globalStorePProto : globalStoreIProto)),
                        MIR_new_ref_op(ctx, isD ? globalStoreDImport : (isP ? globalStorePImport : globalStoreIImport)),
                        MIR_new_int_op(ctx, instr.intValue),
                        isD ? asDoubleOperand(instr.src) : R(instr.src)));
                    break;
                }
                case Opcode::ARRAY_GET: {
                    SlotType vt = instr.valueType;
                    // SPIKE (Aşama 0): Int eleman → doğrudan MIR memory access.
                    // arr.ints, jitData/jitLength view'ından sabit offset ile okunur;
                    // trampoline çağrısı (rt_jit_array_get_i) hot path'te yok.
                    // valueType != Unknown zaten wholeProgramSupported'ta garanti.
                    const bool directInt = vt == SlotType::Int &&
                                           instr.arrayElemKind == ArrayElemKind::Int;
                    const bool directByte = instr.arrayElemKind == ArrayElemKind::Byte;
                    if (directInt || directByte) {
                        static int spikeRegCounter = 0;
                        std::string dName = "agdata" + std::to_string(spikeRegCounter++);
                        std::string lName = "aglen"  + std::to_string(spikeRegCounter++);
                        MIR_reg_t dataReg = MIR_new_func_reg(ctx, func->u.func, MIR_T_I64, dName.c_str());
                        MIR_reg_t lenReg  = MIR_new_func_reg(ctx, func->u.func, MIR_T_I64, lName.c_str());
                        MIR_label_t okL    = MIR_new_label(ctx);
                        MIR_label_t failL  = MIR_new_label(ctx);

                        // data = [arr + offsetof(ArrayObject, jitData)]
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MOV,
                            MIR_new_reg_op(ctx, dataReg),
                            MIR_new_mem_op(ctx, MIR_T_I64, (MIR_disp_t)offsetof(ArrayObject, jitData),
                                           R(instr.left).u.reg, 0, 0)));
                        // len = [arr + offsetof(ArrayObject, jitLength)]
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MOV,
                            MIR_new_reg_op(ctx, lenReg),
                            MIR_new_mem_op(ctx, MIR_T_I64, (MIR_disp_t)offsetof(ArrayObject, jitLength),
                                           R(instr.left).u.reg, 0, 0)));
                        // if idx >= len → fail
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_BGE,
                            MIR_new_label_op(ctx, failL), R(instr.right), MIR_new_reg_op(ctx, lenReg)));
                        // if idx < 0 → fail
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_BLT,
                            MIR_new_label_op(ctx, failL), R(instr.right), MIR_new_int_op(ctx, 0)));
                        // dest = [data + idx*4]  (int32_t eleman).
                        // Signed load is required: ARRAY_GET must preserve
                        // negative int values when the slot is i64-backed.
                        const MIR_type_t loadType = directByte ? MIR_T_U8 : MIR_T_I32;
                        const int elementSize = directByte ? 1 : 4;
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MOV,
                            R(instr.dest),
                            MIR_new_mem_op(ctx, loadType, 0, dataReg,
                                           R(instr.right).u.reg, elementSize)));
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, okL)));

                        // cold error block: bounds fail → pending error → uncaught exit
                        MIR_append_insn(ctx, func, failL);
                        MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4,
                            MIR_new_ref_op(ctx, errorLocationProto),
                            MIR_new_ref_op(ctx, errorLocationImport),
                            MIR_new_int_op(ctx, instr.sourceLine),
                            MIR_new_int_op(ctx, instr.sourceCol)));
                        MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4,
                            MIR_new_ref_op(ctx, abndProto),
                            MIR_new_ref_op(ctx, abndImport),
                            R(instr.right), MIR_new_reg_op(ctx, lenReg)));
                        // RET — JIT'te try/catch yok; uncaught exit runProgram'da
                        MIR_append_insn(ctx, func, MIR_new_ret_insn(ctx, 1, R(instr.dest)));
                        MIR_append_insn(ctx, func, okL);
                        break;
                    }
                    bool isD = (vt == SlotType::Float || vt == SlotType::Float32);
                    bool isP = (vt == SlotType::Ref || vt == SlotType::Str ||
                                vt == SlotType::Decimal);
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 5,
                        MIR_new_ref_op(ctx, isD ? agetDProto : (isP ? agetPProto : agetIProto)),
                        MIR_new_ref_op(ctx, isD ? agetDImport : (isP ? agetPImport : agetIImport)),
                        R(instr.dest), R(instr.left), R(instr.right)));
                    if (isP) emitShadowSet(instr.dest, (int)i + 1);
                    break;
                }
                case Opcode::ARRAY_SET: {
                    SlotType vt = slotKindOf(fn, instr.right);
                    const bool directInt = vt == SlotType::Int &&
                                           instr.arrayElemKind == ArrayElemKind::Int;
                    const bool directByte = instr.arrayElemKind == ArrayElemKind::Byte;
                    if (directInt || directByte) {
                        static int spikeSetRegCounter = 0;
                        std::string dName = "asdata" + std::to_string(spikeSetRegCounter++);
                        std::string lName = "aslen"  + std::to_string(spikeSetRegCounter++);
                        MIR_reg_t dataReg = MIR_new_func_reg(ctx, func->u.func, MIR_T_I64, dName.c_str());
                        MIR_reg_t lenReg  = MIR_new_func_reg(ctx, func->u.func, MIR_T_I64, lName.c_str());
                        MIR_label_t okL   = MIR_new_label(ctx);
                        MIR_label_t failL = MIR_new_label(ctx);

                        MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MOV,
                            MIR_new_reg_op(ctx, dataReg),
                            MIR_new_mem_op(ctx, MIR_T_I64, (MIR_disp_t)offsetof(ArrayObject, jitData),
                                           R(instr.dest).u.reg, 0, 0)));
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MOV,
                            MIR_new_reg_op(ctx, lenReg),
                            MIR_new_mem_op(ctx, MIR_T_I64, (MIR_disp_t)offsetof(ArrayObject, jitLength),
                                           R(instr.dest).u.reg, 0, 0)));
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_BGE,
                            MIR_new_label_op(ctx, failL), R(instr.left), MIR_new_reg_op(ctx, lenReg)));
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_BLT,
                            MIR_new_label_op(ctx, failL), R(instr.left), MIR_new_int_op(ctx, 0)));

                        // ARRAY_SET stores the VM payload width; the source slot
                        // is i64-backed and is truncated to the array element.
                        const MIR_type_t storeType = directByte ? MIR_T_U8 : MIR_T_I32;
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MOV,
                            MIR_new_mem_op(ctx, storeType, 0, dataReg,
                                           R(instr.left).u.reg, directByte ? 1 : 4),
                            R(instr.right)));
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_JMP,
                            MIR_new_label_op(ctx, okL)));

                        MIR_append_insn(ctx, func, failL);
                        MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4,
                            MIR_new_ref_op(ctx, errorLocationProto),
                            MIR_new_ref_op(ctx, errorLocationImport),
                            MIR_new_int_op(ctx, instr.sourceLine),
                            MIR_new_int_op(ctx, instr.sourceCol)));
                        MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 4,
                            MIR_new_ref_op(ctx, abndProto),
                            MIR_new_ref_op(ctx, abndImport),
                            R(instr.left), MIR_new_reg_op(ctx, lenReg)));
                        // The generated function has an i64 return ABI even for
                        // statement-like opcodes. Match ARRAY_GET's error exit
                        // shape instead of emitting a zero-operand RET.
                        MIR_append_insn(ctx, func, MIR_new_ret_insn(ctx, 1, R(instr.dest)));
                        MIR_append_insn(ctx, func, okL);
                        break;
                    }
                    bool isD = (vt == SlotType::Float || vt == SlotType::Float32);
                    bool isP = (vt == SlotType::Ref || vt == SlotType::Str ||
                                vt == SlotType::Decimal);
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 5,
                        MIR_new_ref_op(ctx, isD ? asetDProto : (isP ? asetPProto : asetIProto)),
                        MIR_new_ref_op(ctx, isD ? asetDImport : (isP ? asetPImport : asetIImport)),
                        R(instr.dest), R(instr.left),
                        isD ? asDoubleOperand(instr.right) : R(instr.right)));
                    break;
                }
                case Opcode::STRUCT_NEW: {
                    int64_t metaId = -1;
                    {
                        JitStructMeta m;
                        auto nameIt = fn.structFieldNames.find(instr.functionName);
                        if (nameIt != fn.structFieldNames.end())
                            m.names = std::make_shared<std::vector<std::string>>(nameIt->second);
                        auto nullIt = fn.structFieldNullable.find(instr.functionName);
                        if (nullIt != fn.structFieldNullable.end())
                            m.nullableMask = nullIt->second;
                        if (m.names || !m.nullableMask.empty()) {
                            metaId = (int64_t)rt().structMeta.size();
                            rt().structMeta.push_back(std::move(m));
                        }
                    }
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 5,
                        MIR_new_ref_op(ctx, snewProto), MIR_new_ref_op(ctx, snewImport),
                        R(instr.dest), MIR_new_int_op(ctx, instr.intValue),
                        MIR_new_int_op(ctx, metaId)));
                    emitShadowSet(instr.dest, (int)i + 1);
                    break;
                }
                case Opcode::FIELD_GET: {
                    SlotType vt = instr.valueType;
                    bool isD = (vt == SlotType::Float || vt == SlotType::Float32);
                    bool isP = (vt == SlotType::Ref || vt == SlotType::Str ||
                                vt == SlotType::Decimal);
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 5,
                        MIR_new_ref_op(ctx, isD ? fgetDProto : (isP ? fgetPProto : fgetIProto)),
                        MIR_new_ref_op(ctx, isD ? fgetDImport : (isP ? fgetPImport : fgetIImport)),
                        R(instr.dest), R(instr.src),
                        MIR_new_int_op(ctx, instr.intValue)));
                    if (isP) emitShadowSet(instr.dest, (int)i + 1);
                    if (isNullableSlot(instr.dest))
                        MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 5,
                            MIR_new_ref_op(ctx, fgetNullProto),
                            MIR_new_ref_op(ctx, fgetNullImport),
                            MIR_new_reg_op(ctx, nullFlagRegs[static_cast<size_t>(instr.dest)]),
                            R(instr.src), MIR_new_int_op(ctx, instr.intValue)));
                    break;
                }
                case Opcode::FIELD_SET: {
                    SlotType vt = slotKindOf(fn, instr.right);
                    bool isD = (vt == SlotType::Float || vt == SlotType::Float32);
                    bool isP = (vt == SlotType::Ref || vt == SlotType::Str ||
                                vt == SlotType::Decimal);
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 5,
                        MIR_new_ref_op(ctx, isD ? fsetDProto : (isP ? fsetPProto : fsetIProto)),
                        MIR_new_ref_op(ctx, isD ? fsetDImport : (isP ? fsetPImport : fsetIImport)),
                        R(instr.dest), MIR_new_int_op(ctx, instr.intValue),
                        isD ? asDoubleOperand(instr.right) : R(instr.right)));
                    break;
                }
                case Opcode::ENTER_TRY:
                case Opcode::LEAVE_TRY:
                    // #110: catch hedefi/slotu STATİK olarak handlerTarget[] ve
                    // handlerErrorSlot[] üzerinden bilinir (aşağıdaki
                    // per-instruction kontrol). Runtime'da çerçeve açma/kapama
                    // gerekmez — VM'de TryFrame yığını yalnızca unwind sınırını
                    // tutar; JIT'te unwind propagateLabel üzerinden doğal akışla
                    // gerçekleşir (her fonksiyon kendi shadow çerçevesini sarar).
                    break;
                case Opcode::THROW: {
                    SlotType st = slotKindOf(fn, instr.src);
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 6,
                        MIR_new_ref_op(ctx, throwPProto), MIR_new_ref_op(ctx, throwPImport),
                        R(instr.src), MIR_new_int_op(ctx, (int64_t)st),
                        MIR_new_int_op(ctx, instr.sourceLine),
                        MIR_new_int_op(ctx, instr.sourceCol)));
                    break;
                }
                case Opcode::CALLHOST: {
                    for (size_t ai = 0; ai < instr.argSlots.size(); ++ai) {
                        int      as  = instr.argSlots[ai];
                        SlotType ast = slotKindOf(fn, as);
                        HostKind hk  = HostKind::Int;
                        switch (ast) {
                            case SlotType::Int:     hk = HostKind::Int;     break;
                            case SlotType::LongInt: hk = HostKind::LongInt; break;
                            case SlotType::Float:   hk = HostKind::Float;   break;
                            case SlotType::Float32: hk = HostKind::Float32; break;
                            case SlotType::Str:     hk = HostKind::Str;     break;
                            case SlotType::Decimal: hk = HostKind::Decimal; break;
                            case SlotType::Date:    hk = HostKind::Date;    break;
                            case SlotType::Ref:     hk = HostKind::Ref;     break;
                            default:                hk = HostKind::Int;     break;
                        }
                        bool isD = (ast == SlotType::Float || ast == SlotType::Float32);
                        bool isN = isNullableSlot(as);
                        if (isN) {
                            MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 6,
                                MIR_new_ref_op(ctx, isD ? hostArgNDProto : hostArgNIProto),
                                MIR_new_ref_op(ctx, isD ? hostArgNDImport : hostArgNIImport),
                                MIR_new_int_op(ctx, (int64_t)ai),
                                MIR_new_int_op(ctx, (int64_t)hk),
                                isD ? asDoubleOperand(as) : R(as),
                                MIR_new_reg_op(ctx, nullFlagRegs[static_cast<size_t>(as)])));
                        } else {
                            MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 5,
                                MIR_new_ref_op(ctx, isD ? hostArgDProto : hostArgIProto),
                                MIR_new_ref_op(ctx, isD ? hostArgDImport : hostArgIImport),
                                MIR_new_int_op(ctx, (int64_t)ai),
                                MIR_new_int_op(ctx, (int64_t)hk),
                                isD ? asDoubleOperand(as) : R(as)));
                        }
                    }
                    const HostEntry* he = hostEntryAt(instr.intValue);
                    // valueType doluysa (built-in metodlar) o otoritedir:
                    // registry retKind'i eleman-tipli dönüşlerde statik kalır.
                    HostKind rk = he ? he->retKind : HostKind::Void;
                    if (instr.valueType != SlotType::Unknown) {
                        switch (instr.valueType) {
                            case SlotType::Float:   rk = HostKind::Float;   break;
                            case SlotType::Float32: rk = HostKind::Float32; break;
                            case SlotType::Str:     rk = HostKind::Str;     break;
                            case SlotType::Decimal: rk = HostKind::Decimal; break;
                            case SlotType::Date:    rk = HostKind::Date;    break;
                            case SlotType::Ref:     rk = HostKind::Ref;     break;
                            case SlotType::LongInt: rk = HostKind::LongInt; break;
                            default: break;
                        }
                    }
                    bool retIsD = (rk == HostKind::Float || rk == HostKind::Float32);
                    // Dönüş double proto'dan gelir; hedef slot Float32 ise
                    // register MIR_T_F'tir → araya geçici D register ve D2F.
                    bool needF = retIsD && instr.dest >= 0 &&
                                 slotKindOf(fn, instr.dest) == SlotType::Float32;
                    static int hostDTmp = 0;
                    MIR_reg_t dst = needF
                                  ? MIR_new_func_reg(ctx, func->u.func, MIR_T_D,
                                        ("hostd" + std::to_string(hostDTmp++)).c_str())
                                  : (instr.dest >= 0 ? regs[static_cast<size_t>(instr.dest)]
                                                     : newTmp("hostsink"));
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 5,
                        MIR_new_ref_op(ctx, retIsD ? hostCallDProto : hostCallProto),
                        MIR_new_ref_op(ctx, retIsD ? hostCallDImport : hostCallImport),
                        MIR_new_reg_op(ctx, dst),
                        MIR_new_int_op(ctx, instr.intValue),
                        MIR_new_int_op(ctx, (int64_t)instr.argSlots.size())));
                    if (needF)
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_D2F,
                            R(instr.dest), MIR_new_reg_op(ctx, dst)));
                    if (instr.dest >= 0 && isNullableSlot(instr.dest))
                        MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 3,
                            MIR_new_ref_op(ctx, hostRetNullProto),
                            MIR_new_ref_op(ctx, hostRetNullImport),
                            MIR_new_reg_op(ctx, nullFlagRegs[static_cast<size_t>(instr.dest)])));
                    // Pointer dönüşü GC'ye görünür olmalı: host thunk'ı heap'te
                    // nesne üretmiş olabilir (string metodları, split).
                    if (instr.dest >= 0 &&
                        (rk == HostKind::Str || rk == HostKind::Ref ||
                         rk == HostKind::Decimal))
                        emitShadowSet(instr.dest, (int)i + 1);
                    break;
                }
                case Opcode::LOAD_NULL:
                    // #221: değer register'ı 0'a çekilir (belirlenmiş durum —
                    // çöp okumayı önler), null'luk yandaş bayrakta taşınır.
                    // Bayrak aşağıdaki toplu adımda 1 yapılır.
                    MIR_append_insn(ctx, func,
                        MIR_new_insn(ctx, MIR_MOV, R(instr.dest), MIR_new_int_op(ctx, 0)));
                    break;
                case Opcode::RETURN:
                    if (needsShadowFrame)
                        MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 3,
                            MIR_new_ref_op(ctx, ssLeaveProto), MIR_new_ref_op(ctx, ssLeaveImport),
                            MIR_new_reg_op(ctx, ssBaseReg)));
                    if (fnHasTry)
                        MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 2,
                            MIR_new_ref_op(ctx, traceLeaveProto), MIR_new_ref_op(ctx, traceLeaveImport)));
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 3,
                        MIR_new_ref_op(ctx, callRetNullSetProto),
                        MIR_new_ref_op(ctx, callRetNullSetImport),
                        isNullableSlot(instr.src)
                            ? MIR_new_reg_op(ctx, nullFlagRegs[static_cast<size_t>(instr.src)])
                            : MIR_new_int_op(ctx, 0)));
                    MIR_append_insn(ctx, func, MIR_new_ret_insn(ctx, 1, R(instr.src)));
                    break;
                default:
                    // opcodeSupported() yukarida zaten eledi.
                    break;
            }

            // GC kökleme — TOPLU, opcode başına DEĞİL.
            //
            // #221'deki null-bayrağı bakımıyla aynı gerekçe: kökleme her case
            // içinde elle yapılsaydı, unutulan tek bir opcode SESSİZ bir
            // use-after-free üretirdi. Nitekim öyle oldu — string üreten
            // cast'ler (CAST_*_TO_STR), STRING_CONCAT ve decimal aritmetiği
            // nesne tahsis ettikleri halde köklenmiyordu; ilk agresif-eşik
            // taramasında CAST_FLOAT32_TO_STR bunu ortaya çıkardı.
            //
            // Kural tek yerde ve istisnasız: pointer taşıyan bir dest slot'una
            // yazan HER talimat, sonucu shadow stack'e yansıtır. Zaten kendi
            // case'inde yansıtan opcode'lar (CALL, CALLHOST, ARRAY_*, ...)
            // için bu ikinci yazım zararsızdır — aynı slot aynı değerle
            // tazelenir.
            //
            // Liveness daraltması emitShadowSet içinde yapılır: sonuç bir
            // sonraki talimatta artık canlı değilse kök yazımı atlanır.
            if (instr.dest >= 0) {
                const SlotType destKind = slotKindOf(fn, instr.dest);
                if (destKind == SlotType::Str || destKind == SlotType::Decimal ||
                    destKind == SlotType::Ref)
                    emitShadowSet(instr.dest, (int)i + 1);
            }

            // #221: null bayrağı bakımı — TOPLU, opcode başına DEĞİL.
            //
            // Bu tasarım kasıtlı: bayrağı her case içinde elle ayarlasaydık,
            // unutulan tek bir opcode sessizce yanlış cevap verirdi (eski
            // null'luk sızar → `int? a; a = 5;` sonrası `a == null` true).
            // Burada kural tek yerde ve istisnasız: dest'e yazan her talimat
            // bayrağı tazeler. LOAD_NULL → 1, diğer her şey → 0.
            //
            // EQUAL_EQUAL/NOT_EQUAL kendi case'lerinde null-farkındalıklı
            // karşılaştırma üretir (aşağıdaki nullAwareCompare); onların dest'i
            // bir bool'dur ve nullable değildir, bu yüzden burada 0 yazılması
            // doğrudur.
            if (instr.dest >= 0 && isNullableSlot(instr.dest)) {
                if (instr.opcode == Opcode::LOAD_NULL) {
                    setNullFlag(instr.dest, 1);
                } else if (instr.opcode == Opcode::LOAD_SLOT) {
                    // `b = null;` IR'de LOAD_NULL + LOAD_SLOT olarak üretilir
                    // (bkz. IR dump: LOAD_NULL s11 / LOAD_SLOT s10 = s11), yani
                    // null'luk KOPYA ile taşınır. Bayrağı sıfırlamak burada
                    // sessiz yanlış cevap verirdi — kaynaktan kopyalanmalı.
                    // Kaynak nullable değilse sabit 0 kopyalanır (doğru).
                    if (isNullableSlot(instr.src))
                        MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_MOV,
                            MIR_new_reg_op(ctx, nullFlagRegs[static_cast<size_t>(instr.dest)]),
                            MIR_new_reg_op(ctx, nullFlagRegs[static_cast<size_t>(instr.src)])));
                    else
                        setNullFlag(instr.dest, 0);
                } else if (instr.opcode == Opcode::CALLHOST || instr.opcode == Opcode::CALL ||
                           instr.opcode == Opcode::FIELD_GET || isFallibleCast(instr.opcode)) {
                } else {
                    setNullFlag(instr.dest, 0);
                }
            }
            // #110: hata yayılımı — hata ÜRETEBİLEN talimatın ardından
            // rt().pendingError kontrolü.
            //
            // Koşul TALİMAT bazındadır, fonksiyon bazında DEĞİL. Eskiden
            // `canRaise[name]` idi: fonksiyonda tek bir CALLHOST/DIV/ARRAY_GET
            // bulunması, o fonksiyondaki HER talimattan sonra bir native çağrı
            // yayılmasına yol açıyordu — `mov`, `adds`, `lt` dahil, yani hata
            // üretmesi mümkün olmayanlar dahil.
            //
            // Ölçülen etki (50M turluk saf tamsayı döngüsü, Release):
            //   print YOK  → JIT  30 ms  (VM'in 56 katı hızlı)
            //   print VAR  → JIT 914 ms  (VM'in 1.9 katı)
            // Tek fark döngünün DIŞINDA, bir kez çalışan bir print'ti; o tek
            // CALLHOST bütün fonksiyonu "hata taşıyabilir" ilan edip sıcak
            // döngüye talimat başına bir çağrı ekliyordu.
            //
            // Neden güvenli: rt().pendingError yalnız errorCapable/
            // fallibleCastOp trampolinleri tarafından yazılır. `adds`'ten sonra
            // bayrağın set olmuş olması MÜMKÜN DEĞİLDİR; kontrolü oraya koymak
            // doğruluk sağlamaz, yalnız maliyet üretir.
            //
            // CALL dahildir: çağrılan fonksiyon hata yayabilir ve dönüşte
            // bayrak set olabilir. handlerTarget >= 0 (try bölgesi içi) da
            // korunur — orada akışın catch'e sapması gerekebilir.
            // Sıfıra bölme opcode'ları BU KONTROLDEN MUAFTIR: hata dallarını
            // kendileri yayılıma bağlar (emitJumpToErrorTarget, yukarıda).
            // Genel kontrolün yapacağı iş — rt_jit_error_pending çağırıp
            // bayrağı sorgulamak — orada gereksizdir, cevap zaten bilinir.
            // DDIV/DMOD (decimal) muaf DEĞİLDİR: onların hata yolu trampolinin
            // içindedir, kodgen hatanın oluşup oluşmadığını göremez.
            auto selfPropagatingDiv = [](Opcode op) {
                switch (op) {
                    case Opcode::DIV: case Opcode::MOD:
                    case Opcode::LDIV: case Opcode::LMOD:
                    case Opcode::FDIV: case Opcode::F32DIV:
                        return true;
                    default:
                        return false;
                }
            };
            const bool mayRaiseHere = (errorCapable(instr.opcode) &&
                                       !selfPropagatingDiv(instr.opcode)) ||
                                      fallibleCastOp(instr.opcode) ||
                                      instr.opcode == Opcode::CALL;
            if (instr.opcode != Opcode::RETURN &&
                (mayRaiseHere || handlerTarget[i] >= 0)) {
                MIR_reg_t pending = newTmp("pending");
                MIR_label_t noError = MIR_new_label(ctx);
                MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 3,
                    MIR_new_ref_op(ctx, errPendingProto),
                    MIR_new_ref_op(ctx, errPendingImport),
                    MIR_new_reg_op(ctx, pending)));
                MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_BF,
                    MIR_new_label_op(ctx, noError), MIR_new_reg_op(ctx, pending)));
                if (handlerTarget[i] >= 0) {
                    int errorSlot = handlerErrorSlot[i];
                    MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 3,
                        MIR_new_ref_op(ctx, errTakeProto),
                        MIR_new_ref_op(ctx, errTakeImport), R(errorSlot)));
                    emitShadowSet(errorSlot, (int)i + 1);
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_JMP,
                        MIR_new_label_op(ctx, labelAt[static_cast<size_t>(handlerTarget[i])])));
                } else {
                    MIR_append_insn(ctx, func, MIR_new_insn(ctx, MIR_JMP,
                        MIR_new_label_op(ctx, propagateLabel)));
                }
                MIR_append_insn(ctx, func, noError);
            }
        }

        // #165: fonksiyon sonu etiketi — switch sonrası "JMP → instrN"
        // (past-the-end) hedefi bu etikete gider. RETURN'den sonra asla
        // yürütülmez ama JIT'in geçerli bir hedefi olmalı.
        MIR_append_insn(ctx, func, labelAt[instrN]);
        MIR_append_insn(ctx, func, propagateLabel);
        // Fonksiyon-sonu çıkış yolu (sentinel/propagate) — RETURN opcode'u
        // olmadan buraya düşen akış için çerçeve burada geri sarılır.
        if (needsShadowFrame)
            MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 3,
                MIR_new_ref_op(ctx, ssLeaveProto), MIR_new_ref_op(ctx, ssLeaveImport),
                MIR_new_reg_op(ctx, ssBaseReg)));
        if (fnHasTry)
            MIR_append_insn(ctx, func, MIR_new_call_insn(ctx, 2,
                MIR_new_ref_op(ctx, traceLeaveProto), MIR_new_ref_op(ctx, traceLeaveImport)));
        MIR_op_t zeroRet = ret == MIR_T_D ? MIR_new_double_op(ctx, 0.0)
                           : ret == MIR_T_F ? MIR_new_float_op(ctx, 0.0f)
                                            : MIR_new_int_op(ctx, 0);
        MIR_append_insn(ctx, func, MIR_new_ret_insn(ctx, 1, zeroRet));

        MIR_finish_func(ctx);
    }

    MIR_finish_module(ctx);
    MIR_load_module(ctx, mod);
    MIR_load_external(ctx, "rt_jit_shadow_set",  reinterpret_cast<void*>(rt_jit_shadow_set));
    MIR_load_external(ctx, "rt_jit_shadow_enter", reinterpret_cast<void*>(rt_jit_shadow_enter));
    MIR_load_external(ctx, "rt_jit_shadow_leave", reinterpret_cast<void*>(rt_jit_shadow_leave));
    MIR_load_external(ctx, "rt_jit_array_new",   reinterpret_cast<void*>(rt_jit_array_new));
    MIR_load_external(ctx, "rt_jit_array_len",   reinterpret_cast<void*>(rt_jit_array_len));
    MIR_load_external(ctx, "rt_jit_array_bounds_fail", reinterpret_cast<void*>(rt_jit_array_bounds_fail));
    MIR_load_external(ctx, "rt_jit_array_get_i", reinterpret_cast<void*>(rt_jit_array_get_i));
    MIR_load_external(ctx, "rt_jit_array_get_d", reinterpret_cast<void*>(rt_jit_array_get_d));
    MIR_load_external(ctx, "rt_jit_array_get_p", reinterpret_cast<void*>(rt_jit_array_get_p));
    MIR_load_external(ctx, "rt_jit_array_set_i", reinterpret_cast<void*>(rt_jit_array_set_i));
    MIR_load_external(ctx, "rt_jit_array_set_d", reinterpret_cast<void*>(rt_jit_array_set_d));
    MIR_load_external(ctx, "rt_jit_array_set_p", reinterpret_cast<void*>(rt_jit_array_set_p));
    MIR_load_external(ctx, "rt_jit_struct_new",  reinterpret_cast<void*>(rt_jit_struct_new));
    MIR_load_external(ctx, "rt_jit_field_get_i", reinterpret_cast<void*>(rt_jit_field_get_i));
    MIR_load_external(ctx, "rt_jit_field_get_d", reinterpret_cast<void*>(rt_jit_field_get_d));
    MIR_load_external(ctx, "rt_jit_field_get_p", reinterpret_cast<void*>(rt_jit_field_get_p));
    MIR_load_external(ctx, "rt_jit_field_is_null", reinterpret_cast<void*>(rt_jit_field_is_null));
    MIR_load_external(ctx, "rt_jit_field_set_i", reinterpret_cast<void*>(rt_jit_field_set_i));
    MIR_load_external(ctx, "rt_jit_field_set_d", reinterpret_cast<void*>(rt_jit_field_set_d));
    MIR_load_external(ctx, "rt_jit_field_set_p", reinterpret_cast<void*>(rt_jit_field_set_p));
    MIR_load_external(ctx, "rt_jit_host_arg_i",  reinterpret_cast<void*>(rt_jit_host_arg_i));
    MIR_load_external(ctx, "rt_jit_host_arg_d",  reinterpret_cast<void*>(rt_jit_host_arg_d));
    MIR_load_external(ctx, "rt_jit_host_arg_nullable_i", reinterpret_cast<void*>(rt_jit_host_arg_nullable_i));
    MIR_load_external(ctx, "rt_jit_host_arg_nullable_d", reinterpret_cast<void*>(rt_jit_host_arg_nullable_d));
    MIR_load_external(ctx, "rt_jit_host_call",   reinterpret_cast<void*>(rt_jit_host_call));
    MIR_load_external(ctx, "rt_jit_host_call_d", reinterpret_cast<void*>(rt_jit_host_call_d));
    MIR_load_external(ctx, "rt_jit_host_ret_is_null", reinterpret_cast<void*>(rt_jit_host_ret_is_null));
    MIR_load_external(ctx, "rt_jit_call_arg_null_set", reinterpret_cast<void*>(rt_jit_call_arg_null_set));
    MIR_load_external(ctx, "rt_jit_call_arg_null_get", reinterpret_cast<void*>(rt_jit_call_arg_null_get));
    MIR_load_external(ctx, "rt_jit_call_ret_null_set", reinterpret_cast<void*>(rt_jit_call_ret_null_set));
    MIR_load_external(ctx, "rt_jit_call_ret_null_get", reinterpret_cast<void*>(rt_jit_call_ret_null_get));
    MIR_load_external(ctx, "rt_jit_error_location", reinterpret_cast<void*>(rt_jit_error_location));
    MIR_load_external(ctx, "rt_jit_error_pending", reinterpret_cast<void*>(rt_jit_error_pending));
    MIR_load_external(ctx, "rt_jit_error_take", reinterpret_cast<void*>(rt_jit_error_take));
    MIR_load_external(ctx, "rt_jit_throw_p", reinterpret_cast<void*>(rt_jit_throw_p));
    MIR_load_external(ctx, "rt_jit_trace_enter", reinterpret_cast<void*>(rt_jit_trace_enter));
    MIR_load_external(ctx, "rt_jit_trace_leave", reinterpret_cast<void*>(rt_jit_trace_leave));
    MIR_load_external(ctx, "rt_jit_global_load_i", reinterpret_cast<void*>(rt_jit_global_load_i));
    MIR_load_external(ctx, "rt_jit_global_store_i", reinterpret_cast<void*>(rt_jit_global_store_i));
    MIR_load_external(ctx, "rt_jit_global_load_d", reinterpret_cast<void*>(rt_jit_global_load_d));
    MIR_load_external(ctx, "rt_jit_global_store_d", reinterpret_cast<void*>(rt_jit_global_store_d));
    MIR_load_external(ctx, "rt_jit_global_load_p", reinterpret_cast<void*>(rt_jit_global_load_p));
    MIR_load_external(ctx, "rt_jit_global_store_p", reinterpret_cast<void*>(rt_jit_global_store_p));
    MIR_load_external(ctx, "rt_jit_print_int",   reinterpret_cast<void*>(rt_jit_print_int));
    MIR_load_external(ctx, "rt_jit_print_float", reinterpret_cast<void*>(rt_jit_print_float));
    MIR_load_external(ctx, "rt_jit_print_float32", reinterpret_cast<void*>(rt_jit_print_float32));
    MIR_load_external(ctx, "rt_jit_print_str",   reinterpret_cast<void*>(rt_jit_print_str));
    MIR_load_external(ctx, "rt_jit_long_to_str",    reinterpret_cast<void*>(rt_jit_long_to_str));
    MIR_load_external(ctx, "rt_jit_float32_to_str", reinterpret_cast<void*>(rt_jit_float32_to_str));
    MIR_load_external(ctx, "rt_jit_string_concat", reinterpret_cast<void*>(rt_jit_string_concat));
    MIR_load_external(ctx, "rt_jit_string_eq",     reinterpret_cast<void*>(rt_jit_string_eq));
    MIR_load_external(ctx, "rt_jit_decimal_cmp",   reinterpret_cast<void*>(rt_jit_decimal_cmp));
    MIR_load_external(ctx, "rt_jit_int_to_str",           reinterpret_cast<void*>(rt_jit_int_to_str));
    MIR_load_external(ctx, "rt_jit_float_to_str",         reinterpret_cast<void*>(rt_jit_float_to_str));
    MIR_load_external(ctx, "rt_jit_bool_to_str",          reinterpret_cast<void*>(rt_jit_bool_to_str));
    MIR_load_external(ctx, "rt_jit_str_to_int",           reinterpret_cast<void*>(rt_jit_str_to_int));
    MIR_load_external(ctx, "rt_jit_str_to_float",         reinterpret_cast<void*>(rt_jit_str_to_float));
    MIR_load_external(ctx, "rt_jit_float_to_int_checked", reinterpret_cast<void*>(rt_jit_float_to_int_checked));
    MIR_load_external(ctx, "rt_jit_int_to_byte_checked",  reinterpret_cast<void*>(rt_jit_int_to_byte_checked));
    MIR_load_external(ctx, "rt_jit_str_to_long",           reinterpret_cast<void*>(rt_jit_str_to_long));
    MIR_load_external(ctx, "rt_jit_str_to_float32",        reinterpret_cast<void*>(rt_jit_str_to_float32));
    MIR_load_external(ctx, "rt_jit_long_to_int_checked",   reinterpret_cast<void*>(rt_jit_long_to_int_checked));
    MIR_load_external(ctx, "rt_jit_float_to_long_checked", reinterpret_cast<void*>(rt_jit_float_to_long_checked));
    MIR_load_external(ctx, "rt_jit_cast_begin", reinterpret_cast<void*>(rt_jit_cast_begin));
    MIR_load_external(ctx, "rt_jit_cast_ret_is_null", reinterpret_cast<void*>(rt_jit_cast_ret_is_null));
    MIR_load_external(ctx, "rt_jit_decimal_add", reinterpret_cast<void*>(rt_jit_decimal_add));
    MIR_load_external(ctx, "rt_jit_decimal_sub", reinterpret_cast<void*>(rt_jit_decimal_sub));
    MIR_load_external(ctx, "rt_jit_decimal_mul", reinterpret_cast<void*>(rt_jit_decimal_mul));
    MIR_load_external(ctx, "rt_jit_decimal_div", reinterpret_cast<void*>(rt_jit_decimal_div));
    MIR_load_external(ctx, "rt_jit_decimal_mod", reinterpret_cast<void*>(rt_jit_decimal_mod));
    MIR_load_external(ctx, "rt_jit_decimal_neg", reinterpret_cast<void*>(rt_jit_decimal_neg));
    MIR_load_external(ctx, "rt_jit_int_to_decimal",   reinterpret_cast<void*>(rt_jit_int_to_decimal));
    MIR_load_external(ctx, "rt_jit_float_to_decimal", reinterpret_cast<void*>(rt_jit_float_to_decimal));
    MIR_load_external(ctx, "rt_jit_decimal_to_str",   reinterpret_cast<void*>(rt_jit_decimal_to_str));
    MIR_load_external(ctx, "rt_jit_decimal_to_int",   reinterpret_cast<void*>(rt_jit_decimal_to_int));
    MIR_load_external(ctx, "rt_jit_decimal_to_float", reinterpret_cast<void*>(rt_jit_decimal_to_float));
    MIR_load_external(ctx, "rt_jit_str_to_decimal",   reinterpret_cast<void*>(rt_jit_str_to_decimal));
    MIR_load_external(ctx, "rt_jit_print_decimal",    reinterpret_cast<void*>(rt_jit_print_decimal));
    MIR_load_external(ctx, "rt_jit_div_zero",    reinterpret_cast<void*>(rt_jit_div_zero));
    MIR_load_external(ctx, "rt_jit_mod_zero",    reinterpret_cast<void*>(rt_jit_mod_zero));
    MIR_load_external(ctx, "rt_jit_fdiv_zero",   reinterpret_cast<void*>(rt_jit_fdiv_zero));

    MIR_gen_init(ctx);
    //MIR_output(ctx,stdout);
    // MIRPLAN.md §0: optimizasyon seviyesi determinizm gerekcesiyle
    // kisitlanmaz — MIR'in gcc -O2'yle kiyaslandigi seviye varsayilan.
    MIR_gen_set_optimize_level(ctx, 2);
    MIR_link(ctx, MIR_set_gen_interface, nullptr);

    // Tum fonksiyonlari onceden JIT'le (lazy-gen'e guvenmiyoruz — cam kutu
    // ilkesi: derleme zamani tumuyle burada belirlenmis olsun).
    void* mainPtr = nullptr;
    for (auto& name : program.functionOrder) {
        void* p = MIR_gen(ctx, funcMap.at(name).callRef);
        if (name == "main") mainPtr = p;
    }

    profWarmup.reset();  // "jit-warmup" burada biter

    using SaqutMainFn = int64_t (*)(void);
    auto    compiled = reinterpret_cast<SaqutMainFn>(mainPtr);
    int64_t nativeResult = 0;
    const int runs = std::max(1, executionRuns);
    if (executionSamplesUs)
        executionSamplesUs->clear();
    for (int run = 0; run < runs; ++run) {
        auto execStart = std::chrono::steady_clock::now();
        {
            profiling::StageTimer::ScopedStage profExec(profiler, "jit-exec");
            nativeResult = compiled();
        }
        auto execEnd = std::chrono::steady_clock::now();
        if (executionSamplesUs) {
            executionSamplesUs->push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                execEnd - execStart).count());
        }
        if (executionProgress)
            executionProgress(run + 1, runs);
    }

    std::string uncaughtMessage;
    if (rt().pendingError) {
        if (rt().pendingError->fields.size() > 2 &&
            rt().pendingError->fields[2].kind == ValueKind::String)
            uncaughtMessage = rt().pendingError->fields[2].stringValue();
        if (uncaughtMessage.empty()) uncaughtMessage = "uncaught error";
        rt().pendingError = nullptr;
    }

    MIR_gen_finish(ctx);
    MIR_finish(ctx);

    // Çalışma-zamanı üretilen string/decimal nesnelerini topla — native kod bitti,
    // pointer'lara artık erişilmiyor (GC Dilim 2/§8'e kadar elle temizlik).

    if (!uncaughtMessage.empty()) {
        std::cerr << "runtime error: " << uncaughtMessage << "\n";
        outExitCode = saqut::exit_code::kSoftwareError;
        return true;
    }

    outExitCode = static_cast<int>(nativeResult);
    return true;
}

const GcStats& lastRunGcStats() { return lastRunGcStatsStorage(); }

void setGcThresholdForNextRun(int bytes) { gcThresholdForNextRunStorage() = bytes; }

}  // namespace mir_backend
