// ============================================================================
// saQut FFI — stdin / stdout / stderr host fonksiyonları (#115 Faz 1)
// ============================================================================
//
// Girdi/çıktı akışları. Capability'siz (ADR-043). stdout::write, core_print
// ile aynı stream'e yazar (outputSink DAP yönlendirmesi dahil — #105); stderr
// ayrı hata akışıdır. std::cin/std::cerr from C++ istreambuf.
//
// readBytes/readAll tüm girdiyi belleğe alır (tek atımlık — fs'e benzer
// handle'sız model); streaming/seek ihtiyacı ADR-034 §5 dışıdır.
// ============================================================================

#include <functional>
#include <iostream>
#include <string>
#include "ffi/host_functions.hpp"
#include "ffi/host_bridge.hpp"
#include "runtime/output_lock.hpp"

// ── stdin ───────────────────────────────────────────────────────────────────

static int stdin_readLine(HostCallFrame* f) {
    std::string line;
    if (!std::getline(std::cin, line)) { f->ret = HostSlot::null(); return 0; }
    hostSetRetString(*f, line);
    return 0;
}

static int stdin_readAll(HostCallFrame* f) {
    std::string all((std::istreambuf_iterator<char>(std::cin)),
                    std::istreambuf_iterator<char>());
    hostSetRetString(*f, all);
    return 0;
}

static int stdin_readBytes(HostCallFrame* f) {
    if (!f->env || !f->env->heap) { f->err.set("readBytes: heap yok", "E_HOST"); return 1; }
    std::string all((std::istreambuf_iterator<char>(std::cin)),
                    std::istreambuf_iterator<char>());
    ArrayObject* arr = f->env->heap->allocArray((int)all.size(), ArrayElemKind::Byte);
    arr->bytes.assign(all.begin(), all.end());
    f->ret = HostSlot::fromRef(arr);
    return 0;
}

// ── stdout / stderr ortak yazma gövdesi ─────────────────────────────────────

// metin yazma: byte[] beklenmez; hangi akışa yazılacağı çağırana bağlı.
static int io_write(HostCallFrame* f, std::ostream& os, void* sink) {
    std::string text = fromHostSlot(f->args[0]).toString();
    // ADR-045 §18: yazma çağrı başına atomik (1-f).
    if (sink) {
        std::lock_guard<std::mutex> lock(programOutputMutex());
        (*static_cast<std::function<void(const std::string&)>*>(sink))(text);
    } else {
        writeProgramOutput(os, text);
    }
    f->ret = HostSlot::voidVal();
    return 0;
}

static int io_writeBytes(HostCallFrame* f, std::ostream& os) {
    if (f->args[0].kind != HostKind::Ref || !f->args[0].p) {
        f->err.set("writeBytes: expected byte[]", "E_HOST"); return 1;
    }
    auto* arr = static_cast<ArrayObject*>(f->args[0].p);
    if (arr->elemKind != ArrayElemKind::Byte) {
        f->err.set("writeBytes: expected byte[]", "E_HOST"); return 1;
    }
    writeProgramOutput(os, std::string_view(reinterpret_cast<const char*>(arr->bytes.data()),
                                            arr->bytes.size()));
    f->ret = HostSlot::voidVal();
    return 0;
}

static int stdout_write(HostCallFrame* f) {
    return io_write(f, std::cout, f->env && f->env->outputSink ? f->env->outputSink : nullptr);
}
static int stdout_writeBytes(HostCallFrame* f) {
    return io_writeBytes(f, std::cout);
}
static int stderr_write(HostCallFrame* f) {
    return io_write(f, std::cerr, nullptr);
}
static int stderr_writeBytes(HostCallFrame* f) {
    return io_writeBytes(f, std::cerr);
}

// ── Tablo (stdin / stdout / stderr alt kümesi) ──────────────────────────────
const std::vector<HostFn>& stdinHostFunctions() {
    static const std::vector<HostFn> table = {
        { "STDIN_READ_LINE",  0, HOST_CAN_FAIL, HostKind::Str, stdin_readLine },
        { "STDIN_READ_ALL",   0, HOST_CAN_FAIL, HostKind::Str, stdin_readAll },
        { "STDIN_READ_BYTES", 0, HOST_NEEDS_HEAP | HOST_CAN_FAIL, HostKind::Ref, stdin_readBytes },
    };
    return table;
}

const std::vector<HostFn>& stdoutHostFunctions() {
    static const std::vector<HostFn> table = {
        { "STDOUT_WRITE",      1, 0, HostKind::Void, stdout_write },
        { "STDOUT_WRITE_BYTES",1, HOST_CAN_FAIL, HostKind::Void, stdout_writeBytes },
    };
    return table;
}

const std::vector<HostFn>& stderrHostFunctions() {
    static const std::vector<HostFn> table = {
        { "STDERR_WRITE",      1, 0, HostKind::Void, stderr_write },
        { "STDERR_WRITE_BYTES",1, HOST_CAN_FAIL, HostKind::Void, stderr_writeBytes },
    };
    return table;
}
