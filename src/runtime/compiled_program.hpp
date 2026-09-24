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
// HENÜZ TAŞINMADI: ConstPool (c3), sahipliğin run.hpp'ye alınması (D5).
// ============================================================================

#ifndef SAQUT_RUNTIME_COMPILED_PROGRAM
#define SAQUT_RUNTIME_COMPILED_PROGRAM

#include <atomic>
#include <vector>

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

    // STRUCT_NEW metadata'sı. Koda metaId (indeks) olarak gömülür, eleman
    // pointer'ı değil — vektörün derleme sırasında yeniden tahsisi güvenlidir.
    std::vector<JitStructMeta> structMeta;

    // Bu programı koşan isolate sayısı; yıkıcı sıfır olmasını assert eder.
    mutable std::atomic<int> activeIsolates{0};

    CompiledProgram() = default;
    ~CompiledProgram();
    CompiledProgram(const CompiledProgram&)            = delete;
    CompiledProgram& operator=(const CompiledProgram&) = delete;
};

#endif // SAQUT_RUNTIME_COMPILED_PROGRAM
