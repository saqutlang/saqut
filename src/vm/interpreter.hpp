// ============================================================================
// saQut VM — Interpreter (Bytecode Yorumlayıcı)
//
// IRProgram içindeki talimatları çalıştırır.
// "main" fonksiyonundan başlar, RETURN ile biten frame'leri kapatır.
//
// DÖNGÜ GÜVENLİĞİ (referans invalidation):
//   Her iterasyonun başında callStack.back() tazeden alınır.
//   CALL ve RETURN'den sonra `continue` ile döngü başına dönülür;
//   böylece vector büyümesinden kaynaklanan dangling pointer sorunu olmaz.
// ============================================================================

#ifndef SAQUT_VM_INTERPRETER
#define SAQUT_VM_INTERPRETER

#include <atomic>
#include <vector>
#include <optional>
#include <functional>
#include <set>
#include <unordered_map>
#include <utility>
#include "ir/ir_program.hpp"
#include "ir/ir_liveness.hpp"
#include "core/module_registry.hpp"
#include "vm/call_frame.hpp"
#include "gc/gc_object.hpp"
#include "gc/gc_heap.hpp"
#include "ffi/host_bridge.hpp"   // #222: HostCallScratch / HostRetOwner
#include "profiling/stage_timer.hpp"

// Forward-declare: BenchVMTrace tam tanımı bench/profile.hpp'de.
// Yalnızca pointer tutulur — normal modda sıfır bağımlılık.
struct BenchVMTrace;

// ADR-025: try bloğu girişinde yığına eklenen kayıt
struct TryFrame {
    size_t callStackDepth; // ENTER_TRY anındaki callStack_.size() — unwind için
    int    catchTarget;    // catch bloğunun IR instruction indeksi
    int    errorSlot;      // catch değişkeninin slot numarası (catch frame'inde)
};

// VM, GC'ye kendi köklerini bildiren bir RootSource'tur: modül global'leri,
// çağrı yığınındaki canlı slot'lar ve uçuştaki throw değeri. Kaydı kurucuda
// yapılır, yıkıcıda kaldırılır (Heap sağlayıcıyı sahiplenmez).
class Interpreter : public RootSource {
public:
    // ADR-045 (1-d): IRProgram salt okunur paylaşılır. Heap ve globalSlots
    // Interpreter üyesi kalır (thread başına bir Interpreter); kurucu bunları
    // bağlı isolate'e işaretçi olarak bağlar, yıkıcı önceki bağı geri koyar.
    explicit Interpreter(const IRProgram& program);
    ~Interpreter() override;

    // "main" fonksiyonunu bul ve çalıştır.
    // Tamamlandığında main'in dönüş değerini (int) döndürür.
    int run();

    // DAP: VM'i çalıştırmadan ilklendir (callStack, globaller, vmInitialized_)
    void initForDebug();

    // Profil hook — bench komutu tarafından set edilir (nullptr = kapalı).
    // Normal run/check/ir komutlarında çağrılmaz, sıfır maliyet.
    void setVMTrace(BenchVMTrace* t) { vmTrace_ = t; }
    long long heapAllocCount() const { return heap_.stats().liveObjects; }

    // src/profiling/ (--profile): nullptr = kapalı, sıfır maliyet. Set
    // edilirse "vm-warmup" (initForDebug — frame/global kurulumu) ve
    // "vm-exec" (runUntilEvent'in ANA döngüsü, yani VM'in gerçekten
    // instruction çalıştırdığı kısım) ayrı ayrı raporlanır.
    void setStageProfiler(Profiling::StageTimer* p) { stageProfiler_ = p; }

    // Faz 7 (#105): program çıktısı kancası. DAP modunda print çıktısı
    // protokol stdout'unu kirletmesin diye DapHandler output event'ine
    // yönlendirilir. Varsayılan (boş) std::cout — CLI run/exec DEĞİŞMEZ.
    using OutputSink = std::function<void(const std::string&)>;
    void setOutputSink(OutputSink sink) { outputSink_ = std::move(sink); }

    // ── GC (#77, ADR-022) ────────────────────────────────────────────────────
    // Eşik tabanlı tetikleme: canlı nesne sayısı eşiği aşınca instruction
    // sınırında (safepoint) mark-sweep koşar. n <= 0 → otomatik GC kapalı
    // (yalnızca ~Heap temizler — eski arena davranışı).
    // n > 0 → toplama eşiği (canlı ayak izi, bayt); n <= 0 → toplama kapalı.
    void setGCThreshold(int n) {
        gcThreshold_ = n;   // ADR-045: spawn edilen thread'lere aktarılır
        if (n > 0) heap_.setMinCollectBytes(n);
        else       heap_.setCollectionEnabled(false);
    }
    const GcStats& gcStats() const { return heap_.stats(); }

