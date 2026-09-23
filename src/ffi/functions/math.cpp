// ============================================================================
// saQut FFI — math host fonksiyonları (ADR-034, #107; issue #89)
// ============================================================================
//
// Saf hesap ailesi. Overload YOK → int/float ayrımı isimle
// (abs/absf, min/minf, max/maxf). IEEE754 korunur: sqrt(-1) NaN döner,
// Error FIRLATMAZ (#89).
// ============================================================================

#include <cmath>
#include <cstdlib>
#include "ffi/host_functions.hpp"
#include "ffi/host_bridge.hpp"

// #222: math ailesi NATİF thunk'tır — Value'ya hiç uğramaz.
//
// Bu ailenin sarmalayıcıdan çıkarılması ölçülebilir: sarmalayıcı her çağrıda
// bir std::vector<Value> tahsis eder ve HostSlot→Value→HostSlot çift
// dönüşümü yapar (80 baytlık Value'lar). Natif thunk doğrudan HostSlot okur.
//
// Hepsi HOST_PURE: env istemez, heap'e dokunmaz, hata döndürmez
// (#89: IEEE754 korunur — sqrt(-1) NaN döner, Error FIRLATMAZ).

// #243: std::abs(INT_MIN) C++'ta tanımsız davranıştır (UB). Dilin int
// sözleşmesi ikiye tümleyen sarmadır (overflow hata vermez, sarar), bu yüzden
// negasyon unsigned üzerinden yapılır: abs(INT_MIN) == INT_MIN, tanımlı ve
// VM≡JIT (aynı gövde).
static int math_abs(HostCallFrame* f) {
    const int v = static_cast<int>(hostAsI64(f->args[0]));
    const int r = v < 0 ? static_cast<int>(0u - static_cast<unsigned>(v)) : v;
    f->ret = HostSlot::fromInt(r);
    return 0;
}
static int math_absf(HostCallFrame* f) {
    f->ret = HostSlot::fromFloat(std::fabs(hostAsDouble(f->args[0])));
    return 0;
}
static int math_min(HostCallFrame* f) {
    f->ret = HostSlot::fromInt(static_cast<int>(
        std::min(hostAsI64(f->args[0]), hostAsI64(f->args[1]))));
    return 0;
}
static int math_max(HostCallFrame* f) {
    f->ret = HostSlot::fromInt(static_cast<int>(
        std::max(hostAsI64(f->args[0]), hostAsI64(f->args[1]))));
    return 0;
}
static int math_minf(HostCallFrame* f) {
    f->ret = HostSlot::fromFloat(std::fmin(hostAsDouble(f->args[0]), hostAsDouble(f->args[1])));
    return 0;
}
static int math_maxf(HostCallFrame* f) {
    f->ret = HostSlot::fromFloat(std::fmax(hostAsDouble(f->args[0]), hostAsDouble(f->args[1])));
    return 0;
}
static int math_sqrt(HostCallFrame* f) {
    f->ret = HostSlot::fromFloat(std::sqrt(hostAsDouble(f->args[0])));  // sqrt(-1) → NaN
    return 0;
}
static int math_pow(HostCallFrame* f) {
    f->ret = HostSlot::fromFloat(std::pow(hostAsDouble(f->args[0]), hostAsDouble(f->args[1])));
    return 0;
}
static int math_floor(HostCallFrame* f) {
    f->ret = HostSlot::fromFloat(std::floor(hostAsDouble(f->args[0])));
    return 0;
}
static int math_ceil(HostCallFrame* f) {
    f->ret = HostSlot::fromFloat(std::ceil(hostAsDouble(f->args[0])));
    return 0;
}
static int math_round(HostCallFrame* f) {
    f->ret = HostSlot::fromFloat(std::round(hostAsDouble(f->args[0])));
    return 0;
}
// #89: sabit yok — ffi bildirimi yalnızca fonksiyon; PI/E sıfır-argümanlı
// saf fonksiyon olarak sunulur (import {PI, E} from math; PI();).
static int math_PI(HostCallFrame* f) {
    f->ret = HostSlot::fromFloat(3.14159265358979323846);
    return 0;
}
static int math_E(HostCallFrame* f) {
    f->ret = HostSlot::fromFloat(2.71828182845904523536);
    return 0;
}

// ── Tablo (math alt kümesi) ─────────────────────────────────────────────────
const std::vector<HostFn>& mathHostFunctions() {
    static const std::vector<HostFn> table = {
        { "MATH_ABS", 1, HOST_PURE, HostKind::Int, math_abs },
        { "MATH_ABSF", 1, HOST_PURE, HostKind::Float, math_absf },
        { "MATH_MIN", 2, HOST_PURE, HostKind::Int, math_min },
        { "MATH_MAX", 2, HOST_PURE, HostKind::Int, math_max },
        { "MATH_MINF", 2, HOST_PURE, HostKind::Float, math_minf },
        { "MATH_MAXF", 2, HOST_PURE, HostKind::Float, math_maxf },
        { "MATH_SQRT", 1, HOST_PURE, HostKind::Float, math_sqrt },
        { "MATH_POW", 2, HOST_PURE, HostKind::Float, math_pow },
        { "MATH_FLOOR", 1, HOST_PURE, HostKind::Float, math_floor },
        { "MATH_CEIL", 1, HOST_PURE, HostKind::Float, math_ceil },
        { "MATH_ROUND", 1, HOST_PURE, HostKind::Float, math_round },
        { "MATH_PI", 0, HOST_PURE, HostKind::Float, math_PI },
        { "MATH_E", 0, HOST_PURE, HostKind::Float, math_E },
    };
    return table;
}
