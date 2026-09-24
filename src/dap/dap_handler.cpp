// ============================================================================
// saQut DAP Handler — Faz 6–7: DAP Protokolü + Davranış Düzeltmeleri (#105)
//
// Tüm mesajlar DAP formatında: {"type":"request|response|event","seq":N,...}
// jsonrpc/method/params zarfı KULLANILMAZ (Content-Length çerçevesi aynı kalır).
//
// Faz 7: print çıktısı Interpreter outputSink'i üzerinden `output` event'ine
// gider (protokol stdout'una çıplak bayt sızmaz); stopOnEntry launch argümanı
// işlenir; setBreakpoints.verified lineToFirstIP'e bakar ve yollar
// kanonikleştirilir (05–08 golden senaryoları).
// ============================================================================

#include "dap/dap_handler.hpp"
#include <filesystem>
#include <iostream>
#include <poll.h>
#include <unistd.h>
#include "module/module_loader.hpp"
#include "module/module_graph.hpp"
#include "core/module_registry.hpp"
#include "symbol/symbol_table.hpp"
#include "symbol/symbol_collector.hpp"
#include "semantic/type_checker.hpp"
#include "semantic/structural_validator.hpp"
#include "diagnostic/diagnostic_engine.hpp"
#include "ir/ir_generator.hpp"
#include "vm/value.hpp"
#include "gc/gc_object.hpp"
#include "gc/gc_heap.hpp"
#include "runtime/threading/park.hpp"
#include "runtime/threading/shared_slots.hpp"
#include "runtime/threading/thread_runtime.hpp"
#include "runtime/threading/thread_table.hpp"
#include <cstdlib>
#include <sstream>
#include <climits>

// ADR-045 Faz 4: park katmanının deadlock işleyicisi düz bir fonksiyon
// işaretçisidir; etkin DAP oturumuna buradan ulaşılır.
static DapHandler* g_activeDap = nullptr;
static void dapDeadlockHandler(const std::string& report) {
    if (g_activeDap) g_activeDap->serveDeadlock(report);
}

// ── Yardımcı: DAP response oluştur ──────────────────────────────────────────

nlohmann::json DapHandler::makeResponse(int requestSeq,
                                         const std::string& command,
                                         const nlohmann::json& body,
                                         bool success) {
    return {
        {"seq",         responseSeq_++},
        {"type",        "response"},
        {"request_seq", requestSeq},
        {"success",     success},
        {"command",     command},
        {"body",        body}
    };
}

// ── Event gönderme ──────────────────────────────────────────────────────────
// DAP event formatı: {"type":"event","event":"...","body":{...},"seq":N}
// jsonrpc/method/params YOK.

void DapHandler::sendEvent(const std::string& event,
                            const nlohmann::json& body) {
    nlohmann::json msg = {
        {"type",  "event"},
        {"event", event},
        {"body",  body},
        {"seq",   responseSeq_++}
    };
    JsonRpc::writeMessage(out_, msg);
}

// ── valueToString ───────────────────────────────────────────────────────────
// Struct/array için tek seviyelik özet üretir: {x: 1, y: 2, z: 3} / [4, 5, 6].
// depth > 0'da (özetin içindeki iç referanslar) kısa gösterime düşer — hem
// okunur kalır hem döngüsel referansta (a.self = a) sonsuz özyineleme olmaz.

std::string DapHandler::valueToString(const Value& v, int depth) const {
    switch (v.kind) {
        case ValueKind::Int:     return std::to_string(v.intValue());
        case ValueKind::LongInt: return std::to_string(v.int64Value());
        case ValueKind::Float:
        case ValueKind::Float32: {
            std::ostringstream ss;
            ss << v.floatValue();
            return ss.str();
        }
        case ValueKind::Decimal: return v.decimalValue().toString();
        case ValueKind::String:  return "\"" + v.stringValue() + "\"";
        case ValueKind::Null:    return "null";
        case ValueKind::Date:    return std::to_string(v.int64Value());
        case ValueKind::Ref: {
            if (!v.ref()) return "null";
            if (v.ref()->type == ObjectType::Struct) {
                auto* s = static_cast<StructObject*>(v.ref());
                if (depth > 0) return "{…}";
                std::string out = "{";
                size_t shown = std::min(s->fields.size(), (size_t)6);
                for (size_t i = 0; i < shown; ++i) {
                    if (i) out += ", ";
                    const auto& fn = s->fieldNames ? *s->fieldNames : std::vector<std::string>();
                    std::string fname = (i < fn.size() && !fn[i].empty())
                        ? fn[i] : std::to_string(i);
                    out += fname + ": " + valueToString(s->fields[i], depth + 1);
                }
                if (s->fields.size() > shown) out += ", …";
                return out + "}";
            }
            auto* a = static_cast<ArrayObject*>(v.ref());
            if (depth > 0) return "[…]";
            std::string out = "[";
            int aLen = 0;
            switch (a->elemKind) {
                case ArrayElemKind::Ref:     aLen = (int)a->elements.size(); break;
                case ArrayElemKind::Byte:    aLen = (int)a->bytes.size();    break;
                case ArrayElemKind::Int:     aLen = (int)a->ints.size();     break;
                case ArrayElemKind::LongInt: aLen = (int)a->longs.size();    break;
                case ArrayElemKind::Float32: aLen = (int)a->f32s.size();     break;
                case ArrayElemKind::Float64: aLen = (int)a->f64s.size();     break;
                case ArrayElemKind::Decimal: aLen = (int)a->decimals.size(); break;
            }
            size_t shown = std::min((size_t)aLen, (size_t)8);
            for (size_t i = 0; i < shown; ++i) {
                if (i) out += ", ";
                Value tmp;
                switch (a->elemKind) {
                    case ArrayElemKind::Ref:     tmp = a->elements[i]; break;
                    case ArrayElemKind::Byte:    tmp = Value::fromInt(a->bytes[i]); break;
                    case ArrayElemKind::Int:     tmp = Value::fromInt(a->ints[i]); break;
                    case ArrayElemKind::LongInt: tmp = Value::fromLongInt(a->longs[i]); break;
                    case ArrayElemKind::Float32: tmp = Value::fromFloat32(a->f32s[i]); break;
                    case ArrayElemKind::Float64: tmp = Value::fromFloat(a->f64s[i]); break;
                    case ArrayElemKind::Decimal: tmp = Value::fromDecimal(a->decimals[i]); break;
                }
                out += valueToString(tmp, depth + 1);
            }
            if ((size_t)aLen > shown) out += ", …";
            return out + "]";
        }
    }
    return "?";
}

