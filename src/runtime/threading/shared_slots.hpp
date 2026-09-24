// ============================================================================
// saQut — SharedSlots: `shared` globallerin süreç-global tablosu
// (ADR-045 §SHARED 6-9, Faz 2-b)
//
// DİZİN:   src/runtime/threading/shared_slots.hpp
// KATMAN:  runtime/threading — heap dışında yaşayan paylaşımlı veri
//
// Her `shared` global bir slottur; indeksi DERLEME zamanında atanır (IR
// SHARED_* opcode'ları slot indeksini taşır). Slot türleri:
//   Int   → std::atomic<int64_t>     Float → std::atomic<double>
//   Bool  → std::atomic<bool>        Pool  → PoolCore    List → ListCore
// Primitif slotların `lock` deyimi için kendi std::mutex'i vardır.
//
// Kurulum (configure) program başında, ANA thread'de, hiçbir thread spawn
// edilmeden yapılır; sonrasında tablonun şekli değişmez (yalnız değerler).
// Her mutasyon (store, RMW, unlock) park katmanına notifyShared bildirir;
// Pool/List kendi mutasyonlarında bildirir.
// ============================================================================

#ifndef SAQUT_RUNTIME_THREADING_SHARED_SLOTS
#define SAQUT_RUNTIME_THREADING_SHARED_SLOTS

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/threading/list_core.hpp"
#include "runtime/threading/pool_core.hpp"

namespace saqut::threading {

enum class SharedKind : uint8_t { Int, Float, Bool, Pool, List };

struct SharedSlotDesc {
    SharedKind  kind;
    std::string name;
};

struct SharedSlot {
    SharedKind  kind = SharedKind::Int;
    std::string name;

    std::atomic<int64_t> i{0};
    std::atomic<double>  d{0.0};
    std::atomic<bool>    b{false};
    std::unique_ptr<PoolCore> pool;
    std::unique_ptr<ListCore> list;

    std::mutex lockMu;   // `lock name;` — yalnız primitif slotlarda kullanılır
};

class SharedSlots {
public:
    static SharedSlots& instance();

    // Tabloyu kurar (ana thread, spawn öncesi). Önceki içerik atılır.
    void configure(const std::vector<SharedSlotDesc>& slots);

    int size() const { return static_cast<int>(slots_.size()); }
    SharedSlot& at(int index) { return *slots_[static_cast<size_t>(index)]; }

    // ── Atomik primitif erişim (her yazma notifyShared yapar) ──────────────
    int64_t loadInt(int index);
    void    storeInt(int index, int64_t v);
    int64_t addInt(int index, int64_t delta);      // yeni değeri döndürür (+=, -=)
    double  loadFloat(int index);
    void    storeFloat(int index, double v);
    double  addFloat(int index, double delta);
    bool    loadBool(int index);
    void    storeBool(int index, bool v);

    // ── lock / unlock ───────────────────────────────────────────────────────
    // Çoklu kilit çağıranın sıraladığı indekslerle tek tek alınır (IR slot
    // indeksine göre sıralı üretir → sabit küresel sıra, kilit-kilit deadlock
    // olmaz). unlock notifyShared yapar.
    void lock(int index);
    void unlock(int index);

    PoolCore& pool(int index) { return *at(index).pool; }
    ListCore& list(int index) { return *at(index).list; }

private:
    SharedSlots() = default;
    std::vector<std::unique_ptr<SharedSlot>> slots_;
};

}  // namespace saqut::threading

#endif  // SAQUT_RUNTIME_THREADING_SHARED_SLOTS
