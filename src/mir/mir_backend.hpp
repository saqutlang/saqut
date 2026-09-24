// ============================================================================
// saQut MIR JIT — Fast-mode Dış Arayüz (Dilim 1: int-skaler + kontrol akışı
// + fonksiyon çağrısı + print, #80, MIRPLAN.md)
//
// KATMAN: Backend — VM'e (Interpreter) alternatif ikinci çalıştırma yolu.
// İZOLASYON: Bu dosya <mir.h>/<mir-gen.h> BİLMEZ (MIRPLAN.md §1 kuralı).
//   MIR header'ları YALNIZCA mir_backend.cpp'ye include edilir.
//
// KURAL — KISMİ JIT YOK: --jit istendiğinde programın TAMAMI (main'den
// erişilebilir olsun olmasın, program.functions'daki HER fonksiyon)
// desteklenen opcode kümesinde olmalı. Tek bir desteklenmeyen opcode bile
// bulunsa PROGRAMIN TAMAMI reddedilir — VM'e sessiz düşme YOK. Çağıran
// taraf (run.hpp) false dönüşünde açık bir hata basıp çıkmalı.
//
// KAPSAM (Dilim 1): LOAD_CONST/LOAD_SLOT, tam sayı aritmetiği (ADD/SUB/MUL/
// DIV/MOD — sıfıra bölme fatal hata), bitsel (BAND/BOR/BXOR/SHL/SHR/BNOT),
// karşılaştırma (LESS/LESS_EQUAL/GREATER/GREATER_EQUAL/EQUAL_EQUAL/
// NOT_EQUAL), kontrol akışı (JMP/JIF_FALSE/JIF_TRUE), fonksiyon çağrısı
// (CALL/RETURN, tüm parametre/dönüş tipleri int), ve tek argümanlı
// print(int) (CALLHOST). Struct/array/string/decimal/date/cast/try-catch/
// FFI YOK — bu opcode'lardan biri görülürse program reddedilir.
// ============================================================================

#ifndef SAQUT_MIR_BACKEND
#define SAQUT_MIR_BACKEND

#include <string>
#include <vector>
#include <functional>
#include "ir/ir_program.hpp"
#include "profiling/stage_timer.hpp"
#include "gc/gc_heap.hpp"

namespace mir_backend {

// Programı desteklenen opcode kümesi açısından reddetme sebebini taşır.
struct UnsupportedReason {
    std::string functionName;
    std::string opcodeName;
};

// JIT çalıştırma sırasında çağrı sayaçları.
// nullptr ise sayaç artırılmaz (timing modu — sıfır ek yük).
struct JitCallCounters {
    uint64_t* callhost = nullptr;
    uint64_t* ffi      = nullptr;
    uint64_t* builtin  = nullptr;
};

// Programın TAMAMINI MIR ile native koda derleyip main()'i gerçekten
// çalıştırmayı dener. Başarılıysa true döner, outExitCode main'in RETURN
// değerini taşır — bu durumda program uçtan uca JIT'lenmiştir, VM hiç
// devreye girmemiştir. Desteklenmeyen bir opcode/fonksiyon görülürse HİÇBİR
// ŞEY çalıştırmadan false döner, outReason sebebi taşır.
//
// profiler != nullptr ise iki aşama ayrı raporlanır:
//   "jit-warmup" — IR->MIR çeviri + gerçek native koda derleme (MIR_gen)
//   "jit-exec"   — yalnızca derlenmiş native main()'in ÇALIŞTIRILMASI
bool tryCompileAndRunProgram(IRProgram& program, int& outExitCode,
                              UnsupportedReason& outReason,
                              const std::vector<std::string>& programArgs,
                              Profiling::StageTimer* profiler = nullptr,
                              JitCallCounters* counters = nullptr,
                              int executionRuns = 1,
                              std::vector<long long>* executionSamplesUs = nullptr,
                              const std::function<void(int, int)>& executionProgress = {});

// Bir sonraki JIT koşusunun GC eşiği (bayt). > 0 → eşik, <= 0 → toplama
// kapalı, ayarlanmazsa Heap'in varsayılanı geçerlidir. VM'deki
// Interpreter::setGCThreshold ile aynı sözleşme — --gc-threshold iki
// backend'de de aynı anlama gelir.
void setGcThresholdForNextRun(int bytes);

// Son JIT koşusunun GC istatistiği. Koşu heap'i koşu bitince yıkıldığından
// (ömrü koşuya bağlıdır) sayaçlar yıkımdan hemen önce buraya kopyalanır —
// --gc-stats bunu okur. Hiç JIT koşusu yapılmadıysa alanlar sıfırdır.
const GcStats& lastRunGcStats();

}  // namespace mir_backend

#endif  // SAQUT_MIR_BACKEND
