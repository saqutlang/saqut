// ============================================================================
// saQut — SharedSlots gerçeklemesi (ADR-045 Faz 2-b). Sözleşme: shared_slots.hpp.
// ============================================================================

#include "runtime/threading/shared_slots.hpp"

#include "runtime/threading/park.hpp"

namespace saqut::threading {

SharedSlots& SharedSlots::instance() {
    static SharedSlots slots;
    return slots;
}

void SharedSlots::configure(const std::vector<SharedSlotDesc>& descs) {
    slots_.clear();
    slots_.reserve(descs.size());
    for (const auto& d : descs) {
        auto s  = std::make_unique<SharedSlot>();
        s->kind = d.kind;
        s->name = d.name;
        if (d.kind == SharedKind::Pool) s->pool = std::make_unique<PoolCore>(d.name);
        if (d.kind == SharedKind::List) s->list = std::make_unique<ListCore>(d.name);
        slots_.push_back(std::move(s));
    }
}

int64_t SharedSlots::loadInt(int index) {
    return at(index).i.load(std::memory_order_seq_cst);
}

void SharedSlots::storeInt(int index, int64_t v) {
    at(index).i.store(v, std::memory_order_seq_cst);
    notifyShared();
}

int64_t SharedSlots::addInt(int index, int64_t delta) {
    const int64_t prev = at(index).i.fetch_add(delta, std::memory_order_seq_cst);
    notifyShared();
    return prev + delta;
}

double SharedSlots::loadFloat(int index) {
    return at(index).d.load(std::memory_order_seq_cst);
}

void SharedSlots::storeFloat(int index, double v) {
    at(index).d.store(v, std::memory_order_seq_cst);
    notifyShared();
}

double SharedSlots::addFloat(int index, double delta) {
    const double prev = at(index).d.fetch_add(delta, std::memory_order_seq_cst);
    notifyShared();
    return prev + delta;
}

double SharedSlots::addFloat32(int index, double delta) {
    auto&  a    = at(index).d;
    double prev = a.load(std::memory_order_seq_cst);
    double next = 0.0;
    do {
        next = static_cast<double>(static_cast<float>(prev + delta));
    } while (!a.compare_exchange_weak(prev, next, std::memory_order_seq_cst));
    notifyShared();
    return next;
}

bool SharedSlots::loadBool(int index) {
    return at(index).b.load(std::memory_order_seq_cst);
}

void SharedSlots::storeBool(int index, bool v) {
    at(index).b.store(v, std::memory_order_seq_cst);
    notifyShared();
}

void SharedSlots::lock(int index) { at(index).lockMu.lock(); }

void SharedSlots::unlock(int index) {
    at(index).lockMu.unlock();
    notifyShared();
}

}  // namespace saqut::threading
