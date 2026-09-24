// ============================================================================
// saQut — Thread çalışma zamanı ortak katmanı gerçeklemesi (ADR-045 Faz 3).
// Sözleşme: thread_runtime.hpp.
// ============================================================================

#include "runtime/threading/thread_runtime.hpp"

#include <cstdlib>
#include <iostream>

#include "gc/gc_heap.hpp"
#include "runtime/isolate.hpp"
#include "runtime/output_lock.hpp"
#include "runtime/threading/park.hpp"
#include "runtime/threading/shared_slots.hpp"
#include "runtime/threading/thread_table.hpp"

namespace saqut::threading {

namespace {

// Park öncesi GC (2-g): bağlı isolate'in heap'i — VM'de Interpreter bağlar
// (Isolate::heap), JIT'te koşu heap'i (JitRuntime::heap).
void beforeParkCollect() {
    Isolate* iso = t_isolate;
    if (!iso) return;
    Heap* heap = iso->heap ? iso->heap : iso->jit.heap;
    if (heap) heap->collectBeforePark();
}

}  // namespace

void programBegin(const std::vector<std::pair<int, std::string>>& slots) {
    ThreadTable::instance().registerMain();
    std::vector<SharedSlotDesc> descs;
    descs.reserve(slots.size());
    for (const auto& [kind, name] : slots)
        descs.push_back({static_cast<SharedKind>(kind), name});
    SharedSlots::instance().configure(descs);
    setBeforeParkHook(&beforeParkCollect);
}

void programEnd() { ThreadTable::instance().joinAll(); }

std::stop_token currentStopToken() {
    ThreadCore* core = ThreadTable::current();
    return core ? core->stopToken() : std::stop_token{};
}

std::atomic<uint32_t>* currentPollFlags() {
    ThreadCore* core = ThreadTable::current();
    return core ? &core->pollFlags : nullptr;
}

std::string currentThreadName() {
    ThreadCore* core = ThreadTable::current();
    return core ? core->name : std::string("main");
}

void lockShared(int slot, int tag) {
    SharedSlots::instance().lock(slot);
    if (Isolate* iso = t_isolate) iso->heldLocks.emplace_back(slot, tag);
}

void unlockShared(int slot) {
    Isolate* iso = t_isolate;
    if (!iso) return;
    auto& held = iso->heldLocks;
    for (auto it = held.rbegin(); it != held.rend(); ++it) {
        if (it->first != slot) continue;
        held.erase(std::next(it).base());
        SharedSlots::instance().unlock(slot);
        return;
    }
    // Tutulmayan kilit (açık unlock sonrası blok sonu): etkisiz.
}

void releaseLocksAboveTag(int tag) {
    Isolate* iso = t_isolate;
    if (!iso) return;
    auto& held = iso->heldLocks;
    while (!held.empty() && held.back().second > tag) {
        const int slot = held.back().first;
        held.pop_back();
        SharedSlots::instance().unlock(slot);
    }
}

void releaseAllLocks() { releaseLocksAboveTag(-1); }

void fatalThreadError(const std::string& message, const std::string& trace) {
    std::string text = "runtime error in " + currentThreadName() + ": " + message + "\n";
    if (!trace.empty()) text += trace + (trace.back() == '\n' ? "" : "\n");
    std::cout.flush();
    writeProgramOutput(std::cerr, text);
    std::_Exit(1);
}

}  // namespace saqut::threading
