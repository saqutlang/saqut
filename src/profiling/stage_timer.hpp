// ============================================================================
// saQut Profiling — StageTimer (Aşama Başına Süre Ölçümü)
// ============================================================================
//
// DİZİN:   src/Profiling/stage_timer.hpp
// KATMAN:  Çapraz kesit — CLI'nin (`--profile`) pipeline aşamalarını
//          (token/parser/ir-gen/vm-veya-jit) ölçmesi için kullandığı
//          minimal araç.
//
// AMAÇ:
//   Bu dizin ileride büyümeye aday (opcode-seviyesi profil, bellek
//   izleme, JIT derleme-süresi kırılımı vb. — bkz. mevcut `src/bench/`
//   ile karışmasın: bench.hpp N-tekrarlı istatistiksel ölçüm + opcode
//   trace analiz eder, StageTimer TEK bir koşuda "hangi aşama ne kadar
//   sürdü" sorusuna cevap veren, `saqut run --profile` için düz bir
//   araçtır). Bugün yalnızca ana pipeline aşamalarını ölçüyor.
//
// KULLANIM:
//   Profiling::StageTimer timer;
//   {
//       Profiling::StageTimer::ScopedStage _(&timer, "token");
//       // ... tokenize ...
//   }
//   timer.printReport(std::cerr);
//
//   `timer` işaretçisi nullptr ise ScopedStage HİÇBİR ŞEY yapmaz (ölçüm
//   istenmediğinde saat çağrısı maliyeti bile yok) — çağıran taraf
//   `args.profile ? &timer : nullptr` deseniyle koşulsuz enjekte edebilir.
// ============================================================================

#ifndef SAQUT_PROFILING_STAGE_TIMER
#define SAQUT_PROFILING_STAGE_TIMER

#include <chrono>
#include <iomanip>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Profiling {

class StageTimer {
public:
    using Clock = std::chrono::steady_clock;

    // Bir aşamanın süresi + (opsiyonel) "iş miktarı" (kaç token/node/geçiş/
    // instruction işlendi). Süre ScopedStage'den, adet count()'tan gelir;
    // ikisi bağımsız — bir aşama yalnızca süre taşıyabilir (warmup/exec).
    struct StageInfo {
        long long   microseconds = 0;
        long long   count        = 0;
        std::string unit;                 // ASCII (hizalama için — bkz. count())
        bool        hasCount     = false;
    };

    // Aynı isimle birden çok kez çağrılabilir (ör. çok-modüllü derlemede
    // her dosya için ayrı ayrı tokenize/parse) — süreler isim başına toplanır.
    void add(const std::string& stage, long long microseconds) {
        info(stage).microseconds += microseconds;
    }

    // Aşamanın iş miktarını bildirir (byproduct — ölçüm için ekstra tarama
    // YAPMA, çağıran taraf zaten elindeki sayıyı geçsin). Tekrar çağrılırsa
    // adet TOPLANIR (çok-modüllü derlemede her dosyanın token sayısı gibi).
    // unit ASCII tutulmalı: rapor std::setw ile hizalanıyor ve setw bayt
    // sayar — çok-baytlı UTF-8 karakter (ör. "geçiş") hizayı bozar, "gecis".
    void count(const std::string& stage, long long n, const std::string& unit) {
        StageInfo& si = info(stage);
        si.count += n;
        si.unit     = unit;
        si.hasCount = true;
    }

    long long microsecondsFor(const std::string& stage) const {
        auto it = totals_.find(stage);
        return it == totals_.end() ? 0 : it->second.microseconds;
    }

    void printReport(std::ostream& out) const {
        out << "[profile] stage timings:\n";
        long long total = 0;
        for (const auto& name : order_) {
            const StageInfo& si = totals_.at(name);
            total += si.microseconds;
            out << "  " << std::left << std::setw(14) << name;
            if (si.hasCount) {
                out << std::right << std::setw(12) << groupThousands(si.count)
                    << " " << std::left << std::setw(7) << si.unit;
            } else {
                out << std::setw(20) << "";  // count(12)+bosluk(1)+unit(7)
            }
            out << std::right << std::setw(10) << (si.microseconds / 1000.0)
                << " ms\n";
        }
        out << "  " << std::left << std::setw(14) << "total"
            << std::setw(20) << ""
            << std::right << std::setw(10) << (total / 1000.0) << " ms\n";
    }

    // RAII: yapıcıda saat başlar, yıkıcıda timer->add(stage, gecen_sure)
    // çağrılır. timer == nullptr ise tamamen no-op (saat bile okunmaz).
    class ScopedStage {
    public:
        ScopedStage(StageTimer* timer, std::string stage)
            : timer_(timer), stage_(std::move(stage)),
              start_(timer_ ? Clock::now() : Clock::time_point{}) {}

        ScopedStage(const ScopedStage&)            = delete;
        ScopedStage& operator=(const ScopedStage&) = delete;

        ~ScopedStage() {
            if (!timer_) return;
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                           Clock::now() - start_)
                           .count();
            timer_->add(stage_, us);
        }

    private:
        StageTimer*        timer_;
        std::string         stage_;
        Clock::time_point   start_;
    };

private:
    // Aşama kaydını bulur/oluşturur; ilk görülüşte rapor sırasına ekler.
    StageInfo& info(const std::string& stage) {
        auto it = totals_.find(stage);
        if (it != totals_.end()) return it->second;
        order_.push_back(stage);
        return totals_.emplace(stage, StageInfo{}).first->second;
    }

    // Binlik ayraçlı biçim ("400000" → "400.000") — yalnızca rapor için.
    static std::string groupThousands(long long v) {
        std::string s = std::to_string(v);
        int ins = static_cast<int>(s.size()) - 3;
        while (ins > 0) { s.insert(static_cast<size_t>(ins), "."); ins -= 3; }
        return s;
    }

    std::vector<std::string>                   order_;   // ilk görülme sırası (rapor sırası)
    std::unordered_map<std::string, StageInfo> totals_;
};

}  // namespace Profiling

#endif  // SAQUT_PROFILING_STAGE_TIMER
