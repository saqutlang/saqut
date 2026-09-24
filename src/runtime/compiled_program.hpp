// ============================================================================
// saQut — CompiledProgram (ADR-045, Faz 1)
//
// DİZİN:   src/runtime/compiled_program.hpp
// KATMAN:  runtime — bir kez derlenen, tüm isolate'lerin paylaştığı program
//
// compileProgram() doldurur; derlemeden sonra SALT OKUNURDUR (activeIsolates
// sayacı hariç). Program düzeyindeki veri thread başına kopyalanmaz: koşu
// yolu Isolate::program üzerinden okur (S1).
//
// Yıkım sırası (ADR-045 "CompiledProgram ömrü"): tüm isolate'ler koşuyu
// bitirir (activeIsolates == 0) → MIR_gen_finish → MIR_finish. Yıkıcı
// mir_backend.cpp'dedir (MIR başlıkları burada açılmaz).
//
// Koda gömülebilen her adres (ADR-045 §RUNTIME 6) bu nesnenin sahip olduğu
// veriye işaret eder: ConstPool nesneleri ve programStrings. Kayıt defteri
// (embeddable_) embedProgramPtr'nin debug üyelik assert'ini besler (c3).
// ============================================================================

#ifndef SAQUT_RUNTIME_COMPILED_PROGRAM
#define SAQUT_RUNTIME_COMPILED_PROGRAM

#include <atomic>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "runtime/const_pool.hpp"
#include "runtime/jit_runtime.hpp"   // JitStructMeta

struct MIR_context;   // mir.h: typedef struct MIR_context *MIR_context_t;

struct CompiledProgram {
    // Opaque MIR bağlamı; derleme sonrası hiçbir MIR_* çağrısı yapılmaz
    // (eager gen), yalnız yıkıcı kapatır.
    MIR_context* mirCtx = nullptr;

    // Fonksiyon giriş tablosu. c1'de yalnız main gerekir; Faz 2 spawn'ı
    // diğer girişleri ekleyecek.
    void* mainEntry = nullptr;

    // Global slot sayısı (her isolate kendi global kopyasını bu boyutta kurar).
    int globalCount = 0;

    // ADR-045 (Faz 3-f): fonksiyon giriş tablosu (THREAD_SPAWN hedefi
    // __thread_* fonksiyonları dahil), shared slot tanımları, thread bayrağı.
    std::unordered_map<std::string, void*>   entries;
    std::vector<std::pair<int, std::string>> sharedSlots;   // (kind, ad)
    bool                                     usesThreads = false;

    // STRUCT_NEW metadata'sı. Koda metaId (indeks) olarak gömülür, eleman
    // pointer'ı değil — vektörün derleme sırasında yeniden tahsisi güvenlidir.
    std::vector<JitStructMeta> structMeta;

    // Bu programı koşan isolate sayısı; yıkıcı sıfır olmasını assert eder.
    mutable std::atomic<int> activeIsolates{0};

    // String/decimal literalleri (immortal, program-ömürlü; c3).
    ConstPool constPool;

    // Koda gömülen C-string'ler (trace fn.name/dosya). IRProgram'a değil bu
    // nesneye aittir; deque eleman adresleri push_back'te değişmez.
    std::deque<std::string> programStrings;

    StringObject* internString(const std::string& s) {
        StringObject* o = constPool.internString(s);
        embeddable_.insert(o);
        return o;
    }
    DecimalObject* internDecimal(const DecimalValue& v) {
        DecimalObject* o = constPool.internDecimal(v);
        embeddable_.insert(o);
        return o;
    }
    const char* internProgramString(const std::string& s) {
        programStrings.push_back(s);
        const char* p = programStrings.back().c_str();
        embeddable_.insert(p);
        return p;
    }
    // p koda gömülebilir mi (bu programın sahip olduğu veri mi)?
    bool ownsEmbeddable(const void* p) const { return embeddable_.count(p) != 0; }

    CompiledProgram() = default;
    ~CompiledProgram();
    CompiledProgram(const CompiledProgram&)            = delete;
    CompiledProgram& operator=(const CompiledProgram&) = delete;

private:
    std::unordered_set<const void*> embeddable_;
};

#endif // SAQUT_RUNTIME_COMPILED_PROGRAM
