// ============================================================================
// saQut FFI — net + tls host fonksiyonları
// ============================================================================
//
// İnce thunk katmanı: HostSlot ↔ C++ dönüşümü yapar, işi
// src/net/net_runtime.cpp'ye devreder. Soket/dinleyici/TLS bağlamı dile int
// handle olarak gider (fs'teki yol-string modelinin karşılığı: C++ nesnesi
// host'ta, dilde yalnız numara).
//
// Okuma dönüş sözleşmesi (string?/byte[]?):
//   null → şimdilik veri yok (pollSockets'i bekle)
//   ""   → bağlantı kapandı (EOF / kopma / TLS el sıkışma hatası) → closeSocket
//   dolu → veri
// Hata kodu: E_NET (geçersiz handle, bind hatası, çerçeve sınırı aşımı, TLS
// yapılandırması). Kopan bağlantı hata DEĞİLDİR; EOF olarak görünür.
// ============================================================================

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "ffi/host_bridge.hpp"
#include "ffi/host_functions.hpp"
#include "net/net_runtime.hpp"

namespace net = saqut::net;

static int netFail(HostCallFrame* f, const std::string& err) {
    f->err.set(err, "E_NET");
    return 1;
}

static int argInt(HostCallFrame* f, int i) { return (int)hostAsI64(f->args[i]); }

// Okuma sonucunu string? olarak döndürür.
static int retRead(HostCallFrame* f, net::ReadStatus st, std::string& data,
                   const std::string& err) {
    switch (st) {
        case net::ReadStatus::Data:  hostSetRetString(*f, std::move(data)); return 0;
        case net::ReadStatus::Eof:   hostSetRetString(*f, std::string());   return 0;
        case net::ReadStatus::None:  f->ret = HostSlot::null();             return 0;
        case net::ReadStatus::Error: break;
    }
    return netFail(f, err);
}

// ── net ─────────────────────────────────────────────────────────────────────

static int net_listenTcp(HostCallFrame* f) {
    std::string err;
    const int id = net::listenTcp(hostAsString(f->args[0]), argInt(f, 1),
                                  hostAsI64(f->args[2]) != 0, 0, err);
    if (id < 0) return netFail(f, err);
    f->ret = HostSlot::fromInt(id);
    return 0;
}

static int net_acceptSocket(HostCallFrame* f) {
    std::string err;
    const int id = net::acceptSocket(argInt(f, 0), err);
    if (id < 0) return netFail(f, err);
    f->ret = id == 0 ? HostSlot::null() : HostSlot::fromInt(id);
    return 0;
}

static int net_pollSockets(HostCallFrame* f) {
    if (!f->env || !f->env->heap) return netFail(f, "pollSockets: heap yok");
    thread_local std::vector<int> ready;
    ready.clear();
    std::string err;
    if (!net::pollSockets(argInt(f, 0), ready, err)) return netFail(f, err);
    ArrayObject* arr = f->env->heap->allocArray((int)ready.size(), ArrayElemKind::Int);
    arr->ints.assign(ready.begin(), ready.end());
    arr->syncJitView();
    f->ret = HostSlot::fromRef(arr);
    return 0;
}

static int net_readSocket(HostCallFrame* f) {
    std::string data, err;
    const int max = argInt(f, 1);
    if (max <= 0) return netFail(f, "readSocket: maxBytes must be positive");
    return retRead(f, net::readSocket(argInt(f, 0), (size_t)max, data, err), data, err);
}

static int net_readSocketBytes(HostCallFrame* f) {
    if (!f->env || !f->env->heap) return netFail(f, "readSocketBytes: heap yok");
    std::string data, err;
    const int max = argInt(f, 1);
    if (max <= 0) return netFail(f, "readSocketBytes: maxBytes must be positive");
    const net::ReadStatus st = net::readSocket(argInt(f, 0), (size_t)max, data, err);
    if (st == net::ReadStatus::Error) return netFail(f, err);
    if (st == net::ReadStatus::None) { f->ret = HostSlot::null(); return 0; }
    ArrayObject* arr = f->env->heap->allocArray((int)data.size(), ArrayElemKind::Byte);
    arr->bytes.assign(data.begin(), data.end());   // Eof → boş dizi
    arr->syncJitView();
    f->ret = HostSlot::fromRef(arr);
    return 0;
}

static int net_readSocketUntil(HostCallFrame* f) {
    std::string data, err;
    const int max = argInt(f, 2);
    if (max <= 0) return netFail(f, "readSocketUntil: maxBytes must be positive");
    return retRead(f, net::readSocketUntil(argInt(f, 0), hostAsString(f->args[1]),
                                           (size_t)max, data, err), data, err);
}

static int net_readSocketExact(HostCallFrame* f) {
    std::string data, err;
    const int size = argInt(f, 1);
    if (size < 0) return netFail(f, "readSocketExact: size must not be negative");
    return retRead(f, net::readSocketExact(argInt(f, 0), (size_t)size, data, err), data, err);
}

