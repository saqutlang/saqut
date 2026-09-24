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
// c1: compile/run ayrımı (CompiledProgram + runOnIsolate); JIT HostEnv
// isolate üyesi oldu (önce mir_backend.cpp'de `static HostEnv jitEnv`).
//
// HENÜZ TAŞINMADI (faz1 devamı): ConstPool (c3), Interpreter globalSlots_/Heap, FileRegistry/
// FfiCatalog freeze(). Tek iş parçacıklı davranış bu adımda birebir korunur:
// tek thread'de tek Isolate oluşur ve alanlar eskisi gibi çalışır.
// ============================================================================

#ifndef SAQUT_RUNTIME_ISOLATE
#define SAQUT_RUNTIME_ISOLATE

#include <cassert>
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

    // JIT host çağrılarının ortamı (programArgs, koşu heap'i). runOnIsolate
    // doldurur ve rt().hostEnv'e bağlar (önce süreç-global static'ti).
    HostEnv jitEnv;

    // current(): bağlı isolate'i döndürür; bağlı değilse debug'da assert
    // (c2). Süreç girişi (main.cpp) ana thread isolate'ini bağlar; koşu
    // yolları IsolateGuard ile bağlar. Lazy oluşturma YALNIZ
    // currentOrCreate()'tedir (guard'sız birim testleri ve araçlar).
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

// Sıcak yol: kontrolsüz TLS okuması; debug'da bağlı olma assert'i.
inline Isolate& Isolate::current() {
    assert(t_isolate && "Isolate::current(): bağlı isolate yok (IsolateGuard?)");
    return *t_isolate;
}

// RAII guard: bir koşu boyunca thread'in isolate'ini ve (verilmişse) koşulan
// programı bağlar, çıkışta önceki değerleri geri koyar (S4).
class IsolateGuard {
public:
    explicit IsolateGuard(Isolate& iso, const CompiledProgram* program = nullptr)
        : prev_(t_isolate), iso_(iso), prevProgram_(iso.program) {
        t_isolate = &iso;
        if (program) iso.program = program;
    }
    ~IsolateGuard() {
        iso_.program = prevProgram_;
        t_isolate    = prev_;
    }
    IsolateGuard(const IsolateGuard&)            = delete;
    IsolateGuard& operator=(const IsolateGuard&) = delete;
private:
    Isolate*               prev_;
    Isolate&               iso_;
    const CompiledProgram* prevProgram_;
};

// RAII: kapsam boyunca thread'i isolate'siz bırakır (ADR-045 §RUNTIME 6:
// compileProgram hiçbir isolate bağlı değilken çalışır). Codegen'de kalan bir
// rt()/current() erişimi debug'da assert'e takılır.
class NoIsolateScope {
public:
    NoIsolateScope() : prev_(t_isolate) { t_isolate = nullptr; }
    ~NoIsolateScope() { t_isolate = prev_; }
    NoIsolateScope(const NoIsolateScope&)            = delete;
    NoIsolateScope& operator=(const NoIsolateScope&) = delete;
private:
    Isolate* prev_;
};

#endif // SAQUT_RUNTIME_ISOLATE
