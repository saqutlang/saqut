// ============================================================================
// saQut — Isolate (ADR-045, Faz 1)
//
// DİZİN:   src/runtime/isolate.hpp
// KATMAN:  runtime — iş parçacığı başına izole yürütme durumu
//
// Her OS thread'i bir **isolate**'tır: kendi Heap'i, GC kökleri, JIT çalışma
// bağlamı ve global slot kopyası. Bir isolate başka bir isolate'in heap'ine
// asla dokunmaz (ADR-045 §RUNTIME 1).
//
// BU ADIM (faz1, adım 1-2): daha önce dağınık duran thread_local depolar ve
// JIT çalışma bağlamı tek bir Isolate nesnesinde toplanır:
//   - ShadowStack        (önce: src/gc/shadow_stack.cpp thread_local)
//   - string heap kancası (önce: src/gc/gc_heap.cpp t_activeStringHeap)
//   - RNG durumu          (önce: src/ffi/functions/sys.cpp thread_local)
//   - JitRuntime          (önce: mir_backend.cpp g_jitRuntime global'i)
//
// HENÜZ TAŞINMADI (faz1 devamı): ConstPool, compile/run ayrımı (structMeta'nın
// CompiledProgram'a alınması), Interpreter globalSlots_/Heap, FileRegistry/
// FfiCatalog freeze(). Tek iş parçacıklı davranış bu adımda birebir korunur:
// tek thread'de tek Isolate oluşur ve alanlar eskisi gibi çalışır.
// ============================================================================

#ifndef SAQUT_RUNTIME_ISOLATE
#define SAQUT_RUNTIME_ISOLATE

#include <cstdint>
#include <random>
#include <vector>

#include "gc/shadow_stack.hpp"
#include "runtime/jit_runtime.hpp"

struct Heap;

// Not: yaşam süresi bugün süreç/thread ömrü (new ile ayrılır, serbest
// bırakılmaz). Faz 1 devamında isolate yaşam döngüsü sahipli hale gelecek.
struct Isolate {
    // JIT çalışma bağlamı (önce g_jitRuntime global'iydi).
    JitRuntime jit;

    // JIT canlı referans kökleri (GC bu diziyi tarar).
    ShadowStack shadow;

    // Value::fromString hedefi: aktif heap. nullptr ise yedek havuz kullanılır
    // (bkz. gc_heap.cpp allocValueString).
    Heap* stringHeap = nullptr;

    // sys modülü RNG durumu (mt19937_64 sırayı korumaz; thread başına ayrı).
    std::mt19937_64 rng{std::random_device{}()};

    static Isolate& current();
};

// İş parçacığı başına tek örnek. C++17 inline değişken: tüm çeviri birimleri
// aynı (thread başına) örneği görür.
inline thread_local Isolate* t_isolate = nullptr;

inline Isolate& Isolate::current() {
    if (!t_isolate) t_isolate = new Isolate();
    return *t_isolate;
}

#endif // SAQUT_RUNTIME_ISOLATE