static int net_writeSocket(HostCallFrame* f) {
    std::string err;
    const std::string& s = hostAsString(f->args[1]);
    if (!net::writeSocket(argInt(f, 0), s.data(), s.size(), err)) return netFail(f, err);
    f->ret = HostSlot::voidVal();
    return 0;
}

static int net_writeSocketBytes(HostCallFrame* f) {
    if (f->args[1].kind != HostKind::Ref || !f->args[1].p)
        return netFail(f, "writeSocketBytes: expected byte[]");
    auto* arr = static_cast<ArrayObject*>(f->args[1].p);
    if (arr->elemKind != ArrayElemKind::Byte) return netFail(f, "writeSocketBytes: expected byte[]");
    std::string err;
    if (!net::writeSocket(argInt(f, 0), reinterpret_cast<const char*>(arr->bytes.data()),
                          arr->bytes.size(), err))
        return netFail(f, err);
    f->ret = HostSlot::voidVal();
    return 0;
}

static int net_closeSocket(HostCallFrame* f) {
    std::string err;
    if (!net::closeSocket(argInt(f, 0), err)) return netFail(f, err);
    f->ret = HostSlot::voidVal();
    return 0;
}

static int net_socketPort(HostCallFrame* f) {
    std::string err;
    const int port = net::socketPort(argInt(f, 0), err);
    if (port < 0) return netFail(f, err);
    f->ret = HostSlot::fromInt(port);
    return 0;
}

static int net_socketPeer(HostCallFrame* f) {
    std::string out, err;
    if (!net::socketPeer(argInt(f, 0), out, err)) return netFail(f, err);
    hostSetRetString(*f, std::move(out));
    return 0;
}

static int net_socketPending(HostCallFrame* f) {
    std::string err;
    const long n = net::socketPending(argInt(f, 0), err);
    if (n < 0) return netFail(f, err);
    f->ret = HostSlot::fromInt((int)std::min<long>(n, INT32_MAX));
    return 0;
}

// ── tls ─────────────────────────────────────────────────────────────────────

static int tls_serverContext(HostCallFrame* f) {
    std::string err;
    const int id = net::tlsServerContext(hostAsString(f->args[0]), hostAsString(f->args[1]), err);
    if (id < 0) return netFail(f, err);
    f->ret = HostSlot::fromInt(id);
    return 0;
}

static int tls_listenTls(HostCallFrame* f) {
    std::string err;
    const int ctx = argInt(f, 3);
    if (ctx <= 0) return netFail(f, "listenTls: invalid tls context handle");
    const int id = net::listenTcp(hostAsString(f->args[0]), argInt(f, 1),
                                  hostAsI64(f->args[2]) != 0, ctx, err);
    if (id < 0) return netFail(f, err);
    f->ret = HostSlot::fromInt(id);
    return 0;
}

const std::vector<HostFn>& netHostFunctions() {
    static const std::vector<HostFn> t = {
        { "NET_LISTEN_TCP",   3, HOST_CAN_FAIL,                   HostKind::Int,  net_listenTcp },
        { "NET_ACCEPT",       1, HOST_CAN_FAIL,                   HostKind::Int,  net_acceptSocket },
        { "NET_POLL",         1, HOST_NEEDS_HEAP | HOST_CAN_FAIL, HostKind::Ref,  net_pollSockets },
        { "NET_READ",         2, HOST_CAN_FAIL,                   HostKind::Str,  net_readSocket },
        { "NET_READ_BYTES",   2, HOST_NEEDS_HEAP | HOST_CAN_FAIL, HostKind::Ref,  net_readSocketBytes },
        { "NET_READ_UNTIL",   3, HOST_CAN_FAIL,                   HostKind::Str,  net_readSocketUntil },
        { "NET_READ_EXACT",   2, HOST_CAN_FAIL,                   HostKind::Str,  net_readSocketExact },
        { "NET_WRITE",        2, HOST_CAN_FAIL,                   HostKind::Void, net_writeSocket },
        { "NET_WRITE_BYTES",  2, HOST_CAN_FAIL,                   HostKind::Void, net_writeSocketBytes },
        { "NET_CLOSE",        1, HOST_CAN_FAIL,                   HostKind::Void, net_closeSocket },
        { "NET_PORT",         1, HOST_CAN_FAIL,                   HostKind::Int,  net_socketPort },
        { "NET_PEER",         1, HOST_CAN_FAIL,                   HostKind::Str,  net_socketPeer },
        { "NET_PENDING",      1, HOST_CAN_FAIL,                   HostKind::Int,  net_socketPending },
    };
    return t;
}

const std::vector<HostFn>& tlsHostFunctions() {
    static const std::vector<HostFn> t = {
        { "TLS_SERVER_CONTEXT", 2, HOST_CAN_FAIL, HostKind::Int, tls_serverContext },
        { "TLS_LISTEN",         4, HOST_CAN_FAIL, HostKind::Int, tls_listenTls },
    };
    return t;
}
