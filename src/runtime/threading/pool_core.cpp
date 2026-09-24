// ============================================================================
// saQut — PoolCore gerçeklemesi (ADR-045 Faz 2-e). Sözleşme: pool_core.hpp.
// ============================================================================

#include "runtime/threading/pool_core.hpp"

#include "runtime/threading/park.hpp"

namespace saqut::threading {

bool PoolCore::tryPush(MessageBuffer& msg) {
    std::lock_guard<std::mutex> lk(mu_);
    if (max_ > 0 && static_cast<int64_t>(q_.size()) >= max_) return false;
    q_.push_back(std::move(msg));
    return true;
}

bool PoolCore::tryPop(MessageBuffer& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
}

bool PoolCore::push(MessageBuffer msg, std::stop_token stop) {
    // pred park mutex'i altında çalışır; başarılı olduğu an eleman eklenmiştir.
    const bool ok = park([&] { return tryPush(msg); }, stop, "push " + name_);
    if (ok) notifyShared();
    return ok;
}

bool PoolCore::pop(MessageBuffer& out, std::stop_token stop) {
    const bool ok = park([&] { return tryPop(out); }, stop, "pop " + name_);
    if (ok) notifyShared();
    return ok;
}

void PoolCore::setMax(int64_t max) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        max_ = max > 0 ? max : 0;
    }
    notifyShared();   // sınır büyüdüyse bekleyen push'lar ilerleyebilir
}

int64_t PoolCore::max() const {
    std::lock_guard<std::mutex> lk(mu_);
    return max_;
}

int64_t PoolCore::length() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int64_t>(q_.size());
}

}  // namespace saqut::threading