// ── variablesReference kayıt defteri ─────────────────────────────────────────
// VS Code bir Ref değerinin çocuklarını sonradan ayrı bir `variables`
// isteğiyle sorar — o istekte hangi Value'nun kastedildiğini bilmek için
// ref numarası → Value eşlemesi tutulur. Koşu devam edince (continue/step)
// eski numaralar DAP spec'i gereği geçersizleşir → invalidateVarRefs.

int DapHandler::registerVarRef(const Value& v) {
    if (v.kind != ValueKind::Ref || !v.ref()) return 0;
    int id = nextVarRef_++;
    varRefs_[id] = v;
    return id;
}

void DapHandler::invalidateVarRefs() {
    varRefs_.clear();
    nextVarRef_ = 100000;
}

// ── Struct/array child variable'ları ─────────────────────────────────────────
// Çocuklar da registerVarRef'ten geçer → kullanıcı Variables panelinde
// istediği kadar derine inebilir (vecs → [0] → x).

nlohmann::json DapHandler::buildChildVariables(const Value& v) {
    nlohmann::json vars = nlohmann::json::array();
    if (v.kind != ValueKind::Ref || !v.ref()) return vars;

    if (v.ref()->type == ObjectType::Struct) {
        auto* s = static_cast<StructObject*>(v.ref());
        for (size_t i = 0; i < s->fields.size(); ++i) {
            const Value& fv = s->fields[i];
            const auto& fn = s->fieldNames ? *s->fieldNames : std::vector<std::string>();
            std::string fname = (i < fn.size() && !fn[i].empty())
                ? fn[i] : "field[" + std::to_string(i) + "]";
            vars.push_back({
                {"name",               fname},
                {"value",              valueToString(fv)},
                {"type",               ""},
                {"variablesReference", registerVarRef(fv)}
            });
        }
    } else if (v.ref()->type == ObjectType::Array) {
        auto* a = static_cast<ArrayObject*>(v.ref());
        int aLen = 0;
        switch (a->elemKind) {
            case ArrayElemKind::Ref:     aLen = (int)a->elements.size(); break;
            case ArrayElemKind::Byte:    aLen = (int)a->bytes.size();    break;
            case ArrayElemKind::Int:     aLen = (int)a->ints.size();     break;
            case ArrayElemKind::LongInt: aLen = (int)a->longs.size();    break;
            case ArrayElemKind::Float32: aLen = (int)a->f32s.size();     break;
            case ArrayElemKind::Float64: aLen = (int)a->f64s.size();     break;
            case ArrayElemKind::Decimal: aLen = (int)a->decimals.size(); break;
        }
        for (int i = 0; i < aLen; ++i) {
            Value ev;
            switch (a->elemKind) {
                case ArrayElemKind::Ref:     ev = a->elements[i]; break;
                case ArrayElemKind::Byte:    ev = Value::fromInt(a->bytes[i]); break;
                case ArrayElemKind::Int:     ev = Value::fromInt(a->ints[i]); break;
                case ArrayElemKind::LongInt: ev = Value::fromLongInt(a->longs[i]); break;
                case ArrayElemKind::Float32: ev = Value::fromFloat32(a->f32s[i]); break;
                case ArrayElemKind::Float64: ev = Value::fromFloat(a->f64s[i]); break;
                case ArrayElemKind::Decimal: ev = Value::fromDecimal(a->decimals[i]); break;
            }
            vars.push_back({
                {"name",               "[" + std::to_string(i) + "]"},
                {"value",              valueToString(ev)},
                {"type",               ""},
                {"variablesReference", registerVarRef(ev)}
            });
        }
    }
    return vars;
}

// ── Koşu döngüsü ────────────────────────────────────────────────────────────
// continue/step sonrası VM'i bütçeli çalıştır, uygun event'i gönder.
//
// Faz 8 (#105): sınırsız runUntilEvent yerine bütçe TURLARI — her turun
// arasında stdin'de bekleyen mesaj var mı bakılır. `pause` gelirse koşu
// bırakılır (DAP sırası: pause response'u stopped event'inden ÖNCE yazılır);
// diğer istekler kuyruklanır ve koşu durunca işlenir. Böylece sonsuz döngülü
// program DAP sunucusunu kilitlemez.

