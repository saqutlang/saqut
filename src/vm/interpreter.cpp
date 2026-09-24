// ============================================================================
// saQut VM — Interpreter (Bytecode Yorumlayıcı) Gerçeklemesi
// ============================================================================
//
// DİZİN:   src/vm/interpreter.cpp
// KATMAN:  VM — IRProgram içindeki instruction'ları yorumlar
//
// AMAÇ:
//   Tüm Opcode'ların işlenmesi (~1303 satır). DAP breakpoint/adım API'leri,
//   built-in metod dispatch, hata yönetimi (TRY/THROW), GC tetikleme.
//
// ============================================================================

#include "core/config.hpp"
#include <limits>
#include "vm/interpreter.hpp"
#include "core/int_arithmetic.hpp"
#include "gc/gc_object.hpp"
#include "data/data_registry.hpp"
#include "bench/profile.hpp"
#include "ffi/host_functions.hpp"
#include "ffi/host_registry.hpp"
#include "gc/shadow_stack.hpp"
#include "runtime/isolate.hpp"
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdint>

// int32/int64 aritmetiği tek kaynaktan gelir: core/int_arithmetic.hpp.
// Taşma davranışı (tanımlı 2's-complement wrap, ADR-040) dilin gözlemlenebilir
// sözleşmesidir; VM ve sabit katlama aynı fonksiyonları çağırmak zorundadır —
// ayrı kopyalar sessizce ayrışır. Gerekçelerin tamamı o başlıktadır.
using namespace saqut::intmath;

// ADR-045 (1-d): heap/globalSlots Interpreter üyesidir (thread başına bir
// Interpreter); bağlı isolate onlara işaretçi tutar ki runtime primitifleri
// (Faz 2 mesaj deserialize, park öncesi GC) thread'in heap'ini bulabilsin.
// Guard'sız araçlarda (birim testleri) isolate bağlı olmayabilir.
Interpreter::Interpreter(const IRProgram& program) : program_(program) {
    heap_.addRootSource(this);
    if (Isolate* iso = t_isolate) {
        prevIsolateHeap_    = iso->heap;
        prevIsolateGlobals_ = iso->globalSlots;
        iso->heap           = &heap_;
        iso->globalSlots    = &globalSlots_;
    }
}

Interpreter::~Interpreter() {
    if (Isolate* iso = t_isolate; iso && iso->heap == &heap_) {
        iso->heap        = prevIsolateHeap_;
        iso->globalSlots = prevIsolateGlobals_;
    }
    heap_.removeRootSource(this);
}

// ── buildTrace ─────────────────────────────────────────────────────────────────
// Mevcut callStack_'i en içten dışa gezerek stacktrace string'i üretir.
// pendingThrow_ set edilmeden (unwind olmadan) önce çağrılmalıdır.
std::string Interpreter::buildTrace() const {
    std::string result;
    for (int i = (int)callStack_.size() - 1; i >= 0; --i) {
        const CallFrame& f = callStack_[i];
        if (!f.function) continue;
        // ip zaten artırılmış olduğundan şu an çalışan instruction = ip - 1
        int ip = f.instructionPointer - 1;
        if (ip >= 0 && ip < (int)f.function->instructions.size()) {
            const Instruction& ins = f.function->instructions[ip];
            if (ins.sourceLine > 0) {
                const std::string& file =
                    program_.moduleRegistry.filePath(f.function->moduleId);
                result += f.function->name + " (" + file + ":" +
                          std::to_string(ins.sourceLine) + ":" +
                          std::to_string(ins.sourceCol)  + ")\n";
            } else {
                result += f.function->name + " (?)\n";
            }
        } else {
            result += f.function->name + " (?)\n";
        }
    }
    return result;
}

// ── makeErrorValue ─────────────────────────────────────────────────────────────
// ADR-025: Error struct oluşturur — alan sırası: [line, col, message, trace, code]
Value Interpreter::makeErrorValue(const std::string& message,
                                   const std::string& code,
                                   int line, int col) {
    StructObject* obj = heap_.allocStruct(5);
    obj->fieldNames = errorStructFieldNames();
    obj->fields[0] = Value::fromInt(line);
    obj->fields[1] = Value::fromInt(col);
    obj->fields[2] = Value::fromString(message);
    obj->fields[3] = Value::fromString(buildTrace());
    obj->fields[4] = Value::fromString(code);
    return Value::fromRef(obj);
}

// ── DAP API implementasyonu ────────────────────────────────────────────────────

void Interpreter::setBreakpoint(const std::string& file, int line) {
    breakpoints_.insert({file, line});
}

void Interpreter::clearBreakpoint(const std::string& file, int line) {
    breakpoints_.erase({file, line});
}

void Interpreter::clearAllBreakpoints() {
    breakpoints_.clear();
}

// Faz 7 (#105): (dosya, satır) çalıştırılabilir mi? — setBreakpoints.verified.
// Yorum/boş satıra konan breakpoint'e verified:false dönmek için Faz 5'in
// lineToFirstIP indeksinde arar; dosya eşleşmesi ModuleRegistry üzerinden.
bool Interpreter::isExecutableLine(const std::string& file, int line) const {
    for (const auto& [name, fn] : program_.functions) {
        if (fn.lineToFirstIP.count(line) == 0) continue;
        if (program_.moduleRegistry.filePath(fn.moduleId) == file) return true;
    }
    return false;
}

bool Interpreter::isBreakpoint() const {
    // Sıcak yol: breakpoint yoksa hiçbir iş yapma. Aşağıdaki arama
    // `breakpoints_.count({file, line})` ile geçici bir std::pair kurar ve
    // dosya yolu string'ini KOPYALAR — küme boş olsa bile. Bu fonksiyon her
    // talimatta çağrıldığı için ölçümde VM'in sıcak döngüsündeki tahsislerin
    // ~%92'si buradan geliyordu (boş `while` döngüsünde bile iterasyon başına
    // ~5 tahsis). Debugger bağlı değilken breakpoints_ her zaman boştur.
    if (breakpoints_.empty()) return false;
    if (callStack_.empty()) return false;
    const CallFrame& frame = callStack_.back();
    if (!frame.function) return false;
    int ip = frame.instructionPointer;
    if (ip < 0 || ip >= (int)frame.function->instructions.size()) return false;
    const Instruction& ins = frame.function->instructions[ip];
    if (ins.sourceLine <= 0 || ins.debugHidden) return false;
    // Satıra girişte tetiklen: aynı satırın sonraki komutlarında (ve çağrıdan
    // aynı satıra dönüşte) değil.
    if (frame.lastLine == ins.sourceLine) return false;
    const std::string& file = ins.sourceFile.empty()
        ? program_.moduleRegistry.filePath(frame.function->moduleId)
        : ins.sourceFile;
    return breakpoints_.count({file, ins.sourceLine}) > 0;
}

void Interpreter::checkBreakpoint() {
    if (state_ == RunState::Paused) return;
    if (isBreakpoint())
        state_ = RunState::Paused;
}

void Interpreter::resume() {
    // Faz 5: run() değil, mevcut durumdan devam (bütçe -1 = sınırsız)
    runUntilEvent(-1, -1);
}

void Interpreter::stepInstruction() {
    if (callStack_.empty()) { state_ = RunState::Finished; return; }
    // Faz 5: tek talimat çalıştır
    runUntilEvent(1, -1);
}

void Interpreter::stepInto() {
    if (callStack_.empty()) { state_ = RunState::Finished; return; }
    runUntilEvent(-1, std::numeric_limits<int>::max());
}

void Interpreter::stepToFirstLine() {
    if (callStack_.empty()) { state_ = RunState::Finished; return; }
    forcedStartLine_ = -1;   // hiçbir gerçek satıra eşit değil: ilk görünür satırda dur
    runUntilEvent(-1, std::numeric_limits<int>::max());
}

void Interpreter::clearBreakpointsInFile(const std::string& file) {
    for (auto it = breakpoints_.begin(); it != breakpoints_.end();)
        it = (it->first == file) ? breakpoints_.erase(it) : std::next(it);
}

int Interpreter::nextExecutableLine(const std::string& file, int line, int maxAhead) const {
    for (int l = line; l <= line + maxAhead; ++l)
        if (isExecutableLine(file, l)) return l;
    return 0;
}

std::pair<std::string, int> Interpreter::currentLocation() const {
    if (callStack_.empty()) return {"", 0};
    const CallFrame& f = callStack_.back();
    if (!f.function || f.instructionPointer < 0 ||
        f.instructionPointer >= (int)f.function->instructions.size())
        return {"", currentSourceLine()};
    const Instruction& ins = f.function->instructions[f.instructionPointer];
    std::string file = ins.sourceFile.empty()
        ? program_.moduleRegistry.filePath(f.function->moduleId) : ins.sourceFile;
    return {file, ins.sourceLine};
}

void Interpreter::stepOver() {
    if (callStack_.empty()) { state_ = RunState::Finished; return; }
    // Faz 5: aynı çağrı derinliğinde satır değişene kadar ilerle
    int depth = (int)callStack_.size();
    runUntilEvent(-1, depth);
}

// Faz 5: sourceLine değişene kadar ilerle
void Interpreter::stepLine() {
    if (callStack_.empty()) { state_ = RunState::Finished; return; }
    int depth = (int)callStack_.size();
    runUntilEvent(-1, depth); // stepOver ile aynı mantık
}

// Faz 5: callDepth azalana kadar ilerle
void Interpreter::stepOut() {
    if (callStack_.empty()) { state_ = RunState::Finished; return; }
    int depth = (int)callStack_.size() - 1; // şu anki fonksiyondan çıkış
    runUntilEvent(-1, depth);
}

