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
struct CompiledProgram;

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

    // Koşulan program (runOnIsolate RAII guard'ı bağlar). Faz 2'de spawn için
    // fonksiyon giriş tablosu ve serileştirme için tip tablosu da buradan
    // okunacak. Guard yokken (LSP/birim testleri) nullptr olabilir.
    const CompiledProgram* program = nullptr;

    // Koşu yolunda t_isolate guard tarafından bağlıdır; current() bunu
    // varsayar (Faz 1 ileriki adımında assert'e dönecek). LSP ve birim
    // testleri gibi guard'sız yollar currentOrCreate() kullanır.
    static Isolate& current();
    static Isolate& currentOrCreate();
};

// İş parçacığı başına tek örnek. C++17 inline değişken: tüm çeviri birimleri
// aynı (thread başına) örneği görür.
inline thread_local Isolate* t_isolate = nullptr;

inline Isolate& Isolate::currentOrCreate() {
    if (!t_isolate) t_isolate = new Isolate();
    return *t_isolate;
}

// Geçiş dönemi: current() şimdilik lazy (currentOrCreate ile aynı). Faz 1
// ileriki adımında run yolu guard'ı bağladıktan sonra current() assert'e
// dönecek; guard'sız yollar currentOrCreate() kullanmalı.
inline Isolate& Isolate::current() { return currentOrCreate(); }

// RAII guard: bir koşu boyunca thread'in isolate'ini bağlar, çıkışta önceki
// değeri geri koyar (S4).
class IsolateGuard {
public:
    explicit IsolateGuard(Isolate& iso) : prev_(t_isolate) { t_isolate = &iso; }
    ~IsolateGuard() { t_isolate = prev_; }
    IsolateGuard(const IsolateGuard&)            = delete;
    IsolateGuard& operator=(const IsolateGuard&) = delete;
private:
    Isolate* prev_;
};

#endif // SAQUT_RUNTIME_ISOLATE