void DapHandler::sendTermination(bool withExit) {
    if (terminationSent_) return;
    terminationSent_ = true;
    if (withExit) sendEvent("exited", {{"exitCode", 0}});
    sendEvent("terminated", {});
}

void DapHandler::reportStepResult() {
    if (!vm_) return;
    if (vm_->state() == Interpreter::RunState::Paused)
        sendStopped({{"reason","step"}, {"threadId",1}});
    else if (vm_->state() == Interpreter::RunState::Finished) {
        finishProgram();
        sendTermination(true);
    }
}

void DapHandler::runWithBudget() {
    if (!vm_) return;
    invalidateVarRefs(); // koşu devam ediyor → eski variablesReference'lar öldü
    pauseWorkers(false); // ADR-045 Faz 4: continue/step tüm thread'leri sürdürür

    while (true) {
        Interpreter::RunReason reason = vm_->runUntilEvent(kRunBudgetChunk, -1);
        drainWorkerOutput();   // ADR-045 Faz 4
        syncThreadEvents();

        switch (reason) {
            case Interpreter::RunReason::Breakpoint: {
                nlohmann::json body = {{"reason","breakpoint"}, {"threadId",1}};
                auto hit = bpIds_.find(vm_->currentLocation());
                if (hit != bpIds_.end()) body["hitBreakpointIds"] = {hit->second};
                sendStopped(body);
                drainPendingRequests();
                return;
            }
            case Interpreter::RunReason::StepDone:
                sendStopped({{"reason","step"}, {"threadId",1}});
                drainPendingRequests();
                return;
            case Interpreter::RunReason::Finished:
                finishProgram();
                sendTermination(true);
                drainPendingRequests();
                return;
            case Interpreter::RunReason::Error:
                sendStopped({{"reason","exception"}, {"threadId",1}});
                drainPendingRequests();
                return;
            case Interpreter::RunReason::BudgetExhausted:
                // Tur arası: bekleyen istemci mesajına bak
                if (reader_.hasPending()) {
                    auto msg = reader_.readMessage();
                    if (!msg.is_null() && !msg.is_discarded()) {
                        std::string cmd = msg.value("command", "");
                        if (cmd == "pause") {
                            int seq = msg.value("seq", 0);
                            // DAP sırası: response ÖNCE, stopped SONRA
                            JsonRpc::writeMessage(out_,
                                makeResponse(seq, "pause", {}));
                            sendStopped({{"reason","pause"}, {"threadId",1}});
                            drainPendingRequests();
                            return;
                        }
                        // pause değil — koşu durunca işlenmek üzere kuyrukla
                        pendingRequests_.push_back(std::move(msg));
                    }
                } else if (reader_.eof()) {
                    // İstemci gitti (EOF) — sonsuz döngüde busy-hang kalma
                    sendTermination(false);
                    return;
                }
                continue; // koşuya devam
        }
    }
}

// Koşu sırasında kuyruklanan istekleri işle (koşu durdu — artık güvenli).
// dispatch yeniden runWithBudget çağırabilir (ör. kuyruklanmış continue).
void DapHandler::drainPendingRequests() {
    while (!pendingRequests_.empty()) {
        nlohmann::json msg = std::move(pendingRequests_.front());
        pendingRequests_.pop_front();
        nlohmann::json resp = dispatch(msg);
        if (!resp.is_null())
            JsonRpc::writeMessage(out_, resp);
    }
}

// ── Dispatch ─────────────────────────────────────────────────────────────────
// DAP mesajı formatı: {"type":"request","seq":N,"command":"...","arguments":{...}}

nlohmann::json DapHandler::dispatch(const nlohmann::json& msg) {
    if (msg.is_discarded() || !msg.contains("type")) return nullptr;

    std::string type = msg["type"].get<std::string>();
    if (type != "request") return nullptr;
    if (!msg.contains("command")) return nullptr;

    std::string command = msg["command"].get<std::string>();
    int seq = msg.value("seq", 0);

    if (command == "initialize")         return handleInitialize(msg);
    if (command == "launch")             return handleLaunch(msg);
    if (command == "setBreakpoints")     return handleSetBreakpoints(msg);
    if (command == "configurationDone")  return handleConfigurationDone(msg);
    if (command == "continue")           return handleContinue(msg);
    if (command == "next")               return handleNext(msg);
    if (command == "stepIn")             return handleStepIn(msg);
    if (command == "stepOut")            return handleStepOut(msg);
    if (command == "pause")              return handlePause(msg);
    if (command == "threads")            return handleThreads(msg);
    if (command == "stackTrace")         return handleStackTrace(msg);
    if (command == "scopes")             return handleScopes(msg);
    if (command == "variables")          return handleVariables(msg);
    if (command == "evaluate")           return handleEvaluate(msg);
    if (command == "terminate")          return handleTerminate(msg);
    if (command == "disconnect")         return handleDisconnect(msg);

    // Bilinmeyen command → success:false döndür
    return makeResponse(seq, command, {}, false);
}

// ─────────────────────────────────────────────────────────────────────────────
// Handler'lar
// ─────────────────────────────────────────────────────────────────────────────