    // #90: `--` sonrası argümanlar — sys::args() ile programa geçirilir.
    void setProgramArgs(std::vector<std::string> a) { programArgs_ = std::move(a); }
    const std::vector<std::string>& programArgs() const { return programArgs_; }

    // ── ADR-045 (Faz 3-e): izole thread'ler ──────────────────────────────────
    // Yeni thread'in Interpreter'ı: main yerine sentetik __thread_* giriş
    // fonksiyonunu koşar; başlangıç mesajı (yakalananlar) bu heap'e açılır ve
    // THREAD_ARG ile okunur. Ana olmayan Interpreter program başı/sonu
    // (SharedSlots kurulumu, joinAll) yapmaz.
    void setThreadEntry(const std::string& function, const std::vector<Value>& args) {
        entryFunction_     = function;
        threadArgs_        = args;
        isMainInterpreter_ = false;
    }
    Heap& heap() { return heap_; }

    // ── DAP API ───────────────────────────────────────────────────────────────
    enum class RunState { Running, Paused, Finished };
    // Faz 5: runUntilEvent dönüş nedeni
    enum class RunReason { StepDone, Breakpoint, Finished, BudgetExhausted, Error };

    void setBreakpoint(const std::string& file, int line);
    void clearBreakpoint(const std::string& file, int line);
    void clearAllBreakpoints();
    // Yalnız bir dosyanın breakpoint'lerini siler: DAP setBreakpoints dosya
    // başına gelir; tümünü silmek diğer dosyalardakileri kaybettiriyordu.
    void clearBreakpointsInFile(const std::string& file);
    // Doğrulanmayan satır için aynı dosyada sonraki çalıştırılabilir satır
    // (en çok maxAhead satır ileride); yoksa 0.
    int  nextExecutableLine(const std::string& file, int line, int maxAhead = 50) const;
    // Duraklama noktasının (dosya, satır) çifti — hitBreakpointIds için.
    std::pair<std::string, int> currentLocation() const;
    // Faz 7 (#105): (dosya, satır) çalıştırılabilir bir satıra denk geliyor mu?
    // setBreakpoints.verified için Faz 5'in lineToFirstIP indeksinde arar.
    bool isExecutableLine(const std::string& file, int line) const;

    RunState    state() const { return state_; }
    void        resume();
    void        stepInstruction();
    void        stepOver();
    // Faz 5: satır bazlı adımlar
    void        stepLine();   // sourceLine değişene kadar ilerle
    void        stepOut();    // callDepth azalana kadar ilerle
    // Satır değişene kadar ilerle, çağrılan fonksiyonlara GİR (DAP stepIn).
    // Eskiden stepIn tek IR komutu çalıştırıyordu; satır değişmediği için
    // kullanıcı ikinci kez basmak zorunda kalıyordu.
    void        stepInto();
    // Görünür ilk satıra kadar (global başlatıcı prelude'u dahil) çalış ve
    // o satırın ilk komutu ÇALIŞMADAN dur (DAP stopOnEntry).
    void        stepToFirstLine();

    // Faz 5: instruction budget ile koş — mevcut durumdan devam eder, başlatma yapmaz.
    RunReason   runUntilEvent(int maxInstructions, int startCallDepth = -1);

    int         currentSourceLine() const;
    std::string currentSourceFile() const;
    int         callDepth() const;
    std::string frameSourceFile(int depth) const;
    int         frameSlotCount(int depth) const;
    std::string frameFunctionName(int depth) const;
    int         frameSourceLine(int depth) const;

