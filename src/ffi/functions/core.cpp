// ============================================================================
// saQut FFI — core host fonksiyonları
// ============================================================================
//
// #227: print — registry'de sıradan bir kayıt. Öncesinde CALLHOST'un üçüncü
// dalıydı (functionName == "print" → executeHostFunction) ve JIT'te tek özel
// durumdu. Artık diğer host fonksiyonlarıyla aynı yoldan geçer.
//
// Çıktı yönlendirmesi (DAP modu, #105) env->outputSink üzerinden gelir;
// bağlı değilse doğrudan stdout.
//
// "2026-07-12T10:00:00Z" — v1 yalnızca UTC (ADR-035); başka format → null.
// pattern alt kümesi: yyyy MM dd HH mm ss (ADR-035'te sabitlenir).
// ============================================================================

#include <functional>
#include <iostream>
#include "ffi/host_functions.hpp"
#include "ffi/host_bridge.hpp"
#include "runtime/output_lock.hpp"

static int core_print(HostCallFrame* f) {
    if (f->argc < 1) { f->ret = HostSlot::voidVal(); return 0; }
    std::string text = fromHostSlot(f->args[0]).toString();
    // ADR-045 §18: print çağrı başına atomik (1-f).
    if (f->env && f->env->outputSink) {
        std::lock_guard<std::mutex> lock(programOutputMutex());
        (*static_cast<std::function<void(const std::string&)>*>(f->env->outputSink))(text);
    } else {
        writeProgramOutput(std::cout, text);
    }
    f->ret = HostSlot::voidVal();
    return 0;
}

// core — derleyici sürümü (SAQUT_VERSION derleme zamanında gömülür)
static int core_version(HostCallFrame* f) {
    hostSetRetString(*f, SAQUT_VERSION);
    return 0;
}

// ── Tablo (core alt kümesi) ─────────────────────────────────────────────────
const std::vector<HostFn>& coreHostFunctions() {
    static const std::vector<HostFn> table = {
        { "CORE_VERSION", 0, HOST_PURE, HostKind::Str, core_version },
        { "CORE_PRINT", 1, HOST_PURE, HostKind::Void, core_print },
    };
    return table;
}