nlohmann::json DapHandler::handleInitialize(const nlohmann::json& req) {
    int seq = req.value("seq", 0);

    nlohmann::json caps = {
        {"supportsConfigurationDoneRequest", true},
        {"supportsFunctionBreakpoints",      false},
        {"supportsConditionalBreakpoints",   false},
        {"supportsSetVariable",              false},
        {"supportsStepBack",                 false},
        {"supportsStepInTargetsRequest",     false},
        {"supportsGotoTargetsRequest",       false},
        {"supportsHitConditionalBreakpoints",false},
        {"supportsTerminateRequest",         true},
        {"supportsExceptionInfoRequest",     false},
        {"supportsEvaluateForHovers",        true},
        {"supportTerminateDebuggee",         true},
        // ADR-045 Faz 4: continue/step tüm thread'leri sürdürür (gdb
        // scheduler-locking off); tek thread yürütme istekleri yok.
        {"supportsSingleThreadExecutionRequests", false}
    };

    // DAP kuralı: ÖNCE response, SONRA initialized event
    nlohmann::json resp = makeResponse(seq, "initialize", caps);

    // VS Code her initialize response'u bekler, sonra initialized event'ini işler.
    // Cevabı yaz, sonra event gönder.
    JsonRpc::writeMessage(out_, resp);
    sendEvent("initialized", {});

    initialized_ = true;
    return nullptr;  // dispatch artık yazmasın — biz zaten yazdık
}

nlohmann::json DapHandler::handleLaunch(const nlohmann::json& req) {
    int seq = req.value("seq", 0);
    nlohmann::json args = req.value("arguments", nlohmann::json::object());

    std::string program = args.value("program", "");
    if (program.empty()) {
        sendEvent("output", {{"category","stderr"},
                             {"output","No program specified\n"}});
        sendTermination(false);
        return makeResponse(seq, "launch", {}, false);
    }

    // Derle
    ModuleRegistry   registry;
    DiagnosticEngine diag;
    SymbolTable      table;
    ModuleGraph      graph = ModuleLoader(registry, diag).load(program);

    if (!diag.hasErrors()) {
        SymbolCollector(table, diag).collectModuleGraph(graph);
        if (!diag.hasErrors()) {
            for (auto& u : graph.units) TypeChecker(table, diag).check(u.ast);
            for (auto& u : graph.units) StructuralValidator(diag).validate(u.ast);
        }
    }

    if (diag.hasErrors()) {
        sendEvent("output", {{"category","stderr"}, {"output","Build failed\n"}});
        sendTermination(false);
        return makeResponse(seq, "launch", {});
    }

    IRGenerator irgen;
    irProgram_ = std::make_unique<IRProgram>(
        irgen.generateModuleGraph(graph, table));

    vm_ = std::make_unique<Interpreter>(*irProgram_);

    // Faz 7 (#105): stopOnEntry launch argümanından okunur (DAP varsayılanı
    // false); configurationDone buna göre entry'de durur ya da koşuya başlar.
    stopOnEntry_ = args.value("stopOnEntry", false);

    // Faz 7 (#105): print çıktısını output event'ine yönlendir — protokol
    // stdout'una çıplak bayt sızmaz, VS Code Debug Console'da görünür.
    // ADR-045 Faz 4: sink işçi thread'lere de kopyalanır; onlar protokol
    // akışına doğrudan yazmaz (DAP thread'inin yazılarıyla karışırdı) —
    // kuyruğa koyar, DAP thread'i boşaltır.
    dapThread_ = std::this_thread::get_id();
    vm_->setOutputSink([this](const std::string& text) {
        if (std::this_thread::get_id() == dapThread_) {
            sendEvent("output", {{"category", "stdout"}, {"output", text}});
        } else {
            std::lock_guard<std::mutex> lk(workerOutMu_);
            workerOut_.push_back(text);
        }
    });

    // ADR-045 Faz 4: thread programı — deadlock süreci öldürmez, stopped olur.
    usesThreads_ = irProgram_->usesThreads;
    if (usesThreads_) {
        g_activeDap = this;
        saqut::threading::setDeadlockHandler(&dapDeadlockHandler);
    }

    // VM'i ilklendir ama çalıştırma — configurationDone'da başlatılacak
    vm_->initForDebug();

    return makeResponse(seq, "launch", {});
}

nlohmann::json DapHandler::handleSetBreakpoints(const nlohmann::json& req) {
    int seq = req.value("seq", 0);
    nlohmann::json args = req.value("arguments", nlohmann::json::object());


    std::string sourceFile;
    if (args.contains("source") && args["source"].contains("path"))
        sourceFile = args["source"]["path"].get<std::string>();

    // Faz 7 (#105): istemcinin yolu göreli/symlink'li gelebilir — VM'deki
    // SourceLocation.filePath'ler kanoniktir (ModuleLoader weakly_canonical
    // uygular); eşleşme için aynı biçime getir.
    if (!sourceFile.empty())
        sourceFile = std::filesystem::weakly_canonical(sourceFile).string();

    // D-8: setBreakpoints dosya başına gelir — yalnız bu dosyanınkiler silinir.
    if (vm_) vm_->clearBreakpointsInFile(sourceFile);
    for (auto it = bpIds_.begin(); it != bpIds_.end();)
        it = (it->first.first == sourceFile) ? bpIds_.erase(it) : std::next(it);

    nlohmann::json bps = nlohmann::json::array();
    if (args.contains("breakpoints")) {
        for (const auto& bp : args["breakpoints"]) {
            int line = bp.value("line", 0);
            bool verified = false;

            int id = nextBpId_++;
            if (vm_) {
                // D-3: çalıştırılabilir değilse (yorum, `}`, fonksiyon başlığı)
                // aynı dosyada sonraki çalıştırılabilir satıra kaydırılır ve
                // yanıt kaydırılmış satırı bildirir; editör noktayı taşır.
                int actual = vm_->nextExecutableLine(sourceFile, line);
                verified = actual > 0;
                if (verified) {
                    line = actual;
                    vm_->setBreakpoint(sourceFile, line);
                    bpIds_[{sourceFile, line}] = id;
                }
            }

            bps.push_back({
                {"id",       id},
                {"verified", verified},
                {"line",     line},
                {"source",   {{"path", sourceFile}}}
            });
        }
    }

    return makeResponse(seq, "setBreakpoints", {{"breakpoints", bps}});
}

