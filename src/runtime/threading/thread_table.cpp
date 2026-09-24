// ============================================================================
// saQut — ThreadTable gerçeklemesi (ADR-045 Faz 2-a). Sözleşme: thread_table.hpp.
// ============================================================================

#include "runtime/threading/thread_table.hpp"

#include "runtime/threading/park.hpp"

namespace saqut::threading {

namespace {
thread_local ThreadCore* t_threadCore = nullptr;
}

ThreadTable& ThreadTable::instance() {
    static ThreadTable table;
    return table;
}

ThreadCore* ThreadTable::current() { return t_threadCore; }

ThreadCore& ThreadTable::registerMain() {
    ThreadCore* core = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!threads_.empty() && threads_.front()->id == 1) {
            t_threadCore = threads_.front().get();
            return *t_threadCore;
        }
        auto c  = std::make_unique<ThreadCore>();
        c->id   = nextId_++;
        c->name = "main";
        core    = c.get();
        threads_.push_back(std::move(c));
    }
    t_threadCore = core;
    parkThreadStarted();
    return *core;
}

ThreadCore& ThreadTable::spawn(std::string location, std::function<void(ThreadCore&)> body) {
    ThreadCore* core = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto c  = std::make_unique<ThreadCore>();
        c->id   = nextId_++;
        c->name = "thread#" + std::to_string(c->id) +
                  (location.empty() ? std::string() : " @ " + location);
        core    = c.get();
        threads_.push_back(std::move(c));
        if (recordEvents_.load(std::memory_order_relaxed))
            events_.push_back({core->id, true});
    }
    // live sayacı thread başlamadan artar: ebeveyn hemen ardından park etse
    // bile dedektör çocuğu canlı sayar.
    parkThreadStarted();
    core->thread = std::jthread([this, core, body = std::move(body)]() {
        t_threadCore = core;
        body(*core);
        core->state.store(ThreadState::Finished, std::memory_order_release);
        if (recordEvents_.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lk(mu_);
            events_.push_back({core->id, false});
        }
        parkThreadFinished();
    });
    return *core;
}

void ThreadTable::setRecordLifecycleEvents(bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    recordEvents_.store(on, std::memory_order_relaxed);
    if (!on) events_.clear();
}

std::vector<ThreadTable::LifecycleEvent> ThreadTable::drainLifecycleEvents() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<LifecycleEvent> out(events_.begin(), events_.end());
    events_.clear();
    return out;
}

ThreadCore* ThreadTable::find(int id) {
    std::lock_guard<std::mutex> lk(mu_);
    // id'ler 1'den başlayıp ardışık olduğundan indeks = id - 1.
    if (id < 1 || id > static_cast<int>(threads_.size())) return nullptr;
    return threads_[static_cast<size_t>(id - 1)].get();
}

void ThreadTable::requestStop(int id) {
    ThreadCore* core = find(id);
    if (!core) return;
    core->pollFlags.fetch_or(pollbits::kStop, std::memory_order_release);
    // Park'ta bekleyen thread condition_variable_any'nin stop geri çağrısıyla
    // uyanır; koşan thread geri kenar yoklamasında bayrağı görür.
    core->stopSource.request_stop();
}

std::vector<ThreadCore*> ThreadTable::snapshot() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ThreadCore*> out;
    out.reserve(threads_.size());
    for (auto& t : threads_) out.push_back(t.get());
    return out;
}

void ThreadTable::joinAll() {
    ThreadCore* self = current();
    auto allOthersFinished = [this, self] {
        for (ThreadCore* t : snapshot())
            if (t != self && t->id != 1 && !t->finished()) return false;
        return true;
    };
    // main'in kendi stop'u yok (boş token): yalnız bitiş ya da deadlock.
    park(allOthersFinished, std::stop_token{}, "join (main exit)");
    for (ThreadCore* t : snapshot())
        if (t != self && t->thread.joinable()) t->thread.join();
}

ThreadTable::~ThreadTable() {
    // Statik yıkım: normal yolda joinAll zaten her şeyi bitirdi. Bitmemiş
    // thread kalmışsa (anormal çıkış) join asılı kalırdı → ayır.
    for (auto& t : threads_) {
        if (!t->thread.joinable()) continue;
        if (t->finished()) t->thread.join();
        else               t->thread.detach();
    }
}

}  // namespace saqut::threading
