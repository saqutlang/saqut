// ============================================================================
// saQut — ListCore gerçeklemesi (ADR-045 Faz 2-f). Sözleşme: list_core.hpp.
// ============================================================================

#include "runtime/threading/list_core.hpp"

#include "runtime/threading/park.hpp"

namespace saqut::threading {

ListCore::ListCore(std::string name)
    : dir_(new std::atomic<Chunk*>[kChunkCount]), name_(std::move(name)) {
    for (size_t i = 0; i < kChunkCount; ++i) dir_[i].store(nullptr, std::memory_order_relaxed);
}

ListCore::~ListCore() {
    for (size_t i = 0; i < kChunkCount; ++i) delete dir_[i].load(std::memory_order_relaxed);
}

bool ListCore::append(MessageBuffer msg) {
    {
        std::lock_guard<std::mutex> lk(appendMu_);
        const int64_t n = length_.load(std::memory_order_relaxed);
        if (n >= kCapacity) return false;
        const size_t c = static_cast<size_t>(n) >> kChunkBits;
        const size_t o = static_cast<size_t>(n) & (kChunkSize - 1);
        Chunk* chunk = dir_[c].load(std::memory_order_relaxed);
        if (!chunk) {
            chunk = new Chunk();
            dir_[c].store(chunk, std::memory_order_release);
        }
        chunk->items[o] = std::move(msg);
        // Eleman tamamen yazıldıktan SONRA yayınla.
        length_.store(n + 1, std::memory_order_release);
    }
    notifyShared();   // wait(log.length() > k) gibi koşullar
    return true;
}

const MessageBuffer* ListCore::get(int64_t index) const {
    if (index < 0 || index >= length_.load(std::memory_order_acquire)) return nullptr;
    const size_t c = static_cast<size_t>(index) >> kChunkBits;
    const size_t o = static_cast<size_t>(index) & (kChunkSize - 1);
    const Chunk* chunk = dir_[c].load(std::memory_order_acquire);
    return &chunk->items[o];
}

}  // namespace saqut::threading