nlohmann::json DapHandler::handleConfigurationDone(const nlohmann::json& req) {
    int seq = req.value("seq", 0);

    if (!vm_) {
        return makeResponse(seq, "configurationDone", {});
    }

    // ÖNCE response yaz (DAP kuralı: response her zaman event'ten önce)
    nlohmann::json resp = makeResponse(seq, "configurationDone", {});
    JsonRpc::writeMessage(out_, resp);

    // Faz 7 (#105): stopOnEntry'ye saygı — true ise entry'de dur, false ise
    // doğrudan koşuya başla (breakpoint'e çarpar ya da biter).
    if (stopOnEntry_) {
        // D-1: tek komut çalıştırmak yerine `main`'in ilk görünür satırına
        // kadar ilerle (global başlatıcı prelude'u gizli çalışır).
        vm_->stepToFirstLine();
        if (vm_->state() == Interpreter::RunState::Finished) {
            finishProgram();
            sendTermination(true);
        } else {
            sendStopped({{"reason","entry"}, {"threadId",1}});
        }
    } else {
        runWithBudget();
    }

    return nullptr;
}

nlohmann::json DapHandler::handleContinue(const nlohmann::json& req) {
    int seq = req.value("seq", 0);

    // ÖNCE response yaz
    nlohmann::json resp = makeResponse(seq, "continue", {{"allThreadsContinued", true}});
    JsonRpc::writeMessage(out_, resp);

    if (vm_) {
        runWithBudget();
    }

    return nullptr;
}

nlohmann::json DapHandler::handleNext(const nlohmann::json& req) {
    int seq = req.value("seq", 0);

    // ADR-045 Faz 4 (PLAN B): adımlama yalnız ana thread'de.
    if (req.value("arguments", nlohmann::json::object()).value("threadId", 1) != 1)
        return makeResponse(seq, "next",
            {{"error", "stepping is only supported on the main thread (v1)"}}, false);
    pauseWorkers(false);

    // ÖNCE response yaz
    nlohmann::json resp = makeResponse(seq, "next", {});
    JsonRpc::writeMessage(out_, resp);

    if (vm_) {
        invalidateVarRefs();
        if (vm_->state() == Interpreter::RunState::Finished) {
            sendTermination(true);   // bitmiş programa adım: yalnız bir kez bildir
        } else {
            vm_->stepOver();
            reportStepResult();
        }
    }

    return nullptr;
}

nlohmann::json DapHandler::handleStepIn(const nlohmann::json& req) {
    int seq = req.value("seq", 0);

    // ADR-045 Faz 4 (PLAN B): adımlama yalnız ana thread'de.
    if (req.value("arguments", nlohmann::json::object()).value("threadId", 1) != 1)
        return makeResponse(seq, "stepIn",
            {{"error", "stepping is only supported on the main thread (v1)"}}, false);
    pauseWorkers(false);

    // ÖNCE response yaz
    nlohmann::json resp = makeResponse(seq, "stepIn", {});
    JsonRpc::writeMessage(out_, resp);

    if (vm_) {
        invalidateVarRefs();
        if (vm_->state() == Interpreter::RunState::Finished) {
            sendTermination(true);   // bitmiş programa adım: yalnız bir kez bildir
        } else {
            vm_->stepInto();
            reportStepResult();
        }
    }

    return nullptr;
}

nlohmann::json DapHandler::handleStepOut(const nlohmann::json& req) {
    int seq = req.value("seq", 0);

    // ADR-045 Faz 4 (PLAN B): adımlama yalnız ana thread'de.
    if (req.value("arguments", nlohmann::json::object()).value("threadId", 1) != 1)
        return makeResponse(seq, "stepOut",
            {{"error", "stepping is only supported on the main thread (v1)"}}, false);
    pauseWorkers(false);

    // ÖNCE response yaz
    nlohmann::json resp = makeResponse(seq, "stepOut", {});
    JsonRpc::writeMessage(out_, resp);

    if (vm_) {
        invalidateVarRefs();
        if (vm_->state() == Interpreter::RunState::Finished) {
            sendTermination(true);   // bitmiş programa adım: yalnız bir kez bildir
        } else {
            vm_->stepOut();
            reportStepResult();
        }
    }

    return nullptr;
}

nlohmann::json DapHandler::handlePause(const nlohmann::json& req) {
    int seq = req.value("seq", 0);
    // Faz 8 (#105): koşu SIRASINDA gelen pause runWithBudget'ın tur-arası
    // stdin kontrolünde yakalanır (response + stopped orada yazılır).
    // Buraya düşen pause, VM zaten durmuşken gelmiştir — yalnızca onayla.
    return makeResponse(seq, "pause", {});
}

