// ============================================================================
// saQut — Park katmanı + deadlock dedektörü (ADR-045 Faz 2-g / 2-h)
//
// DİZİN:   src/runtime/threading/park.hpp
// KATMAN:  runtime/threading — tüm bloklamaların geçtiği tek nokta
//
// ADR-045 §RUNTIME 5: "Tüm bloklamalar tek bir park/unpark katmanından
// geçer". v1 basit modeli: süreç-global epoch sayacı + tek mutex + tek
// condition_variable_any.
//
//   park(pred, stop, where):  pred() park mutex'i altında değerlendirilir;
//     doğruysa hemen döner. Değilse thread uyur; her shared mutasyonda
//     (notifyShared) uyanır ve pred'i yeniden dener. stop istenirse false.
//     pred yan etkili olabilir (ör. "kuyruktan al"): doğru döndürdüğü an
//     işlem tamamlanmıştır — kayıp uyandırma yoktur çünkü notifyShared
//     epoch'u park mutex'i altında artırır.
//   notifyShared():  her shared mutasyon (atomik yazma, push, pop, append,
//     unlock, thread bitişi) sonrası çağrılır.
//
// TODO(ADR-045 opt): sembol başına bekleyen listeleri — bugün her mutasyon
// tüm bekleyenleri uyandırır (thundering herd); v1 için kabul edildi.
//
// Deadlock dedektörü (2-h): park mutex'i altında `live` (bitmemiş thread) ve
// `parked` (deadlock'a SAYILAN beklemede olanlar: pop/push/wait/join; sleep
// ve IO sayılmaz) tutulur. Bir notifyShared tüm bekleyenleri "uyanık" sayar
// (epoch değişti → pred yeniden denenecek), yani parked sıfırlanır ve her
// bekleyen yeniden kayıt olur. parked == live olduğu an hiçbir thread'in
// ilerleyemeyeceği kesindir: rapor yazılır, süreç hata koduyla sonlanır
// (işleyici değiştirilebilir: DAP altında stopped olayı, Faz 4).
// SAQUT_NO_DEADLOCK_DETECT=1 ortam değişkeni dedektörü kapatır (Plan B kaçış
// kapısı; varsayılan açık).
// ============================================================================

#ifndef SAQUT_RUNTIME_THREADING_PARK
#define SAQUT_RUNTIME_THREADING_PARK

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>

namespace saqut::threading {

// Yaşam döngüsü (ThreadTable çağırır). live sayacı ana thread'i de içerir.
void parkThreadStarted();
void parkThreadFinished();

// Bkz. dosya başı. `where` deadlock raporunda ve DAP'ta görünür
// ("pop jobs", "join thread#3", "wait" ...). countsForDeadlock=false:
// sleep/IO benzeri, dedektörde sayılmaz.
// Dönüş: pred doğru olduysa true; stop istendiyse false.
bool park(const std::function<bool()>& pred, std::stop_token stop,
          const std::string& where, bool countsForDeadlock = true);

// Shared mutasyon bildirimi: epoch++ (park mutex'i altında) + notify_all.
void notifyShared();

// `wait(koşul)` döngüsü (Faz 3): epoch `seen`'den farklı olana dek bekler
// (deadlock'a sayılır). true: epoch değişti; false: stop istendi.
bool parkUntilEpochChanges(uint64_t seen, std::stop_token stop, const std::string& where);

// Test/teşhis: mevcut epoch.
uint64_t sharedEpoch();

// Park etmeden önce bağlı isolate'in heap'inde "yarım eşik" toplaması
// (Heap::collectBeforePark). Backend bağlar; null ise atlanır.
using BeforeParkHook = void (*)();
void setBeforeParkHook(BeforeParkHook hook);

// ── Deadlock (2-h) ──────────────────────────────────────────────────────────
// report: her thread'in adı + beklediği yer. Varsayılan işleyici stderr'e
// yazar ve süreci kSoftwareError (70) ile sonlandırır. İşleyici park mutex'i
// TUTULMADAN çağrılır.
using DeadlockHandler = void (*)(const std::string& report);
void setDeadlockHandler(DeadlockHandler handler);
void setDeadlockDetectionEnabled(bool enabled);
bool deadlockDetectionEnabled();

}  // namespace saqut::threading

#endif  // SAQUT_RUNTIME_THREADING_PARK
