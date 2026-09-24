// ============================================================================
// saQut — PoolCore: sınırlı, tipli FIFO kuyruk (ADR-045 §SHARED 7, Faz 2-e)
//
// DİZİN:   src/runtime/threading/pool_core.hpp
// KATMAN:  runtime/threading — heap dışında yaşayan paylaşımlı kuyruk
//
// Elemanlar serileştirilmiş mesajlardır (MessageBuffer): heap pointer'ı
// içermezler. push deep copy ile girer; pop alıcının heap'inde yeni nesne
// kurar (deserialize çağıranın işi). max (setMax): 0 = sınırsız.
//
// Bloklama park katmanından geçer (ADR-045 §RUNTIME 5 — PoolCore'un kendi
// notEmpty/notFull CV'leri YOKTUR, bkz. karar günlüğü [2-e]): push dolu
// kuyrukta, pop boş kuyrukta park eder; ikisi de stop_token ile uyanır ve
// false döner (iptal noktası). Her başarılı push/pop/setMax notifyShared yapar.
// ============================================================================

#ifndef SAQUT_RUNTIME_THREADING_POOL_CORE
#define SAQUT_RUNTIME_THREADING_POOL_CORE

#include <cstdint>
#include <deque>
#include <mutex>
#include <stop_token>
#include <string>

#include "runtime/threading/message.hpp"

namespace saqut::threading {

class PoolCore {
public:
    explicit PoolCore(std::string name = {}) : name_(std::move(name)) {}

    // Dolu kuyrukta bloklar. true: eklendi; false: stop istendi (eklenmedi).
    bool push(MessageBuffer msg, std::stop_token stop);

    // Boş kuyrukta bloklar. true: out dolduruldu; false: stop istendi.
    bool pop(MessageBuffer& out, std::stop_token stop);

    // Bloklamayan denemeler (birim testleri, ileride tryPop).
    bool tryPush(MessageBuffer& msg);
    bool tryPop(MessageBuffer& out);

    void    setMax(int64_t max);   // <= 0 → sınırsız
    int64_t max() const;
    int64_t length() const;

    const std::string& name() const { return name_; }

private:
    mutable std::mutex         mu_;
    std::deque<MessageBuffer>  q_;
    int64_t                    max_ = 0;
    std::string                name_;
};

}  // namespace saqut::threading

#endif  // SAQUT_RUNTIME_THREADING_POOL_CORE