nlohmann::json DapHandler::handleThreads(const nlohmann::json& req) {
    int seq = req.value("seq", 0);
    nlohmann::json threads = nlohmann::json::array();
    threads.push_back({{"id", 1}, {"name", "main"}});
    // ADR-045 Faz 4: canlı işçi thread'ler ("thread#N @ dosya:satır").
    if (usesThreads_) {
        for (auto* t : saqut::threading::ThreadTable::instance().snapshot())
            if (t->id != 1 && !t->finished())
                threads.push_back({{"id", t->id}, {"name", t->name}});
    }
    return makeResponse(seq, "threads", {{"threads", threads}});
}

nlohmann::json DapHandler::handleStackTrace(const nlohmann::json& req) {
    int seq = req.value("seq", 0);
    nlohmann::json frames = nlohmann::json::array();
    const int threadId =
        req.value("arguments", nlohmann::json::object()).value("threadId", 1);

    if (threadId == 1) {
        if (vm_) {
            int depth = vm_->callDepth();
            for (int i = 0; i < depth; ++i) {
                frames.push_back({
                    {"id",     i},
                    {"name",   vm_->frameFunctionName(i)},
                    {"line",   vm_->frameSourceLine(i)},
                    {"column", 0},
                    {"source", {{"path", vm_->frameSourceFile(i)}}}
                });
            }
        }
    } else if (usesThreads_) {
        // ADR-045 Faz 4: işçi thread — yalnız park'tayken (all-stop) okunur;
        // bloklanmış thread'in üst frame adı "[bekliyor: ...]" ile işaretlenir.
        auto* core = saqut::threading::ThreadTable::instance().find(threadId);
        if (core) {
            const auto info = saqut::threading::parkInfoOf(*core);
            const std::string mark = (info.parked && info.where != "debug pause")
                                         ? "[bekliyor: " + info.where + "] " : "";
            int d0 = 0;
            Interpreter* w = frameInterpreter(threadId * 100, d0);
            if (w) {
                const int depth = std::min(w->callDepth(), 100);
                for (int i = 0; i < depth; ++i) {
                    frames.push_back({
                        {"id",     threadId * 100 + i},
                        {"name",   (i == 0 ? mark : std::string()) + w->frameFunctionName(i)},
                        {"line",   w->frameSourceLine(i)},
                        {"column", 0},
                        {"source", {{"path", w->frameSourceFile(i)}}}
                    });
                }
            } else {
                frames.push_back({
                    {"id",     threadId * 100},
                    {"name",   info.parked ? "[bekliyor: " + info.where + "]" : "[çalışıyor]"},
                    {"line",   0},
                    {"column", 0}
                });
            }
        }
    }

    return makeResponse(seq, "stackTrace",
        {{"stackFrames", frames}, {"totalFrames", (int)frames.size()}});
}

nlohmann::json DapHandler::handleScopes(const nlohmann::json& req) {
    int seq     = req.value("seq", 0);
    nlohmann::json args = req.value("arguments", nlohmann::json::object());
    int frameId = args.value("frameId", 0);

    nlohmann::json scopes = nlohmann::json::array();
    scopes.push_back({
        {"name",               "Locals"},
        {"variablesReference", 1000 + frameId},
        {"expensive",          false}
    });
    // ADR-045 Faz 4: shared primitif değerleri, Pool ve List uzunlukları.
    if (usesThreads_)
        scopes.push_back({
            {"name",               "Shared"},
            {"variablesReference", kSharedScopeRef},
            {"expensive",          false}
        });
    return makeResponse(seq, "scopes", {{"scopes", scopes}});
}

nlohmann::json DapHandler::handleVariables(const nlohmann::json& req) {
    int seq     = req.value("seq", 0);
    nlohmann::json args = req.value("arguments", nlohmann::json::object());
    int ref     = args.value("variablesReference", 0);

    // ADR-045 Faz 4: "Shared" scope (frame aralığından önce ele alınır).
    if (ref == kSharedScopeRef)
        return makeResponse(seq, "variables", {{"variables", sharedVariables()}});

    // Dal sırası önemli: child ref'ler 100000+, frame (scope) ref'leri
    // 1000+frameId — önce child aralığını ele, yoksa 100000 "frame 99000"
    // sanılıp boş döner.
    if (ref >= 100000) {
        auto it = varRefs_.find(ref);
        nlohmann::json vars = (it != varRefs_.end())
            ? buildChildVariables(it->second)
            : nlohmann::json::array();
        return makeResponse(seq, "variables", {{"variables", vars}});
    }

    // Frame variable'ları (ref >= 1000)
    if (ref >= 1000) {
        // ADR-045 Faz 4: frameId işçi thread çerçevesini de gösterebilir.
        int depth = 0;
        Interpreter* fvm = frameInterpreter(ref - 1000, depth);
        nlohmann::json vars = nlohmann::json::array();

        if (fvm && depth < fvm->callDepth()) {
            // IRFunction'daki slot sayısını al
            int slotCount = fvm->frameSlotCount(depth);
            for (int slot = 0; slot < slotCount; ++slot) {
                Value v = fvm->readSlotInFrame(depth, slot);
                std::string name = fvm->slotName(depth, slot);
                if (name.empty()) continue;  // geçici/adsız slotları atla

                vars.push_back({
                    {"name",               name},
                    {"value",              valueToString(v)},
                    {"type",               ""},
                    {"variablesReference", registerVarRef(v)}
                });
            }
        }

        return makeResponse(seq, "variables", {{"variables", vars}});
    }

    // Tanınmayan ref aralığı
    return makeResponse(seq, "variables",
                        {{"variables", nlohmann::json::array()}});
}

