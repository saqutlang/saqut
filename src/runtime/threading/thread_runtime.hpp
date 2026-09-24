// ============================================================================
// saQut — Thread çalışma zamanı ortak katmanı (ADR-045 Faz 3-e/3-f)
//
// DİZİN:   src/runtime/threading/thread_runtime.hpp
// KATMAN:  runtime/threading — VM ve JIT'in paylaştığı dil-yüzeyi yardımcıları
//
// Backend'ler (Interpreter, MIR JIT trampolinleri) thread opcode'larını bu
// katman üzerinden Faz 2 primitiflerine indirir:
//   programBegin/End  — ana thread kaydı, SharedSlots kurulumu, park öncesi
//                       GC kancası; main dönüşünde tüm thread'lerin join'i
//   kilitler          — isolate başına tutulan-kilit listesi (slot, etiket);
//                       tutulmayan kilidi bırakmak etkisizdir
//   durdurma          — ThreadStopRequested (VM'de C++ istisnası olarak
//                       thread girişine kadar çözülür)
// ============================================================================

#ifndef SAQUT_RUNTIME_THREADING_THREAD_RUNTIME
#define SAQUT_RUNTIME_THREADING_THREAD_RUNTIME

#include <atomic>
#include <cstdint>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace saqut::threading {

// VM: bloklayan bir opcode stop ile döndüğünde ya da geri kenar yoklaması
// stop bitini gördüğünde atılır; saQut try/catch'i (VM-içi) bunu yakalamaz.
struct ThreadStopRequested {};

// Program başı (ana thread, spawn öncesi). slots: (kind, ad) — kind
// IRProgram::SharedSlotDesc ile aynı (0 int, 1 float, 2 bool, 3 Pool, 4 List).
void programBegin(const std::vector<std::pair<int, std::string>>& slots);
// main dönüşü: diğer tüm thread'ler bitene kadar bekler (deadlock dedektörü
// aktif), sonra OS thread'lerini join eder.
void programEnd();

// Çağıran thread'in stop token'ı (kayıtsız thread'de boş token).
std::stop_token currentStopToken();
// Çağıran thread'in yoklama bayrak kelimesi (kayıtsızsa nullptr).
std::atomic<uint32_t>* currentPollFlags();
// Hata/rapor için thread adı ("main", "thread#3 @ a.sqt:12").
std::string currentThreadName();

// ── Kilitler ─────────────────────────────────────────────────────────────
// tag: VM'de lock anındaki try derinliği (catch'e unwind edilirken o try
// içinde alınan kilitler bırakılır); JIT'te 0.
void lockShared(int slot, int tag);
void unlockShared(int slot);
void releaseLocksAboveTag(int tag);   // tag > verilen olanları bırak
void releaseAllLocks();

// Ana thread dışındaki bir thread'de yakalanmayan hata: mesaj + iz yazılır,
// süreç çıkış kodu 1 ile sonlanır (ADR-045 §18; diğer thread'ler beklenmez).
[[noreturn]] void fatalThreadError(const std::string& message, const std::string& trace);

}  // namespace saqut::threading

#endif  // SAQUT_RUNTIME_THREADING_THREAD_RUNTIME
