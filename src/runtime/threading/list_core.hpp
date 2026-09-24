// ============================================================================
// saQut — ListCore: append-only paylaşımlı liste (ADR-045 §SHARED 8, Faz 2-f)
//
// DİZİN:   src/runtime/threading/list_core.hpp
// KATMAN:  runtime/threading — heap dışında yaşayan paylaşımlı liste
//
// Elemanlar sabit boyutlu chunk'larda durur ve ASLA yer değiştirmez:
//   dizin (kChunkCount atomik chunk işaretçisi, bir kez ayrılır) →
//   chunk (kChunkSize mesaj).
// append mutex altında yazar, elemanı yerleştirdikten SONRA uzunluğu
// release ile yayınlar. get(i) uzunluğu acquire ile okur ve KİLİTSİZDİR;
// yayınlanmış bir eleman bir daha yazılmaz. i >= length → hata (çağıran
// runtime hatasına çevirir). Kapasite: kChunkSize × kChunkCount eleman.
// ============================================================================

#ifndef SAQUT_RUNTIME_THREADING_LIST_CORE
#define SAQUT_RUNTIME_THREADING_LIST_CORE

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "runtime/threading/message.hpp"

namespace saqut::threading {

class ListCore {
public:
    static constexpr size_t kChunkBits  = 10;                     // 1024 eleman/chunk
    static constexpr size_t kChunkSize  = size_t{1} << kChunkBits;
    static constexpr size_t kChunkCount = size_t{1} << 14;        // 16384 chunk
    static constexpr int64_t kCapacity  = int64_t(kChunkSize * kChunkCount);  // ~16.7M

    explicit ListCore(std::string name = {});
    ~ListCore();
    ListCore(const ListCore&)            = delete;
    ListCore& operator=(const ListCore&) = delete;

    // true: eklendi; false: kapasite doldu (çağıran runtime hatasına çevirir).
    bool append(MessageBuffer msg);

    // Kilitsiz okuma. Aralık dışında nullptr. Dönen işaretçi liste ömrü
    // boyunca geçerlidir ve işaret ettiği mesaj değişmez.
    const MessageBuffer* get(int64_t index) const;

    int64_t length() const { return length_.load(std::memory_order_acquire); }
    const std::string& name() const { return name_; }

private:
    struct Chunk {
        MessageBuffer items[kChunkSize];
    };

    std::mutex                               appendMu_;
    std::atomic<int64_t>                     length_{0};
    std::unique_ptr<std::atomic<Chunk*>[]>   dir_;
    std::string                              name_;
};

}  // namespace saqut::threading

#endif  // SAQUT_RUNTIME_THREADING_LIST_CORE