    Value       readSlotInFrame(int frameDepth, int slotIndex) const;
    // Faz 5: IRFunction::slotNames kullanarak gerçek değişken adını döndürür
    std::string slotName(int frameDepth, int slotIndex) const;

private:
    const IRProgram&       program_;
    // ADR-045 (Faz 3-e)
    std::string            entryFunction_     = "main";
    std::vector<Value>     threadArgs_;          // GC kökü (collectRoots)
    bool                   isMainInterpreter_ = true;
    bool                   threadingActive_   = false;   // program_.usesThreads
    std::atomic<uint32_t>* pollFlags_         = nullptr; // geri kenar yoklaması
    int                    gcThreshold_       = 0;
    void pollBackEdge();
    void executeThreadOp(const Instruction& instr, CallFrame& frame);
    // Kurucuda bağlı isolate'in önceki heap/globalSlots bağı (yıkıcı geri koyar).
    Heap*                  prevIsolateHeap_    = nullptr;
    std::vector<Value>*    prevIsolateGlobals_ = nullptr;
    std::vector<CallFrame> callStack_;
    // #3 (2026-07-16): tek DÜZ global slot dizisi — LOAD_GLOBAL/STORE_GLOBAL
    // yürütülen fonksiyonun DEĞİL, IRGenerator'ın tüm programa yaydığı flat
    // indekse göre çalışıyor (nameToGlobal_ ADR-034 import-gated stdlib'den
    // önce de tek program-çapında sayaçtı). Önceki "moduleId → vector" haritası
    // çapraz-modül global okuma/yazmada frame.function->moduleId'yi kullanıyordu
    // — bildiren fonksiyonun DEĞİL çağıran fonksiyonun modülüne göre yanlış
    // diziye erişiyordu (export edilmiş global başka modülden okunduğunda
    // sessizce 0 dönüyordu).
    std::vector<Value>     globalSlots_;
    Heap                   heap_;
    // #206: struct alan adları tip başına bir kez, paylaşımlı metadata
    std::unordered_map<std::string, std::shared_ptr<std::vector<std::string>>>
                           structFieldNamesRegistry_;
    std::vector<TryFrame>  tryStack_;
    std::optional<Value>   pendingThrow_;
    BenchVMTrace*          vmTrace_ = nullptr;  // profil hook (bench modunda non-null)
    Profiling::StageTimer* stageProfiler_ = nullptr;  // --profile hook
    OutputSink             outputSink_;         // Faz 7 (#105): boş = std::cout

    // DAP durumu
    RunState state_ = RunState::Running;
    std::set<std::pair<std::string,int>> breakpoints_;  // {file, line}

    // Faz 5: debug koşu kontrolü
    bool vmInitialized_  = false;   // run() başlatmayı bir kez yapar
    int  runBudget_      = 0;       // kalan talimat bütçesi (0 = sınırsız, run() tarafından kullanılmaz)
    int  stepStartDepth_ = -1;      // stepOver/Out için başlangıç derinliği
    int  stepStartLine_  = 0;       // stepOver/Line için başlangıç satırı
    int  forcedStartLine_ = 0;      // !=0 ise runUntilEvent stepStartLine_'ı bundan alır
    int  lastReturnValue_ = 0;      // main'in dönüş değeri (pause/resume sonrası için)

    bool isBreakpoint() const;
    void checkBreakpoint();

    // GC safepoint'i: eşik aşıldıysa toplama koşar. YALNIZCA instruction
    // sınırında çağrılmalıdır — opcode ortasında henüz hiçbir slot'a
    // bağlanmamış nesne kök sayılmaz ve süpürülürdü.
    void maybeCollect() { heap_.collectIfNeeded(); }

public:
    // RootSource: GC'ye bu VM'in canlı referanslarını bildirir.
    void collectRoots(RootSink& sink) override;

private:

    // Kök daraltma: frame'in SADECE o anki talimat noktasında canlı olan
    // slot'larını kök sayar (ir_liveness). ENTER_TRY içeren fonksiyonlar
    // muhafazakârdır (exact=false) — tüm slot'lar kök kalır. Sonuç fonksiyon
    // başına bir kez hesaplanıp önbelleklenir; talimat listesi koşu sırasında
    // değişmez.
    const SlotLiveness& livenessFor(const IRFunction* fn);
    std::unordered_map<const IRFunction*, SlotLiveness> livenessCache_;


    std::vector<std::string> programArgs_; // #90: `--` sonrası argümanlar

    // #222: host çağrı ABI'si — çağrılar arasında YENİDEN KULLANILIR.
    // Çağrı başına heap tahsisi yapmamanın yolu budur: scratch argüman
    // dönüşümünün, owner dönüş değerinin ömrünü taşır; ikisi de her
    // CALLHOST'ta reset edilir, yeniden tahsis edilmez.
    HostCallScratch hostScratch_;
    HostRetOwner    hostRetOwner_;
    // Frame de yeniden kullanılır: içinde HostError'ın iki std::string'i var
    // ve her CALLHOST'ta yeniden kurmak sıcak yolda ölçülebilir maliyetti.
    HostCallFrame   hostFrame_;

    // Faz 5: bütçe/step kısıtlarını kontrol eder, true = durmalı
    bool shouldStop();

    // Error StructObject oluştur (ADR-025): [line, col, message, trace, code]
    Value makeErrorValue(const std::string& message,
                         const std::string& code = "",
                         int line = 0, int col = 0);

    // #229: legacy executeHostFunction silindi — tüm host çağrıları
    // CALLHOST'un tek index-tabanlı yolundan rt_host_call'a gider.

    // Mevcut callStack_'i gezerek stacktrace string'i üretir.
    // pendingThrow_ set edilmeden ÖNCE çağrılmalı (unwind olmadan).
    std::string buildTrace() const;
};

#endif // SAQUT_VM_INTERPRETER