// Stop butonu: VS Code, supportsTerminateRequest ilan edildiği için önce
// `terminate` gönderir (nazik durdurma); ardından `disconnect` gelir.
// Bu handler yokken istek "bilinmeyen komut → success:false" düşüyordu ve
// VS Code "An unknown error occurred" gösteriyordu.
nlohmann::json DapHandler::handleTerminate(const nlohmann::json& req) {
    int seq = req.value("seq", 0);

    // ÖNCE response, SONRA terminated eventi (DAP sırası)
    nlohmann::json resp = makeResponse(seq, "terminate", {});
    JsonRpc::writeMessage(out_, resp);

    invalidateVarRefs();
    shutdownThreads();   // ADR-045: işçiler IRProgram'ı kullanıyor — önce bitmeli
    vm_.reset();
    irProgram_.reset();
    sendTermination(false);
    return nullptr;
}

nlohmann::json DapHandler::handleDisconnect(const nlohmann::json& req) {
    int seq = req.value("seq", 0);
    invalidateVarRefs();
    shutdownThreads();   // ADR-045
    vm_.reset();
    irProgram_.reset();
    return makeResponse(seq, "disconnect", {});
}

// ─────────────────────────────────────────────────────────────────────────────
// ADR-045 Faz 4: çok thread'li programlar
// ─────────────────────────────────────────────────────────────────────────────

void DapHandler::drainWorkerOutput() {
    std::vector<std::string> out;
    {
        std::lock_guard<std::mutex> lk(workerOutMu_);
        out.swap(workerOut_);
    }
    for (auto& text : out)
        sendEvent("output", {{"category", "stdout"}, {"output", text}});
}

void DapHandler::syncThreadEvents() {
    if (!usesThreads_) return;
    std::set<int> live;
    for (auto* t : saqut::threading::ThreadTable::instance().snapshot())
        if (t->id != 1 && !t->finished()) live.insert(t->id);
    for (int id : live)
        if (!knownThreads_.count(id))
            sendEvent("thread", {{"reason", "started"}, {"threadId", id}});
    for (int id : knownThreads_)
        if (!live.count(id))
            sendEvent("thread", {{"reason", "exited"}, {"threadId", id}});
    knownThreads_ = std::move(live);
}

void DapHandler::pauseWorkers(bool on) {
    if (usesThreads_) saqut::threading::setDebugPauseAll(on);
}

// All-stop: ana thread durduğunda işçiler bir sonraki yoklama noktasında park
// eder (pop/wait'te bekleyenler zaten durmuş sayılır).
void DapHandler::sendStopped(nlohmann::json body) {
    if (usesThreads_) {
        pauseWorkers(true);
        drainWorkerOutput();
        syncThreadEvents();
        body["allThreadsStopped"] = true;
    }
    sendEvent("stopped", body);
}

// main döndü: açık thread'ler beklenir (CLI ile aynı semantik), sonra olaylar.
void DapHandler::finishProgram() {
    if (!usesThreads_) return;
    pauseWorkers(false);
    saqut::threading::programEnd();
    drainWorkerOutput();
    syncThreadEvents();
}

void DapHandler::shutdownThreads() {
    if (!usesThreads_) return;
    auto& table = saqut::threading::ThreadTable::instance();
    for (auto* t : table.snapshot())
        if (t->id != 1 && !t->finished()) table.requestStop(t->id);
    pauseWorkers(false);
    table.joinAll();
    drainWorkerOutput();
    usesThreads_ = false;
    if (g_activeDap == this) {
        g_activeDap = nullptr;
        saqut::threading::setDeadlockHandler(nullptr);
    }
}

Interpreter* DapHandler::frameInterpreter(int frameId, int& depth) const {
    if (frameId < 100) {
        depth = frameId;
        return vm_.get();
    }
    if (!usesThreads_) return nullptr;
    const int threadId = frameId / 100;
    depth = frameId % 100;
    auto* core = saqut::threading::ThreadTable::instance().find(threadId);
    if (!core || !core->debugTarget) return nullptr;
    // Yalnız park'taki (all-stop'ta duraklatılmış ya da bloklanmış) thread'in
    // çerçeveleri sabittir.
    if (!saqut::threading::parkInfoOf(*core).parked) return nullptr;
    return static_cast<Interpreter*>(core->debugTarget);
}

nlohmann::json DapHandler::sharedVariables() const {
    using namespace saqut::threading;
    nlohmann::json vars = nlohmann::json::array();
    auto& slots = SharedSlots::instance();
    for (int i = 0; i < slots.size(); ++i) {
        SharedSlot& s = slots.at(i);
        std::string value;
        switch (s.kind) {
            case SharedKind::Int:   value = std::to_string(slots.loadInt(i)); break;
            case SharedKind::Float: value = formatFloat32Print(slots.loadFloat(i)); break;
            case SharedKind::Bool:  value = slots.loadBool(i) ? "true" : "false"; break;
            case SharedKind::Pool:  value = "Pool(length=" + std::to_string(slots.pool(i).length()) + ")"; break;
            case SharedKind::List:  value = "List(length=" + std::to_string(slots.list(i).length()) + ")"; break;
        }
        vars.push_back({{"name", s.name}, {"value", value}, {"type", ""},
                        {"variablesReference", 0}});
    }
    return vars;
}

