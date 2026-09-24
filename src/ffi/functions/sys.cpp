// ============================================================================
// saQut FFI — sys host fonksiyonları (#90)
// ============================================================================
//
// Non-deterministik/dış-durum-okuyan aile. Kaynak: OS CSPRNG
// (std::random_device), rand() DEĞİL. Gövde double üretir (#227: bildirim ile
// gövde arasındaki tip farkı kayıtlıdır — JIT bu çağrıyı VM ile aynı çıktıyı
// vererek çalıştırır; tip düzeltmesi ayrı iştir).
// ============================================================================

#include <chrono>
#include <random>
#include <thread>
#include "ffi/host_functions.hpp"
#include "ffi/host_bridge.hpp"
#include "runtime/isolate.hpp"

// RNG durumu Isolate üyesidir (ADR-045, Faz 1): mt19937_64 sırayı korumaz,
// iki thread ayni anda çekerse yarış olur ve seri bozulur. Isolate ile her
// iş parçacığı kendi üretecini kurar — tek thread'de davranış birebir aynı
// (tohum random_device'ten, çağrı başına değil üretim başına alınır).
static std::mt19937_64& sysRng() {
    return Isolate::current().rng;
}

static int sys_random(HostCallFrame* f) {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    // Eski davranış Value::fromFloat (double) — root.sqt `float` yazsa da
    // gözlemlenen çıktı double biçimidir, birebir korunur.
    f->ret = HostSlot::fromFloat(dist(sysRng()));
    return 0;
}

static int sys_randomInt(HostCallFrame* f) {
    int lo = (int)hostAsI64(f->args[0]), hi = (int)hostAsI64(f->args[1]);
    if (lo >= hi) {
        f->err.set("randomInt: invalid range [" + std::to_string(lo) +
                   ", " + std::to_string(hi) + ")", "E_HOST");
        return 1;
    }
    std::uniform_int_distribution<int> dist(lo, hi - 1);
    f->ret = HostSlot::fromInt(dist(sysRng()));
    return 0;
}

static int sys_env(HostCallFrame* f) {
    const char* v = std::getenv(hostAsString(f->args[0]).c_str());
    // Tanımsız değişken null döner (root.sqt: `string?`) — hata DEĞİL.
    if (!v) { f->ret = HostSlot::null(); return 0; }
    hostSetRetString(*f, std::string(v));
    return 0;
}

static int sys_sleep(HostCallFrame* f) {
    std::this_thread::sleep_for(std::chrono::milliseconds(hostAsI64(f->args[0])));
    f->ret = HostSlot::voidVal();
    return 0;
}

static int sys_args(HostCallFrame* f) {
    if (!f->env || !f->env->heap) { f->err.set("args: heap yok", "E_HOST"); return 1; }
    // Eski kod burada `ctx.programArgs ? *ctx.programArgs : std::vector{}`
    // yazıyordu — programArgs null iken GEÇİCİ bir vector'e referans bağlayan
    // sarkan referanstı. Boş tablo doğrudan ele alınır.
    static const std::vector<std::string> kNoArgs;
    const auto& args = f->env->programArgs ? *f->env->programArgs : kNoArgs;
    ArrayObject* arr = f->env->heap->allocArray((int)args.size());
    for (const auto& s : args)
        arr->elements.push_back(Value::fromString(s));
    f->ret = HostSlot::fromRef(arr);
    return 0;
}

// ── Tablo (sys alt kümesi) ──────────────────────────────────────────────────
const std::vector<HostFn>& sysHostFunctions() {
    static const std::vector<HostFn> table = {
        { "SYS_RANDOM",     0, 0, HostKind::Float, sys_random },
        { "SYS_RANDOM_INT", 2, HOST_CAN_FAIL, HostKind::Int, sys_randomInt },
        { "SYS_ENV",        1, 0, HostKind::Str, sys_env },
        { "SYS_SLEEP",      1, 0, HostKind::Void, sys_sleep },
        { "SYS_ARGS", 0, HOST_NEEDS_HEAP | HOST_NEEDS_ARGS, HostKind::Ref, sys_args },
    };
    return table;
}