int Interpreter::currentSourceLine() const {
    if (callStack_.empty()) return 0;
    const CallFrame& f = callStack_.back();
    int ip = f.instructionPointer;
    // Faz 9 (#105): duraklama semantiği — SIRADAKİ (henüz çalışmamış)
    // instruction'ın satırı raporlanır; adım kontrolü fetch ÖNCESİNE
    // alındığından durulan nokta ip'nin kendisidir. Fonksiyon sonundaysa
    // son çalışan instruction'ın satırına düşülür.
    if (f.function && ip >= 0 && ip < (int)f.function->instructions.size())
        return f.function->instructions[ip].sourceLine;
    if (f.function && ip > 0 && ip - 1 < (int)f.function->instructions.size())
        return f.function->instructions[ip - 1].sourceLine;
    return 0;
}

std::string Interpreter::currentSourceFile() const {
    if (callStack_.empty()) return "";
    const CallFrame& f = callStack_.back();
    int ip = f.instructionPointer;
    if (f.function && ip > 0 && ip - 1 < (int)f.function->instructions.size()) {
        const Instruction& ins = f.function->instructions[ip - 1];
        return ins.sourceFile.empty()
            ? program_.moduleRegistry.filePath(f.function->moduleId)
            : ins.sourceFile;
    }
    return "";
}

int Interpreter::callDepth() const {
    return (int)callStack_.size();
}

std::string Interpreter::frameSourceFile(int depth) const {
    if (depth < 0 || depth >= (int)callStack_.size()) return "";
    const CallFrame& f = callStack_[(int)callStack_.size() - 1 - depth];
    int ip = f.instructionPointer - 1;
    if (f.function && ip >= 0 && ip < (int)f.function->instructions.size()) {
        const Instruction& ins = f.function->instructions[ip];
        return ins.sourceFile.empty()
            ? program_.moduleRegistry.filePath(f.function->moduleId)
            : ins.sourceFile;
    }
    return program_.moduleRegistry.filePath(f.function ? f.function->moduleId : 0);
}

int Interpreter::frameSlotCount(int depth) const {
    if (depth < 0 || depth >= (int)callStack_.size()) return 0;
    const CallFrame& f = callStack_[(int)callStack_.size() - 1 - depth];
    return f.function ? f.function->slotCount : 0;
}

std::string Interpreter::frameFunctionName(int depth) const {
    if (depth < 0 || depth >= (int)callStack_.size()) return "";
    const CallFrame& f = callStack_[(int)callStack_.size() - 1 - depth];
    return f.function ? f.function->name : "";
}

int Interpreter::frameSourceLine(int depth) const {
    if (depth < 0 || depth >= (int)callStack_.size()) return 0;
    const CallFrame& f = callStack_[(int)callStack_.size() - 1 - depth];
    // Faz 9 (#105): aktif frame'de (depth 0) durulan nokta = SIRADAKİ
    // instruction; üst frame'lerde ip dönüş adresidir — çağrıyı yapan
    // satır (ip-1'deki CALL) raporlanır.
    int ip = (depth == 0) ? f.instructionPointer : f.instructionPointer - 1;
    if (f.function && ip >= 0 && ip < (int)f.function->instructions.size())
        return f.function->instructions[ip].sourceLine;
    if (f.function && ip - 1 >= 0 && ip - 1 < (int)f.function->instructions.size())
        return f.function->instructions[ip - 1].sourceLine;
    return 0;
}

Value Interpreter::readSlotInFrame(int frameDepth, int slotIndex) const {
    if (frameDepth < 0 || frameDepth >= (int)callStack_.size())
        return Value::fromInt(0);
    const CallFrame& f = callStack_[(int)callStack_.size() - 1 - frameDepth];
    if (slotIndex < 0 || slotIndex >= (int)f.slots.size())
        return Value::fromInt(0);
    return f.slots[slotIndex];
}

// Faz 5: IRFunction::slotNames kullanarak gerçek değişken adını döndürür.
std::string Interpreter::slotName(int frameDepth, int slotIndex) const {
    if (frameDepth < 0 || frameDepth >= (int)callStack_.size()) return "";
    const CallFrame& f = callStack_[(int)callStack_.size() - 1 - frameDepth];
    if (!f.function) return "";
    if (slotIndex < 0 || slotIndex >= (int)f.function->slotNames.size()) return "";
    return f.function->slotNames[slotIndex];
}

