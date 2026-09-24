// ============================================================================
// saQut — ThreadTable / ThreadCore (ADR-045 Faz 2-a)
//
// DİZİN:   src/runtime/threading/thread_table.hpp
// KATMAN:  runtime/threading — süreç-global thread kaydı
//
// Bir saQut `Thread` değeri heap nesnesi DEĞİLDİR: bu tabloya bir tamsayı
// id'dir (ADR-045 Bölüm 2: handle'ın GC finalizer'ı gerekmez). id'ler
// monotondur: main = 1, sonraki her spawn bir artar; kayıtlar süreç boyunca
// silinmez (bitmiş thread'in id'si geçerli kalır, join/running sorgulanabilir).
//
// Her ThreadCore bir OS thread'idir (std::jthread) ve gövdesi kendi Isolate'ini
// kurar. Tablo mutex korumalıdır; ThreadCore adresleri sabittir (unique_ptr).
// ============================================================================

#ifndef SAQUT_RUNTIME_THREADING_THREAD_TABLE
#define SAQUT_RUNTIME_THREADING_THREAD_TABLE

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

struct Isolate;

namespace saqut::threading {

enum class ThreadState : uint8_t { Running, Parked, Finished };

// Yoklama (poll) bayrak kelimesi (Faz 3-g): her döngü geri kenarında okunur.
namespace pollbits {
constexpr uint32_t kStop       = 1u << 0;  // t.stop() istendi
constexpr uint32_t kDebugPause = 1u << 1;  // DAP all-stop (Faz 4)
}  // namespace pollbits

struct ThreadCore {
    int         id = 0;
    std::string name;              // "main" ya da "thread#N @ dosya:satır"
    std::jthread thread;           // main için boş (ana OS thread'i)
    std::stop_source stopSource;   // main için kullanılır; spawn'da jthread'inki
    Isolate*    isolate = nullptr; // gövde bağlar; yalnız duraklatılmışken okunur

    std::atomic<ThreadState> state{ThreadState::Running};
    std::atomic<uint32_t>    pollFlags{0};

    // Park mutex'i altında yazılır/okunur (deadlock raporu, DAP "[bekliyor: ...]").
    std::string waitingOn;
    // Yakalanmayan hata (gövde bitmeden yazar; finished sonrası okunur).
    std::string uncaughtError;

    std::stop_token stopToken() const { return stopSource.get_token(); }
    bool finished() const { return state.load(std::memory_order_acquire) == ThreadState::Finished; }
};

class ThreadTable {
public:
    static ThreadTable& instance();

    // Ana thread'i id 1 / "main" olarak kaydeder (idempotent).
    ThreadCore& registerMain();

    // Yeni OS thread'i başlatır; body yeni thread'de koşar. Gövdenin
    // bitişi (normal, stop, hata) tablo tarafından `finished` işaretlenir ve
    // park katmanına bildirilir. Ad: "thread#N" + (location boş değilse
    // " @ " + location), ör. "thread#3 @ main.sqt:12" (Faz 4 DAP adı).
    ThreadCore& spawn(std::string location, std::function<void(ThreadCore&)> body);

    ThreadCore* find(int id);

    // Çağıran thread'in kaydı (ana thread registerMain sonrası, spawn'lar
    // gövde başında bağlanır). Kayıtsız thread'de nullptr.
    static ThreadCore* current();

    // stop isteği: bloklamaz. Bekleyen (park) thread stop_token ile uyanır;
    // koşan thread bir sonraki yoklama noktasında (geri kenar) çıkar.
    void requestStop(int id);

    // main dönüşünde: main dışındaki tüm thread'leri bitene kadar bekler
    // (bekleme park katmanından geçer → deadlock dedektörü aktif).
    void joinAll();

    // Anlık görüntü (DAP threads isteği, deadlock raporu). Döndürülen
    // işaretçiler süreç boyunca geçerlidir.
    std::vector<ThreadCore*> snapshot();

    ~ThreadTable();

private:
    ThreadTable() = default;
    std::mutex                               mu_;
    std::deque<std::unique_ptr<ThreadCore>>  threads_;
    int                                      nextId_ = 1;
};

}  // namespace saqut::threading

#endif  // SAQUT_RUNTIME_THREADING_THREAD_TABLE
