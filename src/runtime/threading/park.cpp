// ============================================================================
// saQut — Park katmanı + deadlock dedektörü gerçeklemesi (ADR-045 Faz 2-g/2-h)
// Sözleşme: park.hpp.
// ============================================================================

#include "runtime/threading/park.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>

#include "cli/exit_codes.hpp"
#include "runtime/output_lock.hpp"
#include "runtime/threading/thread_table.hpp"

namespace saqut::threading {

namespace {

struct ParkState {
    std::mutex                  mu;
    std::condition_variable_any cv;
    uint64_t                    epoch  = 0;
    int                         live   = 0;  // bitmemiş kayıtlı thread'ler
    int                         parked = 0;  // bu epoch'ta deadlock'a sayılan bekleyenler
};

ParkState& state() {
    static ParkState s;
    return s;
}

bool envDetectionEnabled() {
    const char* v = std::getenv("SAQUT_NO_DEADLOCK_DETECT");
    return !(v && v[0] != '\0' && v[0] != '0');
}

std::atomic<BeforeParkHook>  g_beforePark{nullptr};
std::atomic<DeadlockHandler> g_deadlockHandler{nullptr};
std::atomic<bool>            g_detectionEnabled{envDetectionEnabled()};

// Park mutex'i TUTULURKEN çağrılır (waitingOn park mutex'i altında yazılır).
std::string buildDeadlockReport() {
    std::ostringstream os;
    os << "runtime error: all threads are blocked (deadlock)\n";
    for (ThreadCore* t : ThreadTable::instance().snapshot()) {
        if (t->finished()) continue;
        os << "  " << t->name << " (id " << t->id << "): ";
        if (t->waitingOn.empty()) os << "running";
        else                      os << "waiting on " << t->waitingOn;
        os << "\n";
    }
    return os.str();
}

void defaultDeadlockHandler(const std::string& report) {
    std::cout.flush();
    writeProgramOutput(std::cerr, report);
    // Diğer thread'ler sonsuza dek bloklu: statik yıkıcılar (ThreadTable'ın
    // jthread join'i) asılı kalırdı → _Exit.
    std::_Exit(saqut::exit_code::kSoftwareError);
}

}  // namespace

void parkThreadStarted() {
    auto& s = state();
    std::lock_guard<std::mutex> lk(s.mu);
    ++s.live;
}

void parkThreadFinished() {
    auto& s = state();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        --s.live;
        ++s.epoch;       // join bekleyenleri uyandır; herkes yeniden kayıt olur
        s.parked = 0;
    }
    s.cv.notify_all();
}

void notifyShared() {
    auto& s = state();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        ++s.epoch;
        s.parked = 0;    // tüm bekleyenler pred'i yeniden deneyecek
    }
    s.cv.notify_all();
}

uint64_t sharedEpoch() {
    auto& s = state();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.epoch;
}

void setBeforeParkHook(BeforeParkHook hook) { g_beforePark.store(hook); }
void setDeadlockHandler(DeadlockHandler handler) { g_deadlockHandler.store(handler); }
void setDeadlockDetectionEnabled(bool enabled) { g_detectionEnabled.store(enabled); }
bool deadlockDetectionEnabled() { return g_detectionEnabled.load(); }

bool park(const std::function<bool()>& pred, std::stop_token stop,
          const std::string& where, bool countsForDeadlock) {
    auto& s = state();
    ThreadCore* self = ThreadTable::current();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (pred()) return true;
    }
    // Uyumadan önce: yarım eşik GC (safepoint — çağıran opcode sınırında).
    if (BeforeParkHook hook = g_beforePark.load()) hook();

    std::unique_lock<std::mutex> lk(s.mu);
    if (self) {
        self->waitingOn = where;
        self->state.store(ThreadState::Parked, std::memory_order_release);
    }
    bool satisfied = false;
    for (;;) {
        if (pred()) { satisfied = true; break; }
        if (stop.stop_requested()) break;

        const uint64_t myEpoch = s.epoch;
        if (countsForDeadlock) {
            ++s.parked;
            if (g_detectionEnabled.load() && s.live > 0 && s.parked >= s.live) {
                std::string report = buildDeadlockReport();
                DeadlockHandler handler = g_deadlockHandler.load();
                lk.unlock();
                (handler ? handler : defaultDeadlockHandler)(report);
                lk.lock();
                // İşleyici döndüyse (DAP): uyanana dek beklemeye devam.
            }
        }
        s.cv.wait(lk, stop, [&] { return s.epoch != myEpoch; });
        // epoch değişmediyse (stop/sahte uyanma) kaydımızı biz geri alırız;
        // değiştiyse notifyShared parked'ı zaten sıfırladı.
        if (countsForDeadlock && s.epoch == myEpoch) --s.parked;
    }
    if (self) {
        self->waitingOn.clear();
        self->state.store(ThreadState::Running, std::memory_order_release);
    }
    return satisfied;
}

}  // namespace saqut::threading