// Deadlock (park katmanı çağırır; tüm thread'ler park'ta — DAP thread'i de
// içeride bloklu). Süreci öldürmek yerine stopped(exception) gönderir ve
// istekleri burada yanıtlar; ilerleme istekleri reddedilir, terminate /
// disconnect süreci 70 ile sonlandırır.
void DapHandler::serveDeadlock(const std::string& report) {
    sendEvent("output", {{"category", "stderr"}, {"output", report}});
    sendEvent("stopped", {{"reason", "exception"},
                          {"description", "deadlock: all threads are blocked"},
                          {"text", report},
                          {"threadId", 1},
                          {"allThreadsStopped", true}});
    while (true) {
        auto msg = reader_.readMessage();
        if (msg.is_null() || msg.is_discarded()) {
            if (reader_.eof()) std::_Exit(70);
            continue;
        }
        const std::string cmd = msg.value("command", "");
        const int seq = msg.value("seq", 0);
        if (cmd == "continue" || cmd == "next" || cmd == "stepIn" || cmd == "stepOut" ||
            cmd == "pause") {
            JsonRpc::writeMessage(out_, makeResponse(seq, cmd,
                {{"error", "program is deadlocked (all threads are blocked)"}}, false));
            continue;
        }
        if (cmd == "terminate" || cmd == "disconnect") {
            JsonRpc::writeMessage(out_, makeResponse(seq, cmd, {}));
            sendEvent("terminated", {});
            out_.flush();
            std::_Exit(70);
        }
        auto resp = dispatch(msg);
        if (!resp.is_null()) JsonRpc::writeMessage(out_, resp);
    }
}

// ── evaluate ─────────────────────────────────────────────────────────────────
// VS Code debug hover'ı ve Debug Console'daki ifadeler buradan geçer.
// Desteklenen ifade biçimi: IDENT ( .IDENT | [SAYI] )*  — ör. a, a.x,
// vecs[0].z. Tam ifade değerlendirme (aritmetik vb.) kapsam dışı.

bool DapHandler::resolveExpression(const std::string& expr, int frameId,
                                   Value& out) const {
    if (!vm_) return false;
    size_t i = 0, n = expr.size();
    auto skipWs = [&]{ while (i < n && (expr[i] == ' ' || expr[i] == '\t')) ++i; };
    auto isIdent = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
    };

    skipWs();
    size_t start = i;
    while (i < n && isIdent(expr[i])) ++i;
    if (i == start) return false;
    std::string name = expr.substr(start, i - start);

    // Kök isim: istenen frame'in slotlarında ara
    if (frameId < 0 || frameId >= vm_->callDepth()) frameId = 0;
    bool found = false;
    int slotCount = vm_->frameSlotCount(frameId);
    for (int slot = 0; slot < slotCount; ++slot) {
        if (vm_->slotName(frameId, slot) == name) {
            out = vm_->readSlotInFrame(frameId, slot);
            found = true;
            break;
        }
    }
    if (!found) return false; // TODO(dap): global değişkenler henüz çözülmüyor

    // Zincir: .alan ve [indeks]
    while (true) {
        skipWs();
        if (i >= n) return true;
        if (expr[i] == '.') {
            ++i; skipWs();
            size_t fs = i;
            while (i < n && isIdent(expr[i])) ++i;
            if (i == fs) return false;
            std::string field = expr.substr(fs, i - fs);
            if (out.kind != ValueKind::Ref || !out.ref() ||
                out.ref()->type != ObjectType::Struct) return false;
            auto* s = static_cast<StructObject*>(out.ref());
            bool hit = false;
            if (s->fieldNames) {
                auto& fn = *s->fieldNames;
                for (size_t f = 0; f < fn.size() && f < s->fields.size(); ++f) {
                    if (fn[f] == field) { out = s->fields[f]; hit = true; break; }
                }
            }
            if (!hit) return false;
        } else if (expr[i] == '[') {
            ++i; skipWs();
            size_t ds = i;
            while (i < n && expr[i] >= '0' && expr[i] <= '9') ++i;
            if (i == ds) return false;
            int idx = std::stoi(expr.substr(ds, i - ds));
            skipWs();
            if (i >= n || expr[i] != ']') return false;
            ++i;
            if (out.kind != ValueKind::Ref || !out.ref() ||
                out.ref()->type != ObjectType::Array) return false;
            auto* a = static_cast<ArrayObject*>(out.ref());
            if (idx < 0 || idx >= (int)a->elements.size()) return false;
            out = a->elements[idx];
        } else {
            return false; // tanınmayan ek — aritmetik vb. desteklenmiyor
        }
    }
}

nlohmann::json DapHandler::handleEvaluate(const nlohmann::json& req) {
    int seq = req.value("seq", 0);
    nlohmann::json args = req.value("arguments", nlohmann::json::object());
    std::string expr    = args.value("expression", "");
    int frameId         = args.value("frameId", 0);

    Value v;
    if (!resolveExpression(expr, frameId, v)) {
        // Hover bağlamında sessizce başarısız ol — VS Code tooltip göstermez.
        return makeResponse(seq, "evaluate",
            {{"result", "ifade çözülemedi: " + expr}}, false);
    }

    return makeResponse(seq, "evaluate", {
        {"result",             valueToString(v)},
        {"type",               ""},
        {"variablesReference", registerVarRef(v)}
    });
}
