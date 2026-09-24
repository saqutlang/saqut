// ============================================================================
// saQut — JIT çalışma bağlamı tipleri (JitRuntime ve yardımcıları)
//
// DİZİN:   src/runtime/jit_runtime.hpp
// KATMAN:  runtime — Isolate üyesi olan JIT çalışma durumu
//
// Bu tanımlar daha önce src/mir/mir_backend.cpp içindeki anonim namespace'te
// duruyordu. ADR-045 (Faz 1) ile JitRuntime, Isolate'in bir üyesi oldu; bu
// yüzden bir başlığa taşındı. Çağrı noktaları değişmedi: rt() hâlâ tek
// erişim noktasıdır (gövdesi artık Isolate::current()).
// ============================================================================

#ifndef SAQUT_RUNTIME_JIT_RUNTIME
#define SAQUT_RUNTIME_JIT_RUNTIME

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gc/gc_object.hpp"      // StructObject, StringObject, DecimalObject
#include "ffi/host_abi.hpp"      // HostSlot, HostEnv, HostCallFrame
#include "ffi/host_bridge.hpp"   // HostRetOwner

struct Heap;                          // gc/gc_heap.hpp
namespace mir_backend { struct JitCallCounters; }

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

// JIT'in ÇALIŞMA ZAMANI durumu. Isolate üyesidir (ADR-045): her iş parçacığı
// kendi örneğini görür; başka bir isolate'in durumuna dokunulmaz.
//
// ALAN SINIFI (S2 tablosu): aşağıdaki alanların tamamı KOŞU sırasında
// yazılır → Isolate. Derleme sırasında dolan `structMeta` c1'de
// CompiledProgram'a taşındı (program düzeyi, thread başına kopyalanmaz).
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
    mir_backend::JitCallCounters* benchCounters = nullptr;

    // Deterministik iz yığını (ADR-025) — yalnızca try'lı fonksiyonlar.
    std::vector<JitTraceFrame> traceStack;

    // #254: saQut çağrı derinliği (VM callStack_ boyunun karşılığı) ve
    // native yığın koruması. stackBase çalıştırma girişinde kaydedilir.
    int64_t     callDepth  = 0;
    const char* stackBase  = nullptr;
    size_t      stackBudget = 0;

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

#endif // SAQUT_RUNTIME_JIT_RUNTIME