// Faz 5: bütçe/step kısıtı kontrolü — döngü başında çağrılır.
bool Interpreter::shouldStop() {
    if (state_ == RunState::Paused) return true;
    if (runBudget_ <= 0 && runBudget_ != 0) return true; // bütçe tükendi (0 = sınırsız değil)
    // stepOver/stepLine: satır değişti mi?
    if (stepStartDepth_ >= 0 && stepStartLine_ > 0) {
        int curLine = currentSourceLine();
        int curDepth = (int)callStack_.size();
        if (curLine > 0 && curLine != stepStartLine_ && curDepth <= stepStartDepth_) {
            state_ = RunState::Paused;
            return true;
        }
    }
    // stepOut: derinlik azaldı mı?
    if (stepStartDepth_ >= 0 && stepStartLine_ == 0) {
        if ((int)callStack_.size() <= stepStartDepth_) {
            state_ = RunState::Paused;
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Interpreter::collectRoots — VM'in GC'ye bildirdiği kökler
// ─────────────────────────────────────────────────────────────────────────────
//
// Üç kaynak: modül global'leri, çağrı yığınındaki frame slot'ları ve
// uçuştaki throw değeri. (JIT'in shadow stack'i AYRI bir kök sağlayıcıdır —
// bu VM'in işi değildir.)
//
// KÖK DARALTMA: bir frame'in tüm slot'ları değil, yalnızca o anki talimat
// noktasında CANLI olanları bildirilir. Canlı = değeri ileride en az bir kez
// daha okunacak. Son okumasından sonra slot ölüdür; içindeki nesne başka bir
// yoldan erişilemiyorsa toplanabilir. Analiz src/ir/ir_liveness.cpp'dedir.
//
// GÜVENLİ TARAF: ENTER_TRY içeren fonksiyonlarda analiz muhafazakârdır
// (exact=false) ve tüm slot'lar bildirilir — try bölgesinden catch'e
// atlandığında hangi slot'un okunacağı henüz bilinmiyor. Fazla bildirim
// yalnızca gecikmiş toplamadır; eksik bildirim canlı nesnenin süpürülmesi.

const SlotLiveness& Interpreter::livenessFor(const IRFunction* fn) {
    auto it = livenessCache_.find(fn);
    if (it == livenessCache_.end())
        it = livenessCache_.emplace(fn, computeSlotLiveness(*fn)).first;
    return it->second;
}

void Interpreter::collectRoots(RootSink& sink) {
    for (const Value& global : globalSlots_) sink.acceptValue(global);

    for (CallFrame& frame : callStack_) {
        const SlotLiveness& liveness = livenessFor(frame.function);

        if (!liveness.exact) {
            for (const Value& slot : frame.slots) sink.acceptValue(slot);
            continue;
        }

        for (int slotIndex = 0; slotIndex < (int)frame.slots.size(); ++slotIndex) {
            Value& slot = frame.slots[(size_t)slotIndex];
            if (liveness.isLiveBefore(frame.instructionPointer, slotIndex)) {
                sink.acceptValue(slot);
            } else if (slot.kind == ValueKind::Ref ||
                       slot.kind == ValueKind::String) {
                // Ölü slot temizlenir: içindeki nesne bu turda toplanabilir
                // ve slot'ta serbest bırakılmış adres kalmamalıdır — frame'i
                // sonradan okuyan araçlar (DAP değişken görünümü) sarkmış
                // işaretçi görmesin.
                slot = Value::null();
            }
        }
    }

    if (pendingThrow_) sink.acceptValue(*pendingThrow_);
}

Interpreter::RunReason Interpreter::runUntilEvent(int maxInstructions,
                                                    int startCallDepth) {
    if (callStack_.empty() || !vmInitialized_) {
        state_ = RunState::Finished;
        return RunReason::Finished;
    }
    state_          = RunState::Running;
    runBudget_      = maxInstructions;
    stepStartDepth_ = startCallDepth;
    stepStartLine_  = forcedStartLine_ != 0 ? forcedStartLine_
                    : (startCallDepth >= 0) ? currentSourceLine() : 0;
    forcedStartLine_ = 0;

    // Faz 7 (#105): breakpoint ÜSTÜNDE dururken devam edilirse aynı satıra
    // yeniden takılma — bir kaynak satırı birden çok instruction ürettiğinden
    // "üzerinde durduğumuz satır"dan çıkana kadar bp kontrolü atlanır.
    // TODO(faz8): satır içi çağrıdan aynı satıra dönüşte bp yeniden vurur
    // (GDB "her varışta bir kez" semantiği için hit-noktası takibi gerekir).
    int         resumeSkipLine = 0;
    std::string resumeSkipFile;
    if (isBreakpoint() && !callStack_.empty()) {
        const CallFrame& f = callStack_.back();
        const Instruction& ins = f.function->instructions[f.instructionPointer];
        resumeSkipLine = ins.sourceLine;
        resumeSkipFile = ins.sourceFile.empty()
            ? program_.moduleRegistry.filePath(f.function->moduleId)
            : ins.sourceFile;
    }

    // run() ile aynı döngü — ortak kod yolu
    // src/profiling/ (--profile): "vm-exec" TAM OLARAK bu döngünün süresi —
    // VM'in gerçekten instruction çalıştırdığı kısım (kapanış: while'ın
    // kendi kapanış parantezinden hemen sonra).
    { Profiling::StageTimer::ScopedStage _profExec(stageProfiler_, "vm-exec");
    while (!callStack_.empty()) {
        // Bütçe kontrolü: < 0 = sınırsız, == 0 = tükendi, > 0 = kalan hak
        // runUntilEvent(-1, ...) → sınırsız
        // runUntilEvent(1, ...)  → 1 instruction, sonra dur
        if (runBudget_ < 0) {
            // Sınırsız bütçe — devam
        } else if (runBudget_ == 0) {
            state_ = RunState::Paused;
            return RunReason::BudgetExhausted;
        }

        // Breakpoint kontrolü (resume satırı atlanır, bkz. yukarı)
        if (isBreakpoint()) {
            bool onResumeLine = false;
            if (resumeSkipLine > 0) {
                int curLine = 0;
                std::string curFile;
                const CallFrame& f = callStack_.back();
                if (f.function &&
                    f.instructionPointer < (int)f.function->instructions.size()) {
                    const Instruction& ins =
                        f.function->instructions[f.instructionPointer];
                    curLine = ins.sourceLine;
                    curFile = ins.sourceFile.empty()
                        ? program_.moduleRegistry.filePath(f.function->moduleId)
                        : ins.sourceFile;
                }
                onResumeLine = (curLine == resumeSkipLine &&
                                curFile == resumeSkipFile);
            }
            if (!onResumeLine) {
                state_ = RunState::Paused;
                return RunReason::Breakpoint;
            }
        } else {
            // Resume satırından çıkıldı — bundan sonra normal kontrol
            resumeSkipLine = 0;
        }

        // GC safepoint (#77): instruction sınırı — tüm canlı nesneler bu
        // noktada bir slot'a (frame/modül) ya da pendingThrow_'a bağlıdır.
        maybeCollect();

        CallFrame& frame = callStack_.back();

        if (frame.instructionPointer >= (int)frame.function->instructions.size()) {
            int destSlot = frame.returnDestSlot;
            callStack_.pop_back();
            if (!callStack_.empty() && destSlot != -1)
                callStack_.back().slots[destSlot] = Value::fromInt(0);
            // stepOut: derinlik azaldı → tamam
            if (stepStartDepth_ >= 0 && stepStartLine_ == 0 &&
                (int)callStack_.size() <= stepStartDepth_) {
                state_ = RunState::Paused;
                return RunReason::StepDone;
            }
            if (callStack_.empty()) {
                state_ = RunState::Finished;
                return RunReason::Finished;
            }
            continue;
        }

        // Adım kontrolü (stepOver/stepLine): SIRADAKİ instruction yeni bir
        // satıra aitse o instruction ÇALIŞMADAN dur. Faz 9 (#105) düzeltmesi:
        // eski konum (fetch SONRASI) satır sınırındaki ilk instruction'ı
        // yutuyordu — print gibi tek-instruction'lık satırlar adımlamada
        // hiç çalışmıyordu.
        if (stepStartLine_ != 0 && stepStartDepth_ >= 0) {
            const Instruction& nextIns = frame.function->instructions[frame.instructionPointer];
            // Gizli komutlar (global başlatıcı prelude'u) satır sınırı sayılmaz.
            int nextLine = nextIns.debugHidden ? 0 : nextIns.sourceLine;
            int curDepth = (int)callStack_.size();
            if (nextLine > 0 && nextLine != stepStartLine_ && curDepth <= stepStartDepth_) {
                state_ = RunState::Paused;
                return RunReason::StepDone;
            }
        }

        const Instruction& instr = frame.function->instructions[frame.instructionPointer];
        frame.instructionPointer++;
        if (!breakpoints_.empty() && !instr.debugHidden && instr.sourceLine > 0) [[unlikely]]
            frame.lastLine = instr.sourceLine;
        if (runBudget_ > 0) runBudget_--;

        // Profil hook
        if (vmTrace_) [[unlikely]]
            vmTrace_->pushDispatch(static_cast<uint8_t>(instr.opcode));

        switch (instr.opcode) {

        case Opcode::LOAD_CONST:
            frame.slots[instr.dest] = Value::fromInt(instr.intValue);
            break;

        case Opcode::LOAD_STRING:
            frame.slots[instr.dest] = Value::fromString(instr.stringValue);
            break;

        case Opcode::LOAD_NULL:
            frame.slots[instr.dest] = Value::null();
            break;

        case Opcode::LOAD_SLOT:
            frame.slots[instr.dest] = frame.slots[instr.src];
            break;

        // ── Aritmetik ─────────────────────────────────────────────────────
        // TypeChecker derleme zamanında tipleri doğruladı — burada sadece hesap yapılır.
        // İstisna: sıfıra bölme gerçek bir çalışma zamanı koşuludur, kontrol edilir.
        case Opcode::ADD:
            frame.slots[instr.dest] = Value::fromInt(
                wrapAddI32(frame.slots[instr.left].intValue(), frame.slots[instr.right].intValue()));
            break;
        case Opcode::SUB:
            frame.slots[instr.dest] = Value::fromInt(
                wrapSubI32(frame.slots[instr.left].intValue(), frame.slots[instr.right].intValue()));
            break;
        case Opcode::MUL:
            frame.slots[instr.dest] = Value::fromInt(
                wrapMulI32(frame.slots[instr.left].intValue(), frame.slots[instr.right].intValue()));
            break;
        case Opcode::DIV: {
            int d = frame.slots[instr.right].intValue();
            if (d == 0) { pendingThrow_ = makeErrorValue("division by zero", "E_DIVZERO", instr.sourceLine, instr.sourceCol); break; }
            frame.slots[instr.dest] = Value::fromInt(wrapDivI32(frame.slots[instr.left].intValue(), d));
            break;
        }
        case Opcode::MOD: {
            int d = frame.slots[instr.right].intValue();
            if (d == 0) { pendingThrow_ = makeErrorValue("modulo by zero", "E_DIVZERO", instr.sourceLine, instr.sourceCol); break; }
            frame.slots[instr.dest] = Value::fromInt(wrapModI32(frame.slots[instr.left].intValue(), d));
            break;
        }
        // #237: ** — negatif üs tamsayıda kesirli sonuç verirdi, hata.
        case Opcode::POW: {
            int e = frame.slots[instr.right].intValue();
            if (e < 0) { pendingThrow_ = makeErrorValue("negative exponent is undefined for integers", "E_POWNEG", instr.sourceLine, instr.sourceCol); break; }
            frame.slots[instr.dest] = Value::fromInt(
                wrapPowI32(frame.slots[instr.left].intValue(), e));
            break;
        }
        case Opcode::LPOW: {
            long long e = frame.slots[instr.right].int64Value();
            if (e < 0) { pendingThrow_ = makeErrorValue("negative exponent is undefined for integers", "E_POWNEG", instr.sourceLine, instr.sourceCol); break; }
            frame.slots[instr.dest] = Value::fromLongInt(
                wrapPowI64(frame.slots[instr.left].int64Value(), e));
            break;
        }

        // ── Bitsel ────────────────────────────────────────────────────────
        case Opcode::BAND:
            frame.slots[instr.dest] = Value::fromInt(
                frame.slots[instr.left].intValue() & frame.slots[instr.right].intValue());
            break;
        case Opcode::BOR:
            frame.slots[instr.dest] = Value::fromInt(
                frame.slots[instr.left].intValue() | frame.slots[instr.right].intValue());
            break;
        case Opcode::BXOR:
            frame.slots[instr.dest] = Value::fromInt(
                frame.slots[instr.left].intValue() ^ frame.slots[instr.right].intValue());
            break;
        case Opcode::SHL:
            frame.slots[instr.dest] = Value::fromInt(
                wrapShlI32(frame.slots[instr.left].intValue(), frame.slots[instr.right].intValue()));
            break;
        case Opcode::SHR:
            frame.slots[instr.dest] = Value::fromInt(
                wrapShrI32(frame.slots[instr.left].intValue(), frame.slots[instr.right].intValue()));
            break;
        case Opcode::BNOT:
            frame.slots[instr.dest] = Value::fromInt(~frame.slots[instr.src].intValue());
            break;

        // ── Global değişken erişimi ────────────────────────────────────────
        // #3: instr.intValue IRGenerator'ın program-çapında (modüller arası)
        // tek flat indeksi — yürüten fonksiyonun moduleId'siyle KARIŞTIRILMAZ,
        // aksi halde başka modülden import edilmiş bir global yanlış (ya da
        // sınır dışı) diziye erişirdi.
        case Opcode::LOAD_GLOBAL:
            frame.slots[instr.dest] = globalSlots_[instr.intValue];
            break;
        case Opcode::STORE_GLOBAL:
            globalSlots_[instr.intValue] = frame.slots[instr.src];
            break;

        // ── Karşılaştırma ─────────────────────────────────────────────────
        case Opcode::LESS: {
            auto& lv = frame.slots[instr.left]; auto& rv = frame.slots[instr.right];
            int r;
            if (lv.kind == ValueKind::Date && rv.kind == ValueKind::Date)
                r = (lv.int64Value() < rv.int64Value() ? 1 : 0);
            else if (lv.kind == ValueKind::Decimal || rv.kind == ValueKind::Decimal)
                r = DecimalValue::compare(lv.decimalValue(), rv.decimalValue()) < 0 ? 1 : 0;
            else if (lv.isFloaty() || rv.isFloaty())
                r = (lv.asDouble() < rv.asDouble() ? 1 : 0);
            else r = (lv.asI64() < rv.asI64() ? 1 : 0);
            frame.slots[instr.dest] = Value::fromInt(r);
            break;
        }
        case Opcode::LESS_EQUAL: {
            auto& lv = frame.slots[instr.left]; auto& rv = frame.slots[instr.right];
            int r;
            if (lv.kind == ValueKind::Date && rv.kind == ValueKind::Date)
                r = (lv.int64Value() <= rv.int64Value() ? 1 : 0);
            else if (lv.kind == ValueKind::Decimal || rv.kind == ValueKind::Decimal)
                r = DecimalValue::compare(lv.decimalValue(), rv.decimalValue()) <= 0 ? 1 : 0;
            else if (lv.isFloaty() || rv.isFloaty())
                r = (lv.asDouble() <= rv.asDouble() ? 1 : 0);
            else r = (lv.asI64() <= rv.asI64() ? 1 : 0);
            frame.slots[instr.dest] = Value::fromInt(r);
            break;
        }
        case Opcode::GREATER: {
            auto& lv = frame.slots[instr.left]; auto& rv = frame.slots[instr.right];
            int r;
            if (lv.kind == ValueKind::Date && rv.kind == ValueKind::Date)
                r = (lv.int64Value() > rv.int64Value() ? 1 : 0);
            else if (lv.kind == ValueKind::Decimal || rv.kind == ValueKind::Decimal)
                r = DecimalValue::compare(lv.decimalValue(), rv.decimalValue()) > 0 ? 1 : 0;
            else if (lv.isFloaty() || rv.isFloaty())
                r = (lv.asDouble() > rv.asDouble() ? 1 : 0);
            else r = (lv.asI64() > rv.asI64() ? 1 : 0);
            frame.slots[instr.dest] = Value::fromInt(r);
            break;
        }
        case Opcode::GREATER_EQUAL: {
            auto& lv = frame.slots[instr.left]; auto& rv = frame.slots[instr.right];
            int r;
            if (lv.kind == ValueKind::Date && rv.kind == ValueKind::Date)
                r = (lv.int64Value() >= rv.int64Value() ? 1 : 0);
            else if (lv.kind == ValueKind::Decimal || rv.kind == ValueKind::Decimal)
                r = DecimalValue::compare(lv.decimalValue(), rv.decimalValue()) >= 0 ? 1 : 0;
            else if (lv.isFloaty() || rv.isFloaty())
                r = (lv.asDouble() >= rv.asDouble() ? 1 : 0);
            else r = (lv.asI64() >= rv.asI64() ? 1 : 0);
            frame.slots[instr.dest] = Value::fromInt(r);
            break;
        }
        case Opcode::EQUAL_EQUAL: {
            auto& lv = frame.slots[instr.left]; auto& rv = frame.slots[instr.right];
            int r;
            // ADR-021/027: null kind ayrı işlenir — null yalnızca null'a eşittir
            if (lv.kind == ValueKind::Null && rv.kind == ValueKind::Null)
                r = 1;
            else if (lv.kind == ValueKind::Null || rv.kind == ValueKind::Null)
                r = 0;
            else if (lv.kind == ValueKind::Ref || rv.kind == ValueKind::Ref)
                r = (lv.ref() == rv.ref() ? 1 : 0); // ADR-023: array/struct kimlik
            else if (lv.kind == ValueKind::Date && rv.kind == ValueKind::Date)
                r = (lv.int64Value() == rv.int64Value() ? 1 : 0);
            else if (lv.kind == ValueKind::String)
                r = (lv.stringValue() == rv.stringValue() ? 1 : 0);
            else if (lv.kind == ValueKind::Decimal || rv.kind == ValueKind::Decimal)
                r = (lv.decimalValue() == rv.decimalValue() ? 1 : 0);
            else if (lv.isFloaty() || rv.isFloaty())
                r = (lv.asDouble() == rv.asDouble() ? 1 : 0);
            else
                r = (lv.asI64() == rv.asI64() ? 1 : 0);
            frame.slots[instr.dest] = Value::fromInt(r);
            break;
        }
        case Opcode::NOT_EQUAL: {
            auto& lv = frame.slots[instr.left]; auto& rv = frame.slots[instr.right];
            int r;
            if (lv.kind == ValueKind::Null && rv.kind == ValueKind::Null)
                r = 0;
            else if (lv.kind == ValueKind::Null || rv.kind == ValueKind::Null)
                r = 1;
            else if (lv.kind == ValueKind::Ref || rv.kind == ValueKind::Ref)
                r = (lv.ref() != rv.ref() ? 1 : 0);
            else if (lv.kind == ValueKind::Date && rv.kind == ValueKind::Date)
                r = (lv.int64Value() != rv.int64Value() ? 1 : 0);
            else if (lv.kind == ValueKind::String)
                r = (lv.stringValue() != rv.stringValue() ? 1 : 0);
            else if (lv.kind == ValueKind::Decimal || rv.kind == ValueKind::Decimal)
                r = (lv.decimalValue() != rv.decimalValue() ? 1 : 0);
            else if (lv.isFloaty() || rv.isFloaty())
                r = (lv.asDouble() != rv.asDouble() ? 1 : 0);
            else
                r = (lv.asI64() != rv.asI64() ? 1 : 0);
            frame.slots[instr.dest] = Value::fromInt(r);
            break;
        }

        // ── Kontrol akışı ─────────────────────────────────────────────────
        case Opcode::JMP:
            frame.instructionPointer = instr.jumpTarget;
            break;
        case Opcode::JIF_FALSE:
            if (!frame.slots[instr.cond].isTruthy())
                frame.instructionPointer = instr.jumpTarget;
            break;
        case Opcode::JIF_TRUE:
            if (frame.slots[instr.cond].isTruthy())
                frame.instructionPointer = instr.jumpTarget;
            break;

        // ── Fonksiyon çağrısı ─────────────────────────────────────────────
        case Opcode::CALL: {
            if (vmTrace_) [[unlikely]] ++vmTrace_->vmSaqutCalls;
            const IRFunction* callee = program_.findFunction(instr.functionName);
            if (!callee)
                throw std::runtime_error(
                    "'" + instr.functionName + "' function not found");
            // #254: derinlik sınırı — yakalanabilir hata, çağrı yapılmaz.
            if ((int)callStack_.size() >= gMaxCallDepth) [[unlikely]] {
                pendingThrow_ = makeErrorValue(
                    "stack overflow: maximum call depth (" + std::to_string(gMaxCallDepth) +
                        ") exceeded",
                    "E_STACK_OVERFLOW", instr.sourceLine, instr.sourceCol);
                break;
            }

            CallFrame newFrame;
            newFrame.function           = callee;
            newFrame.instructionPointer = 0;
            newFrame.slots.resize(callee->slotCount, Value::fromInt(0));
            newFrame.returnDestSlot     = instr.dest;

            for (int i = 0; i < (int)instr.argSlots.size(); i++)
                newFrame.slots[i] = frame.slots[instr.argSlots[i]];

            callStack_.push_back(std::move(newFrame));
            continue;
        }

        // ── Dönüş ─────────────────────────────────────────────────────────
        //
        // GC (#77): eski "her RETURN'de koşulsuz collect" kaldırıldı — hem
        // her dönüşte tüm modül slotlarını geçici vektöre kopyalıyordu hem de
        // döngü İÇİNDE tahsis yapan program hiç toplanmıyordu (#67 zayıflığı).
        // Toplama artık döngü başındaki eşik tabanlı maybeCollect() safepoint'i
        // (dönüş değeri o noktada caller slot'una yazılmış olur — kök kararlı).
        case Opcode::RETURN: {
            Value returnValue    = frame.slots[instr.src];
            int   returnDestSlot = frame.returnDestSlot;
            callStack_.pop_back();

            if (!callStack_.empty() && returnDestSlot != -1)
                callStack_.back().slots[returnDestSlot] = returnValue;

            if (callStack_.empty()) {
                lastReturnValue_ = returnValue.intValue();
                state_ = RunState::Finished;
                return RunReason::Finished;
            }

            // stepOut: derinlik azaldı → tamam
            if (stepStartDepth_ >= 0 && stepStartLine_ == 0 &&
                (int)callStack_.size() <= stepStartDepth_) {
                state_ = RunState::Paused;
                return RunReason::StepDone;
            }
            continue;
        }

        // ── Float aritmetik (#44) ─────────────────────────────────────────
        case Opcode::LOAD_FLOAT:
            frame.slots[instr.dest] = Value::fromFloat(instr.floatValue);
            break;
        case Opcode::FADD:
            frame.slots[instr.dest] = Value::fromFloat(
                frame.slots[instr.left].floatValue() + frame.slots[instr.right].floatValue());
            break;
        case Opcode::FSUB:
            frame.slots[instr.dest] = Value::fromFloat(
                frame.slots[instr.left].floatValue() - frame.slots[instr.right].floatValue());
            break;
        case Opcode::FMUL:
            frame.slots[instr.dest] = Value::fromFloat(
                frame.slots[instr.left].floatValue() * frame.slots[instr.right].floatValue());
            break;
        case Opcode::FDIV: {
            double r = frame.slots[instr.right].floatValue();
            if (r == 0.0) { pendingThrow_ = makeErrorValue("float division by zero", "E_DIVZERO", instr.sourceLine, instr.sourceCol); break; }
            frame.slots[instr.dest] = Value::fromFloat(frame.slots[instr.left].floatValue() / r);
            break;
        }
        // #237: double üs — libm pow(). Tamsayıdan farklı olarak burada
        // pow() DOĞRU seçimdir: JIT de aynı libm pow()'u çağırır (MIR'de
        // native üs komutu yok), yani iki backend bit-birebir aynı sonucu
        // verir. Negatif üs ondalıkta tanımlıdır (2.0 ** -1 = 0.5), hata yok.
        case Opcode::FPOW:
            frame.slots[instr.dest] = Value::fromFloat(
                std::pow(frame.slots[instr.left].floatValue(),
                         frame.slots[instr.right].floatValue()));
            break;
        // #241: ondalık kalan. `/` ile aynı sözleşme: sıfır bölen IEEE NaN
        // değil, yakalanabilir E_DIVZERO. JIT aynı std::fmod gövdesini çağırır.
        case Opcode::FMOD: {
            double r = frame.slots[instr.right].floatValue();
            if (r == 0.0) { pendingThrow_ = makeErrorValue("float modulo by zero", "E_DIVZERO", instr.sourceLine, instr.sourceCol); break; }
            frame.slots[instr.dest] = Value::fromFloat(std::fmod(frame.slots[instr.left].floatValue(), r));
            break;
        }
        case Opcode::F32MOD: {
            float r = (float) frame.slots[instr.right].floatValue();
            if (r == 0.0f) { pendingThrow_ = makeErrorValue("float modulo by zero", "E_DIVZERO", instr.sourceLine, instr.sourceCol); break; }
            frame.slots[instr.dest] = Value::fromFloat32(
                std::fmod((float) frame.slots[instr.left].floatValue(), r));
            break;
        }
        case Opcode::FNEG:
            frame.slots[instr.dest] = Value::fromFloat(-frame.slots[instr.src].floatValue());
            break;
        case Opcode::INT_TO_FLOAT:
            frame.slots[instr.dest] = Value::fromFloat((double)frame.slots[instr.src].intValue());
            break;
        case Opcode::FLOAT_TO_INT:
            frame.slots[instr.dest] = Value::fromInt((int)frame.slots[instr.src].floatValue());
            break;

        // ── float32 aritmetiği (ADR-040) — gerçek `float` hassasiyetiyle
        // hesaplanır (double'a genişletip yuvarlamak değil), MIR'in native
        // MIR_T_F FADD/FSUB/FMUL/FDIV'iyle bit-birebir aynı sonucu vermek için
        // (çift-yuvarlama riskinden kaçınılır, VM≡JIT diferansiyel sözleşme).
        case Opcode::LOAD_FLOAT32:
            frame.slots[instr.dest] = Value::fromFloat32(instr.floatValue);
            break;
        case Opcode::F32ADD:
            frame.slots[instr.dest] = Value::fromFloat32((double)(
                (float)frame.slots[instr.left].floatValue() + (float)frame.slots[instr.right].floatValue()));
            break;
        case Opcode::F32SUB:
            frame.slots[instr.dest] = Value::fromFloat32((double)(
                (float)frame.slots[instr.left].floatValue() - (float)frame.slots[instr.right].floatValue()));
            break;
        case Opcode::F32MUL:
            frame.slots[instr.dest] = Value::fromFloat32((double)(
                (float)frame.slots[instr.left].floatValue() * (float)frame.slots[instr.right].floatValue()));
            break;
        case Opcode::F32DIV: {
            float r = (float)frame.slots[instr.right].floatValue();
            if (r == 0.0f) { pendingThrow_ = makeErrorValue("float division by zero", "E_DIVZERO", instr.sourceLine, instr.sourceCol); break; }
            frame.slots[instr.dest] = Value::fromFloat32((double)((float)frame.slots[instr.left].floatValue() / r));
            break;
        }
        // #237: float32 üs — powf() (double'a genişletip pow() DEĞİL:
        // çift yuvarlama JIT ile ayrışmaya yol açardı, F32ADD ile aynı kural).
        case Opcode::F32POW:
            frame.slots[instr.dest] = Value::fromFloat32((double)(
                ::powf((float)frame.slots[instr.left].floatValue(),
                          (float)frame.slots[instr.right].floatValue())));
            break;
        case Opcode::F32NEG:
            frame.slots[instr.dest] = Value::fromFloat32((double)(-(float)frame.slots[instr.src].floatValue()));
            break;
        case Opcode::INT_TO_FLOAT32:
            frame.slots[instr.dest] = Value::fromFloat32((double)frame.slots[instr.src].intValue());
            break;
        case Opcode::FLOAT32_TO_INT: {
            float fv = (float)frame.slots[instr.src].floatValue();
            if (!std::isfinite(fv) || fv < (float)INT_MIN || fv > (float)INT_MAX) {
                if (instr.left == 1) frame.slots[instr.dest] = Value::null();
                else pendingThrow_ = makeErrorValue(
                    "float value out of int range or NaN/Inf", "E_CAST",
                    instr.sourceLine, instr.sourceCol);
            } else {
                frame.slots[instr.dest] = Value::fromInt((int)fv);
            }
            break;
        }
        case Opcode::FLOAT_TO_FLOAT32:
            // double → float: veri kaybı gerçekleşir (E003 derleme zamanında uyardı).
            frame.slots[instr.dest] = Value::fromFloat32(frame.slots[instr.src].floatValue());
            break;
        case Opcode::FLOAT32_TO_FLOAT:
            // float → double: kayıpsız genişletme, kind değişir (Float32 → Float).
            frame.slots[instr.dest] = Value::fromFloat(frame.slots[instr.src].floatValue());
            break;

        // ── longint aritmetiği (ADR-040) — 64-bit, rank kulesi dışında izole ──
        case Opcode::LOAD_LONG:
            frame.slots[instr.dest] = Value::fromLongInt(instr.int64Value);
            break;
        case Opcode::LADD:
            frame.slots[instr.dest] = Value::fromLongInt(
                wrapAddI64(frame.slots[instr.left].int64Value(), frame.slots[instr.right].int64Value()));
            break;
        case Opcode::LSUB:
            frame.slots[instr.dest] = Value::fromLongInt(
                wrapSubI64(frame.slots[instr.left].int64Value(), frame.slots[instr.right].int64Value()));
            break;
        case Opcode::LMUL:
            frame.slots[instr.dest] = Value::fromLongInt(
                wrapMulI64(frame.slots[instr.left].int64Value(), frame.slots[instr.right].int64Value()));
            break;
        case Opcode::LDIV: {
            long long d = frame.slots[instr.right].int64Value();
            if (d == 0) { pendingThrow_ = makeErrorValue("division by zero", "E_DIVZERO", instr.sourceLine, instr.sourceCol); break; }
            frame.slots[instr.dest] = Value::fromLongInt(
                wrapDivI64(frame.slots[instr.left].int64Value(), d));
            break;
        }
        case Opcode::LMOD: {
            long long d = frame.slots[instr.right].int64Value();
            if (d == 0) { pendingThrow_ = makeErrorValue("modulo by zero", "E_DIVZERO", instr.sourceLine, instr.sourceCol); break; }
            frame.slots[instr.dest] = Value::fromLongInt(
                wrapModI64(frame.slots[instr.left].int64Value(), d));
            break;
        }
        case Opcode::LNEG:
            frame.slots[instr.dest] = Value::fromLongInt(wrapNegI64(frame.slots[instr.src].int64Value()));
            break;
        case Opcode::LBAND:
            frame.slots[instr.dest] = Value::fromLongInt(
                frame.slots[instr.left].int64Value() & frame.slots[instr.right].int64Value());
            break;
        case Opcode::LBOR:
            frame.slots[instr.dest] = Value::fromLongInt(
                frame.slots[instr.left].int64Value() | frame.slots[instr.right].int64Value());
            break;
        case Opcode::LBXOR:
            frame.slots[instr.dest] = Value::fromLongInt(
                frame.slots[instr.left].int64Value() ^ frame.slots[instr.right].int64Value());
            break;
        case Opcode::LSHL:
            frame.slots[instr.dest] = Value::fromLongInt(
                wrapShlI64(frame.slots[instr.left].int64Value(), frame.slots[instr.right].int64Value()));
            break;
        case Opcode::LSHR:
            frame.slots[instr.dest] = Value::fromLongInt(
                wrapShrI64(frame.slots[instr.left].int64Value(), frame.slots[instr.right].int64Value()));
            break;
        case Opcode::LBNOT:
            frame.slots[instr.dest] = Value::fromLongInt(~frame.slots[instr.src].int64Value());
            break;
        case Opcode::INT_TO_LONG:
            // int → longint: kayıpsız genişletme, işaret uzatılır (32→64 bit).
            frame.slots[instr.dest] = Value::fromLongInt((long long)frame.slots[instr.src].intValue());
            break;
        case Opcode::LONG_TO_INT_CHECKED: {
            long long lv = frame.slots[instr.src].int64Value();
            if (lv < INT_MIN || lv > INT_MAX) {
                if (instr.left == 1) frame.slots[instr.dest] = Value::null();
                else pendingThrow_ = makeErrorValue(
                    "longint value " + std::to_string(lv) + " out of int range", "E_CAST",
                    instr.sourceLine, instr.sourceCol);
            } else {
                frame.slots[instr.dest] = Value::fromInt((int)lv);
            }
            break;
        }

        // ── Struct (ADR-020: referans semantiği) ──────────────────────────
        case Opcode::STRUCT_NEW: {
            StructObject* obj = heap_.allocStruct(instr.intValue);
            // #218: fieldNames IRFunction metadata'dan al
            {
                const auto& fn = *callStack_.back().function;
                auto it = fn.structFieldNames.find(instr.functionName);
                if (it != fn.structFieldNames.end()) {
                    auto names = std::make_shared<std::vector<std::string>>(it->second);
                    auto& reg = structFieldNamesRegistry_;
                    auto regIt = reg.find(instr.functionName);
                    if (regIt != reg.end()) {
                        obj->fieldNames = regIt->second;
                    } else {
                        reg[instr.functionName] = names;
                        obj->fieldNames = names;
                    }
                }
            }
            // ADR-021 zero-init: nullable alanlar (`T? f`) null başlar.
            // allocStruct alanları varsayılan Value{} ile doldurur; o da
            // ValueKind::Int (0). `int f` için doğru, `T? f` için değil —
            // maske olmadan `s.f == null` sessizce false dönerdi.
            {
                const auto& fn = *callStack_.back().function;
                auto nit = fn.structFieldNullable.find(instr.functionName);
                if (nit != fn.structFieldNullable.end()) {
                    const auto& mask = nit->second;
                    size_t n = std::min(mask.size(), obj->fields.size());
                    for (size_t fi = 0; fi < n; fi++)
                        if (mask[fi]) obj->fields[fi] = Value::null();
                }
            }
            callStack_.back().slots[instr.dest] = Value::fromRef(obj);
            break;
        }
        case Opcode::FIELD_GET: {
            Value& objVal = frame.slots[instr.src];
            // #255: null struct üzerinde alan erişimi yakalanabilir bir
            // runtime hatasıdır (E_NULL), süreç sonlandıran std::runtime_error
            // değil. JIT aynı mesaj/kodu üretir (rt_jit_null_struct).
            if (objVal.kind != ValueKind::Ref || !objVal.ref()) {
                pendingThrow_ = makeErrorValue("null struct access", "E_NULL", instr.sourceLine, instr.sourceCol);
                break;
            }
            auto* obj = (StructObject*)objVal.ref();
            int idx = instr.intValue;
            if (idx < 0 || idx >= (int)obj->fields.size())
                throw std::runtime_error("invalid struct field index " + std::to_string(idx));
            frame.slots[instr.dest] = obj->fields[idx];
            break;
        }
        case Opcode::FIELD_SET: {
            Value& objVal = frame.slots[instr.dest];
            if (objVal.kind != ValueKind::Ref || !objVal.ref()) {
                pendingThrow_ = makeErrorValue("null struct access", "E_NULL", instr.sourceLine, instr.sourceCol);
                break;
            }
            auto* obj = (StructObject*)objVal.ref();
            int idx = instr.intValue;
            if (idx < 0 || idx >= (int)obj->fields.size())
                throw std::runtime_error("invalid struct field index " + std::to_string(idx));
            obj->fields[idx] = frame.slots[instr.right];
            break;
        }

        // ── Array (ADR-020: referans semantiği, #206: packed type-tagged) ──
        case Opcode::ARRAY_NEW: {
            ArrayObject* arr = heap_.allocArray(instr.intValue, instr.arrayElemKind);
            // Elemanları varsayılan değerle doldur (constructor reserve kullanır,
            // resize yapmaz — #206: slice/push builtin'leri push_back ile çalışır)
            switch (instr.arrayElemKind) {
                case ArrayElemKind::Ref:     arr->elements.resize(instr.intValue, Value::fromInt(0)); break;
                case ArrayElemKind::Byte:    arr->bytes.resize(instr.intValue, 0);  break;
                case ArrayElemKind::Int:     arr->ints.resize(instr.intValue, 0);   break;
                case ArrayElemKind::LongInt: arr->longs.resize(instr.intValue, 0);  break;
                case ArrayElemKind::Float32: arr->f32s.resize(instr.intValue, 0.0f); break;
                case ArrayElemKind::Float64: arr->f64s.resize(instr.intValue, 0.0);  break;
                case ArrayElemKind::Decimal: arr->decimals.resize(instr.intValue);   break;
            }
            frame.slots[instr.dest] = Value::fromRef(arr);
            break;
        }
        case Opcode::ARRAY_GET: {
            Value& arrVal = frame.slots[instr.left];
            if (arrVal.kind != ValueKind::Ref || !arrVal.ref()) {
                pendingThrow_ = makeErrorValue("expected array, got different type", "E_TYPE", instr.sourceLine, instr.sourceCol); break;
            }
            auto* arr = (ArrayObject*)arrVal.ref();
            int idx = frame.slots[instr.right].intValue();
            // #206: elemKind'a göre doğru buffer'ın size'ını kontrol et
            int len = 0;
            switch (arr->elemKind) {
                case ArrayElemKind::Ref:     len = (int)arr->elements.size(); break;
                case ArrayElemKind::Byte:    len = (int)arr->bytes.size();    break;
                case ArrayElemKind::Int:     len = (int)arr->ints.size();     break;
                case ArrayElemKind::LongInt: len = (int)arr->longs.size();    break;
                case ArrayElemKind::Float32: len = (int)arr->f32s.size();     break;
                case ArrayElemKind::Float64: len = (int)arr->f64s.size();     break;
                case ArrayElemKind::Decimal: len = (int)arr->decimals.size(); break;
            }
            if (idx < 0 || idx >= len) {
                pendingThrow_ = makeErrorValue(
                    "array index out of bounds (index=" + std::to_string(idx) +
                    ", length=" + std::to_string(len) + ")", "E_OOB",
                    instr.sourceLine, instr.sourceCol);
                break;
            }
            // #206: elemKind'a göre doğru buffer'dan oku
            switch (arr->elemKind) {
                case ArrayElemKind::Ref:     frame.slots[instr.dest] = arr->elements[idx]; break;
                case ArrayElemKind::Byte:    frame.slots[instr.dest] = Value::fromInt(arr->bytes[idx]); break;
                case ArrayElemKind::Int:     frame.slots[instr.dest] = Value::fromInt(arr->ints[idx]); break;
                case ArrayElemKind::LongInt: frame.slots[instr.dest] = Value::fromLongInt(arr->longs[idx]); break;
                case ArrayElemKind::Float32: frame.slots[instr.dest] = Value::fromFloat32(arr->f32s[idx]); break;
                case ArrayElemKind::Float64: frame.slots[instr.dest] = Value::fromFloat(arr->f64s[idx]); break;
                case ArrayElemKind::Decimal: frame.slots[instr.dest] = Value::fromDecimal(arr->decimals[idx]); break;
            }
            break;
        }
        case Opcode::ARRAY_SET: {
            Value& arrVal = frame.slots[instr.dest];
            if (arrVal.kind != ValueKind::Ref || !arrVal.ref()) {
                pendingThrow_ = makeErrorValue("expected array, got different type", "E_TYPE", instr.sourceLine, instr.sourceCol); break;
            }
            auto* arr = (ArrayObject*)arrVal.ref();
            int idx = frame.slots[instr.left].intValue();
            // #206: elemKind'a göre doğru buffer'ın size'ını kontrol et
            int len = 0;
            switch (arr->elemKind) {
                case ArrayElemKind::Ref:     len = (int)arr->elements.size(); break;
                case ArrayElemKind::Byte:    len = (int)arr->bytes.size();    break;
                case ArrayElemKind::Int:     len = (int)arr->ints.size();     break;
                case ArrayElemKind::LongInt: len = (int)arr->longs.size();    break;
                case ArrayElemKind::Float32: len = (int)arr->f32s.size();     break;
                case ArrayElemKind::Float64: len = (int)arr->f64s.size();     break;
                case ArrayElemKind::Decimal: len = (int)arr->decimals.size(); break;
            }
            if (idx < 0 || idx >= len) {
                pendingThrow_ = makeErrorValue(
                    "array index out of bounds (index=" + std::to_string(idx) +
                    ", length=" + std::to_string(len) + ")", "E_OOB",
                    instr.sourceLine, instr.sourceCol);
                break;
            }
            // #206: elemKind'a göre doğru buffer'a yaz
            const Value& val = frame.slots[instr.right];
            switch (arr->elemKind) {
                case ArrayElemKind::Ref:     arr->elements[idx] = val; break;
                case ArrayElemKind::Byte:    arr->bytes[idx] = (uint8_t)val.intValue(); break;
                case ArrayElemKind::Int:     arr->ints[idx] = val.intValue(); break;
                case ArrayElemKind::LongInt: arr->longs[idx] = val.asI64(); break;
                case ArrayElemKind::Float32: arr->f32s[idx] = (float)val.asDouble(); break;
                case ArrayElemKind::Float64: arr->f64s[idx] = val.asDouble(); break;
                case ArrayElemKind::Decimal: arr->decimals[idx] = val.decimalValue(); break;
            }
            break;
        }
        case Opcode::ARRAY_LEN: {
            Value& arrVal = frame.slots[instr.src];
            if (arrVal.kind != ValueKind::Ref || !arrVal.ref()) {
                pendingThrow_ = makeErrorValue("null array access", "E_NULL", instr.sourceLine, instr.sourceCol);
                break;
            }
            auto* arr = (ArrayObject*)arrVal.ref();
            // #206: elemKind'a göre doğru buffer'ın size'ını döndür
            int len = 0;
            switch (arr->elemKind) {
                case ArrayElemKind::Ref:     len = (int)arr->elements.size(); break;
                case ArrayElemKind::Byte:    len = (int)arr->bytes.size();    break;
                case ArrayElemKind::Int:     len = (int)arr->ints.size();     break;
                case ArrayElemKind::LongInt: len = (int)arr->longs.size();    break;
                case ArrayElemKind::Float32: len = (int)arr->f32s.size();     break;
                case ArrayElemKind::Float64: len = (int)arr->f64s.size();     break;
                case ArrayElemKind::Decimal: len = (int)arr->decimals.size(); break;
            }
            frame.slots[instr.dest] = Value::fromInt(len);
            break;
        }

        // ── Tip dönüşümleri (ADR-026: as operatörü) ─────────────────────
        case Opcode::CAST_INT_TO_STR: {
            frame.slots[instr.dest] = Value::fromString(
                std::to_string(frame.slots[instr.src].intValue()));
            break;
        }
        case Opcode::CAST_FLOAT_TO_STR: {
            std::ostringstream oss;
            double fv = frame.slots[instr.src].floatValue();
            oss << fv;
            frame.slots[instr.dest] = Value::fromString(oss.str());
            break;
        }
        case Opcode::CAST_BOOL_TO_STR:
            frame.slots[instr.dest] = Value::fromString(
                frame.slots[instr.src].intValue() ? "true" : "false");
            break;

        case Opcode::CAST_STR_TO_INT: {
            const std::string& s = frame.slots[instr.src].stringValue();
            try {
                size_t pos;
                long long v = std::stoll(s, &pos);
                if (pos != s.size()) throw std::invalid_argument("incomplete parse");
                if (v < INT_MIN || v > INT_MAX) throw std::out_of_range("overflow");
                frame.slots[instr.dest] = Value::fromInt((int)v);
            } catch (...) {
                if (instr.left == 1) frame.slots[instr.dest] = Value::null();
                else pendingThrow_ = makeErrorValue(
                    "'" + s + "' cannot convert to int", "E_CAST",
                    instr.sourceLine, instr.sourceCol);
            }
            break;
        }
        case Opcode::CAST_STR_TO_FLOAT: {
            const std::string& s = frame.slots[instr.src].stringValue();
            try {
                size_t pos;
                double v = std::stod(s, &pos);
                if (pos != s.size()) throw std::invalid_argument("incomplete parse");
                frame.slots[instr.dest] = Value::fromFloat(v);
            } catch (...) {
                if (instr.left == 1) frame.slots[instr.dest] = Value::null();
                else pendingThrow_ = makeErrorValue(
                    "'" + s + "' cannot convert to float", "E_CAST",
                    instr.sourceLine, instr.sourceCol);
            }
            break;
        }
        case Opcode::CAST_FLOAT_TO_INT_CHECKED: {
            double fv = frame.slots[instr.src].floatValue();
            if (!std::isfinite(fv) || fv < (double)INT_MIN || fv > (double)INT_MAX) {
                if (instr.left == 1) frame.slots[instr.dest] = Value::null();
                else pendingThrow_ = makeErrorValue(
                    "float value out of int range or NaN/Inf", "E_CAST",
                    instr.sourceLine, instr.sourceCol);
            } else {
                frame.slots[instr.dest] = Value::fromInt((int)fv); // truncate to zero
            }
            break;
        }
        case Opcode::CAST_LONG_TO_STR: {
            frame.slots[instr.dest] = Value::fromString(
                std::to_string(frame.slots[instr.src].int64Value()));
            break;
        }
        case Opcode::CAST_STR_TO_LONG: {
            const std::string& s = frame.slots[instr.src].stringValue();
            try {
                size_t pos;
                long long v = std::stoll(s, &pos);
                if (pos != s.size()) throw std::invalid_argument("incomplete parse");
                frame.slots[instr.dest] = Value::fromLongInt(v);
            } catch (...) {
                if (instr.left == 1) frame.slots[instr.dest] = Value::null();
                else pendingThrow_ = makeErrorValue(
                    "'" + s + "' cannot convert to longint", "E_CAST",
                    instr.sourceLine, instr.sourceCol);
            }
            break;
        }
        case Opcode::CAST_FLOAT32_TO_STR: {
            // MIR rt_jit_float32_to_str ile birebir (setprecision(9), gerçek float).
            std::ostringstream oss;
            oss << std::setprecision(9) << (float)frame.slots[instr.src].floatValue();
            frame.slots[instr.dest] = Value::fromString(oss.str());
            break;
        }
        case Opcode::CAST_STR_TO_FLOAT32: {
            const std::string& s = frame.slots[instr.src].stringValue();
            try {
                size_t pos;
                float v = std::stof(s, &pos);
                if (pos != s.size()) throw std::invalid_argument("incomplete parse");
                frame.slots[instr.dest] = Value::fromFloat32((double)v);
            } catch (...) {
                if (instr.left == 1) frame.slots[instr.dest] = Value::null();
                else pendingThrow_ = makeErrorValue(
                    "'" + s + "' cannot convert to float", "E_CAST",
                    instr.sourceLine, instr.sourceCol);
            }
            break;
        }
        case Opcode::CAST_FLOAT_TO_LONG_CHECKED: {
            double fv = frame.slots[instr.src].floatValue();
            if (!std::isfinite(fv) || fv < -9223372036854775808.0 || fv >= 9223372036854775808.0) {
                if (instr.left == 1) frame.slots[instr.dest] = Value::null();
                else pendingThrow_ = makeErrorValue(
                    "float value out of longint range or NaN/Inf", "E_CAST",
                    instr.sourceLine, instr.sourceCol);
            } else {
                frame.slots[instr.dest] = Value::fromLongInt((long long)fv); // sıfıra kırp
            }
            break;
        }
        case Opcode::CAST_INT_TO_BYTE_CHECKED: {
            // #86: int → byte, 0-255 dışı sessiz kırpılmaz — fallible
            int iv = frame.slots[instr.src].intValue();
            if (iv < 0 || iv > 255) {
                if (instr.left == 1) frame.slots[instr.dest] = Value::null();
                else pendingThrow_ = makeErrorValue(
                    // Mesaj SARMA YOLUNU da gösterir: `as byte` kontrollüdür
                    // (ADR-040 Faz 4 — tipler arası dönüşüm doğrular), sarma
                    // isteyen kullanıcının yazabileceği ifade `(x & 255) as
                    // byte`'tır ve o hata vermez. JIT'teki eşdeğer mesajla
                    // birebir aynı tutulmalıdır (mir_backend.cpp).
                    "integer value " + std::to_string(iv) +
                        " out of byte range (0-255) — to wrap, mask first: "
                        "`(value & 255) as byte`",
                    "E_CAST", instr.sourceLine, instr.sourceCol);
            } else {
                frame.slots[instr.dest] = Value::fromInt(iv); // byte int olarak taşınır
            }
            break;
        }

        // ── Decimal aritmetik (ADR-028) ──────────────────────────────────
        case Opcode::LOAD_DECIMAL:
            frame.slots[instr.dest] = Value::fromDecimal(instr.decimalValue);
            break;
        case Opcode::DADD: {
            auto r = DecimalValue::add(frame.slots[instr.left].decimalValue(),
                                       frame.slots[instr.right].decimalValue());
            if (r.isOverflow()) {
                pendingThrow_ = makeErrorValue("decimal overflow", "E_DECIMAL_OVERFLOW",
                                               instr.sourceLine, instr.sourceCol); break;
            }
            frame.slots[instr.dest] = Value::fromDecimal(r);
            break;
        }
        case Opcode::DSUB: {
            auto r = DecimalValue::sub(frame.slots[instr.left].decimalValue(),
                                       frame.slots[instr.right].decimalValue());
            if (r.isOverflow()) {
                pendingThrow_ = makeErrorValue("decimal overflow", "E_DECIMAL_OVERFLOW",
                                               instr.sourceLine, instr.sourceCol); break;
            }
            frame.slots[instr.dest] = Value::fromDecimal(r);
            break;
        }
        case Opcode::DMUL: {
            auto r = DecimalValue::mul(frame.slots[instr.left].decimalValue(),
                                       frame.slots[instr.right].decimalValue());
            if (r.isOverflow()) {
                pendingThrow_ = makeErrorValue("decimal overflow", "E_DECIMAL_OVERFLOW",
                                               instr.sourceLine, instr.sourceCol); break;
            }
            frame.slots[instr.dest] = Value::fromDecimal(r);
            break;
        }
        case Opcode::DDIV: {
            const DecimalValue& divisor = frame.slots[instr.right].decimalValue();
            if (divisor.coeff == 0) {
                pendingThrow_ = makeErrorValue("decimal division by zero", "E_DECIMAL_DIVZERO",
                                               instr.sourceLine, instr.sourceCol); break;
            }
            auto r = DecimalValue::div(frame.slots[instr.left].decimalValue(), divisor);
            if (r.isOverflow()) {
                pendingThrow_ = makeErrorValue("decimal overflow", "E_DECIMAL_OVERFLOW",
                                               instr.sourceLine, instr.sourceCol); break;
            }
            frame.slots[instr.dest] = Value::fromDecimal(r);
            break;
        }
        case Opcode::DMOD: {
            const DecimalValue& divisor = frame.slots[instr.right].decimalValue();
            if (divisor.coeff == 0) {
                pendingThrow_ = makeErrorValue("decimal modulo by zero", "E_DECIMAL_DIVZERO",
                                               instr.sourceLine, instr.sourceCol); break;
            }
            auto r = DecimalValue::mod(frame.slots[instr.left].decimalValue(), divisor);
            if (r.isOverflow()) {
                pendingThrow_ = makeErrorValue("decimal overflow", "E_DECIMAL_OVERFLOW",
                                               instr.sourceLine, instr.sourceCol); break;
            }
            frame.slots[instr.dest] = Value::fromDecimal(r);
            break;
        }
        case Opcode::DNEG:
            frame.slots[instr.dest] = Value::fromDecimal(
                DecimalValue::neg(frame.slots[instr.src].decimalValue()));
            break;
        case Opcode::INT_TO_DECIMAL:
            frame.slots[instr.dest] = Value::fromDecimal(
                DecimalValue::fromInt(frame.slots[instr.src].intValue()));
            break;
        case Opcode::FLOAT_TO_DECIMAL:
            frame.slots[instr.dest] = Value::fromDecimal(
                DecimalValue::fromDouble(frame.slots[instr.src].floatValue()));
            break;
        case Opcode::CAST_DECIMAL_TO_STR:
            frame.slots[instr.dest] = Value::fromString(
                frame.slots[instr.src].decimalValue().toString());
            break;
        case Opcode::CAST_DECIMAL_TO_FLOAT:
            frame.slots[instr.dest] = Value::fromFloat(
                frame.slots[instr.src].decimalValue().toDouble());
            break;
        case Opcode::CAST_DECIMAL_TO_INT: {
            const DecimalValue& dv = frame.slots[instr.src].decimalValue();
            DecimalValue trunc = DecimalValue::truncate(dv);
            if (trunc.coeff < INT_MIN || trunc.coeff > INT_MAX) {
                if (instr.left == 1) frame.slots[instr.dest] = Value::null();
                else pendingThrow_ = makeErrorValue(
                    "decimal value out of int range", "E_CAST",
                    instr.sourceLine, instr.sourceCol);
            } else {
                frame.slots[instr.dest] = Value::fromInt((int)trunc.coeff);
            }
            break;
        }
        case Opcode::CAST_STR_TO_DECIMAL: {
            const std::string& s = frame.slots[instr.src].stringValue();
            try {
                DecimalValue dv = DecimalValue::fromString(s);
                frame.slots[instr.dest] = Value::fromDecimal(dv);
            } catch (...) {
                if (instr.left == 1) frame.slots[instr.dest] = Value::null();
                else pendingThrow_ = makeErrorValue(
                    "'" + s + "' cannot convert to decimal", "E_CAST",
                    instr.sourceLine, instr.sourceCol);
            }
            break;
        }

        // ── String (ADR-024: immutable değer-tipi, içerik ==) ────────────
        case Opcode::STRING_CONCAT:
            frame.slots[instr.dest] = Value::fromString(
                frame.slots[instr.left].stringValue() +
                frame.slots[instr.right].stringValue());
            break;

        // ── Hata yönetimi (ADR-025) ──────────────────────────────────────
        case Opcode::ENTER_TRY:
            tryStack_.push_back({callStack_.size(), instr.jumpTarget, instr.dest});
            break;

        case Opcode::LEAVE_TRY:
            if (!tryStack_.empty()) tryStack_.pop_back();
            break;

        case Opcode::THROW: {
            Value errVal = frame.slots[instr.src];
            // If user throws Error struct, fill trace field (fields[3])
            if (errVal.kind == ValueKind::Ref && errVal.ref() &&
                errVal.ref()->type == ObjectType::Struct) {
                auto* errObj = static_cast<StructObject*>(errVal.ref());
                if ((int)errObj->fields.size() >= 4)
                    errObj->fields[3] = Value::fromString(buildTrace());
            } else {
                // #4 — mimari karar: struct-olmayan (düz string vb.) throw
                // değeri otomatik Error{message=<değerin string temsili>,
                // code="", line, col, trace}'a sarmalanır. Böylece her
                // `catch (Error e)` güvenle e.code/e.message okuyabilir.
                errVal = makeErrorValue(errVal.toString(), "",
                                         instr.sourceLine, instr.sourceCol);
            }
            pendingThrow_ = errVal;
            break;
        }

        // ── FFI ───────────────────────────────────────────────────────────
        case Opcode::CALLHOST: {
            // #229: TEK index-tabanlı yol — functionName (print/__ffi__/
            // __builtin_method__) yalnız IR dump etiketidir, dispatch ona
            // bakmaz. Her aile (host fonksiyonu, builtin metod, print) aynı
            // rt_host_call girişinden geçer; hata da tek koddan akar (E_HOST).
            if (vmTrace_) [[unlikely]] {
                ++vmTrace_->vmFfiCalls;
                // Bench ayrımı (JIT ile aynı blok mantığı): builtin aralığı.
                if (instr.intValue >= kBuiltinBase && instr.intValue < kCoreBase)
                    ++vmTrace_->vmBuiltinCalls;
            }
            hostScratch_.reset();
            hostScratch_.slots.reserve(instr.argSlots.size());
            for (int s : instr.argSlots)
                hostScratch_.slots.push_back(toHostSlot(frame.slots[s], hostScratch_));

            // #229 (DAP riski): CORE_PRINT thunk'ı env->outputSink'i kullanır.
            // Bağlı değilse (boş sink) nullptr gider — thunk stdout'a yazar;
            // bağlıysa (DAP) adres gider, çıktı protokole yönlenir.
            HostEnv benv{&programArgs_, &heap_, outputSink_ ? &outputSink_ : nullptr};
            HostCallFrame& bf = hostFrame_;
            bf.reset();
            bf.args     = hostScratch_.slots.data();
            bf.argc     = static_cast<int32_t>(hostScratch_.slots.size());
            bf.env      = &benv;
            bf.retOwner = &hostRetOwner_;

            if (rt_host_call(instr.intValue, &bf) != 0) {
                pendingThrow_ = makeErrorValue(bf.err.message,
                                               bf.err.code.empty() ? "E_HOST" : bf.err.code,
                                               instr.sourceLine, instr.sourceCol);
            } else if (instr.dest >= 0) {
                callStack_.back().slots[instr.dest] = fromHostSlot(bf.ret);
            }
            break;
        }
        }

        // ── pendingThrow_ işle: try varsa catch'e unwind, yoksa fırlat ───
        if (pendingThrow_.has_value()) {
            Value errVal = std::move(*pendingThrow_);
            pendingThrow_.reset();

            if (!tryStack_.empty()) {
                TryFrame tf = tryStack_.back();
                tryStack_.pop_back();
                // catch bloğunun bulunduğu frame'e unwind
                while (callStack_.size() > tf.callStackDepth)
                    callStack_.pop_back();
                // Error'ı catch değişkenine bağla ve catch etiketine atla
                callStack_.back().slots[tf.errorSlot] = errVal;
                callStack_.back().instructionPointer  = tf.catchTarget;
            } else {
                // Uncaught error — extract message and raise as C++ exception
                std::string msg = "uncaught error";
                if (errVal.kind == ValueKind::Ref && errVal.ref()) {
                    auto* s = static_cast<StructObject*>(errVal.ref());
                    if ((int)s->fields.size() > 2 &&
                        s->fields[2].kind == ValueKind::String)
                        msg = s->fields[2].stringValue();
                } else if (errVal.kind == ValueKind::String) {
                    msg = errVal.stringValue();
                }
                throw std::runtime_error(msg);
            }
            continue;
        }
    }
    }  // _profExec kapsamı — "vm-exec" burada biter

    // Döngü bitti — callStack boş
    state_ = RunState::Finished;
    return RunReason::Finished;
}

// DAP: run()'ın ilklendirme kısmı. VM'i çalıştırmadan hazırlar.
void Interpreter::initForDebug() {
    if (vmInitialized_) return;

    // Tek-string-modeli: Value::fromString bu koşuda Interpreter'ın
    // heap'ine tahsis etsin (globaller kurulumu string üretebilir).
    setValueStringHeap(&heap_);

    // Globalleri sıfırla — tek flat dizi (bkz. globalSlots_ yorum notu, #3)
    globalSlots_.assign(program_.globalCount, Value::fromInt(0));

    const IRFunction* mainFunction = program_.findFunction("main");
    if (!mainFunction)
        throw std::runtime_error("'main' function not found");

    CallFrame mainFrame;
    mainFrame.function           = mainFunction;
    mainFrame.instructionPointer = 0;
    mainFrame.slots.resize(mainFunction->slotCount, Value::fromInt(0));
    mainFrame.returnDestSlot     = -1;
    callStack_.push_back(std::move(mainFrame));
    vmInitialized_ = true;
}

// Faz 5: run() artık başlatma + runUntilEvent çağrısı.
int Interpreter::run() {
    // Eğer VM zaten başlatıldıysa (DAP resume) — sadece devam et
    if (vmInitialized_ && !callStack_.empty()) {
        runUntilEvent(-1, -1);
        return lastReturnValue_;
    }

    // Tek-string-modeli: Value::fromString bu koşuda Interpreter'ın
    // heap'ine tahsis etsin (her run kendi heap'ini bağlar — thread hazır).
    setValueStringHeap(&heap_);

    {
        Profiling::StageTimer::ScopedStage _prof(stageProfiler_, "vm-warmup");
        initForDebug();
    }

    runUntilEvent(-1, -1);
    return lastReturnValue_;
}

// #229: legacy executeHostFunction("print") yolu silindi — print artık
// registry'de sıradan bir kayıttır (CORE_PRINT thunk'ı) ve CALLHOST'un tek
// index-tabanlı yolundan geçer (yukarıdaki case Opcode::CALLHOST). Aynı
// yol tüm host fonksiyonlarını ve built-in metodları taşır (rt_host_call,
// kBuiltinBase + id); gövdeler src/data/ modüllerinde DataMethod kaydında.
