// ============================================================================
// saQut — TCP/TLS çalışma zamanı (gerçekleme)
// ============================================================================
//
// Mimari özet için net_runtime.hpp başlığına bak. Bu dosyadaki ana parçalar:
//   Table  — süreç-global id → Sock tablosu (mutex korumalı).
//   Loop   — thread başına epoll örneği + kapanmakta olan bağlantılar.
//   fill   — çekirdekten/SSL'den giriş tamponuna okuma (non-blocking).
//   flush  — çıkış tamponunu sokete/SSL'e yazma (non-blocking).
//   poll   — epoll olaylarını iç işlere (flush, el sıkışma, kapanış) ve
//            kullanıcıya bildirilecek id'lere ayırma.
//
// Tetikleme seviye tabanlıdır (level-triggered): okunmadan kalan çekirdek
// verisi bir sonraki poll'da yeniden bildirilir; bu yüzden fill bir üst
// sınırda durabilir.
// ============================================================================

#include "net/net_runtime.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace saqut::net {
namespace {

using Clock = std::chrono::steady_clock;

// Kapanış süreleri: çıkış tamponunu boşaltma ve karşı tarafın FIN'ini bekleme.
constexpr auto kFlushTimeout = std::chrono::seconds(10);
constexpr auto kDrainTimeout = std::chrono::seconds(2);
constexpr size_t kReadChunk  = 64 * 1024;
constexpr int    kMaxEvents  = 256;

enum class Kind : uint8_t { Listener, Conn, TlsContext };
enum class CloseState : uint8_t { Open, Flushing, Draining };

struct TlsCtx {
    SSL_CTX* ctx = nullptr;
    ~TlsCtx() { if (ctx) SSL_CTX_free(ctx); }
};

struct Loop;

struct Sock {
    int   id   = 0;
    Kind  kind = Kind::Conn;
    int   fd   = -1;
    Loop* loop = nullptr;

    // Listener: kabul edilen bağlantılar bu bağlamla TLS olur.
    // TlsContext handle'ı: bağlamın kendisi.
    std::shared_ptr<TlsCtx> tls;

    // Conn
    SSL*   ssl         = nullptr;
    bool   handshaking = false;
    bool   eof         = false;   // karşı taraf kapattı / bağlantı koptu
    bool   wantWrite   = false;   // EPOLLOUT kayıtlı mı
    int    tlsRetryLen = 0;       // WANT_* sonrası SSL_write aynı uzunlukla tekrarlanmalı
    std::string in;               // giriş tamponu; okunmuş kısım [0, inPos)
    size_t inPos   = 0;
    size_t scanPos = 0;           // readSocketUntil ayraç aramasının devam noktası
    std::string out;              // çıkış tamponu; gönderilmiş kısım [0, outPos)
    size_t outPos  = 0;
    sockaddr_storage peer{};

    CloseState        closeState = CloseState::Open;
    Clock::time_point deadline{};

    size_t avail() const { return in.size() - inPos; }
    size_t pending() const { return out.size() - outPos; }

    ~Sock() {
        if (ssl) SSL_free(ssl);
        if (fd >= 0) ::close(fd);
    }
};

// ── Thread başına olay döngüsü ──────────────────────────────────────────────

struct Loop {
    int epfd = -1;
    // Kullanıcının kapattığı ama henüz boşalmamış/kapanmamış bağlantılar.
    std::vector<std::unique_ptr<Sock>> closing;

    Loop() { epfd = ::epoll_create1(EPOLL_CLOEXEC); }
    ~Loop() {
        closing.clear();
        if (epfd >= 0) ::close(epfd);
    }
};

Loop& currentLoop() {
    thread_local Loop loop;
    return loop;
}

bool epollAdd(Sock* s, uint32_t events) {
    epoll_event ev{};
    ev.events   = events;
    ev.data.ptr = s;
    return ::epoll_ctl(s->loop->epfd, EPOLL_CTL_ADD, s->fd, &ev) == 0;
}

void setWantWrite(Sock* s, bool on) {
    if (s->wantWrite == on || s->fd < 0) return;
    epoll_event ev{};
    ev.events   = EPOLLIN | EPOLLRDHUP | (on ? EPOLLOUT : 0u);
    ev.data.ptr = s;
    ::epoll_ctl(s->loop->epfd, EPOLL_CTL_MOD, s->fd, &ev);
    s->wantWrite = on;
}

void destroy(std::unique_ptr<Sock>& s) {
    if (s->fd >= 0 && s->loop) ::epoll_ctl(s->loop->epfd, EPOLL_CTL_DEL, s->fd, nullptr);
    s.reset();
}

// ── Handle tablosu ──────────────────────────────────────────────────────────

struct Table {
    std::mutex mu;
    std::unordered_map<int, std::unique_ptr<Sock>> map;
    int next = 1;
};

// Bilerek yıkılmaz: OpenSSL kendi temizliğini atexit ile kaydeder; tablo
// OpenSSL'den önce kurulduysa statik yıkıcısı OPENSSL_cleanup'tan SONRA
// koşar ve SSL_free temizlenmiş kütüphaneyi çağırır. Süreç sonunda fd'leri
// çekirdek kapatır.
Table& table() {
    static Table* t = new Table;
    return *t;
}

int insert(std::unique_ptr<Sock> s) {
    Table& t = table();
    std::lock_guard<std::mutex> lock(t.mu);
    // Taşmada 1'e sar ve canlı id'leri atla (2^31 handle sonra).
    while (t.map.count(t.next)) t.next = (t.next == INT_MAX) ? 1 : t.next + 1;
    const int id = t.next;
    t.next = (t.next == INT_MAX) ? 1 : t.next + 1;
    s->id = id;
    t.map.emplace(id, std::move(s));
    return id;
}

Sock* find(int id) {
    Table& t = table();
    std::lock_guard<std::mutex> lock(t.mu);
    auto it = t.map.find(id);
    return it == t.map.end() ? nullptr : it->second.get();
}

std::unique_ptr<Sock> take(int id) {
    Table& t = table();
    std::lock_guard<std::mutex> lock(t.mu);
    auto it = t.map.find(id);
    if (it == t.map.end()) return nullptr;
    std::unique_ptr<Sock> s = std::move(it->second);
    t.map.erase(it);
    return s;
}

Sock* findKind(int id, Kind kind, std::string& err) {
    Sock* s = find(id);
    if (!s) { err = "invalid socket handle " + std::to_string(id); return nullptr; }
    if (s->kind != kind) {
        static const char* names[] = {"listener", "connection", "tls context"};
        err = "handle " + std::to_string(id) + " is not a " + names[(int)kind];
        return nullptr;
    }
    return s;
}

// ── Süreç geneli başlatma ───────────────────────────────────────────────────

void netInit() {
    static std::once_flag once;
    std::call_once(once, [] { std::signal(SIGPIPE, SIG_IGN); });
}

void sslInit() {
    static std::once_flag once;
    std::call_once(once, [] {
        OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr);
    });
}

std::string sslErrorString() {
    unsigned long e = ERR_get_error();
    ERR_clear_error();
    if (e == 0) return "unknown TLS error";
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    return buf;
}

// ── Giriş: çekirdek/SSL → giriş tamponu ─────────────────────────────────────

void compactIn(Sock* s) {
    if (s->inPos == 0) return;
    if (s->inPos < s->in.size() / 2 && s->inPos < kReadChunk) return;
    s->in.erase(0, s->inPos);
    s->scanPos = s->scanPos > s->inPos ? s->scanPos - s->inPos : 0;
    s->inPos = 0;
}

void flush(Sock* s);

// El sıkışmasını ilerletir. true: tamamlandı (ya da zaten tamamdı).
bool driveHandshake(Sock* s) {
    if (!s->handshaking) return true;
    const int r = SSL_do_handshake(s->ssl);
    if (r == 1) {
        s->handshaking = false;
        if (s->pending() > 0) flush(s);
        else setWantWrite(s, false);
        return true;
    }
    switch (SSL_get_error(s->ssl, r)) {
        case SSL_ERROR_WANT_READ:  setWantWrite(s, false); return false;
        case SSL_ERROR_WANT_WRITE: setWantWrite(s, true);  return false;
        default:
            ERR_clear_error();
            s->eof = true;
            return false;
    }
}

// Tamponda en az `cap` bayt olana, çekirdek boşalana ya da EOF'a dek okur.
void fill(Sock* s, size_t cap) {
    if (s->eof) return;
    thread_local char chunk[kReadChunk];
    compactIn(s);

    if (s->ssl) {
        if (!driveHandshake(s)) return;
        while (s->avail() < cap) {
            const int n = SSL_read(s->ssl, chunk, (int)sizeof(chunk));
            if (n > 0) { s->in.append(chunk, (size_t)n); continue; }
            const int e = SSL_get_error(s->ssl, n);
            if (e == SSL_ERROR_WANT_READ) break;
            if (e == SSL_ERROR_WANT_WRITE) { setWantWrite(s, true); break; }
            // ZERO_RETURN (close_notify) ya da gerçek hata: bağlantı bitti.
            ERR_clear_error();
            s->eof = true;
            break;
        }
        return;
    }

    while (s->avail() < cap) {
        const ssize_t n = ::recv(s->fd, chunk, sizeof(chunk), 0);
        if (n > 0) {
            s->in.append(chunk, (size_t)n);
            // Kısa okuma: çekirdek büyük olasılıkla boşaldı. EAGAIN için ayrı
            // bir sistem çağrısı yapma; kalan veri varsa epoll yeniden bildirir.
            if ((size_t)n < sizeof(chunk)) break;
            continue;
        }
        if (n == 0) { s->eof = true; break; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        s->eof = true;   // ECONNRESET vb. — okuyan için kapanmış bağlantı
        break;
    }
}

// ── Çıkış: çıkış tamponu → soket/SSL ────────────────────────────────────────

void compactOut(Sock* s) {
    if (s->outPos == s->out.size()) { s->out.clear(); s->outPos = 0; return; }
    if (s->outPos >= kReadChunk && s->outPos >= s->out.size() / 2) {
        s->out.erase(0, s->outPos);
        s->outPos = 0;
    }
}

void flush(Sock* s) {
    if (s->eof && s->closeState == CloseState::Open) {
        // Kopmuş bağlantıya yazılamaz; veriyi at (okuyan taraf Eof görür).
        s->out.clear(); s->outPos = 0; s->tlsRetryLen = 0;
        return;
    }
    if (s->ssl && s->handshaking) return;   // el sıkışma bitince boşaltılır

    while (s->pending() > 0) {
        const char* p   = s->out.data() + s->outPos;
        const size_t len = s->pending();
        if (s->ssl) {
            const int want = s->tlsRetryLen > 0 ? s->tlsRetryLen
                                                : (int)std::min<size_t>(len, INT_MAX);
            const int n = SSL_write(s->ssl, p, want);
            if (n > 0) { s->outPos += (size_t)n; s->tlsRetryLen = 0; continue; }
            const int e = SSL_get_error(s->ssl, n);
            if (e == SSL_ERROR_WANT_WRITE) { s->tlsRetryLen = want; setWantWrite(s, true); return; }
            if (e == SSL_ERROR_WANT_READ)  { s->tlsRetryLen = want; return; }
            ERR_clear_error();
        } else {
            const ssize_t n = ::send(s->fd, p, len, MSG_NOSIGNAL);
            if (n > 0) { s->outPos += (size_t)n; continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { setWantWrite(s, true); return; }
        }
        // Yazma hatası (EPIPE/ECONNRESET): bağlantı koptu.
        s->eof = true;
        s->out.clear(); s->outPos = 0; s->tlsRetryLen = 0;
        setWantWrite(s, false);
        return;
    }
    compactOut(s);
    setWantWrite(s, false);
}

// ── Kapanış ─────────────────────────────────────────────────────────────────

// RFC 9112 §9.6: yazma yönünü kapat, karşı tarafın FIN'ini okuyup atarak bekle.
// Doğrudan close() istemcinin henüz okunmamış verisi varken RST üretir ve
// istemci yanıtı okuyamadan bağlantıyı kaybeder.
void startDrain(Sock* s) {
    if (s->ssl && !s->handshaking && !s->eof) {
        SSL_shutdown(s->ssl);   // close_notify; non-blocking, sonuç önemsiz
        ERR_clear_error();
    }
    ::shutdown(s->fd, SHUT_WR);
    s->closeState = CloseState::Draining;
    s->deadline   = Clock::now() + kDrainTimeout;
    setWantWrite(s, false);
}

// true: yok edilebilir.
bool drain(Sock* s) {
    thread_local char sink[4096];
    for (;;) {
        const ssize_t n = ::recv(s->fd, sink, sizeof(sink), 0);
        if (n > 0) continue;
        if (n == 0) return true;
        if (errno == EINTR) continue;
        return !(errno == EAGAIN || errno == EWOULDBLOCK);
    }
}

// Kapanmakta olan bağlantıda bir olay. true: yok edilebilir.
bool progressClosing(Sock* s) {
    if (s->closeState == CloseState::Flushing) {
        flush(s);
        if (s->eof) return true;
        if (s->pending() > 0) return false;
        startDrain(s);
    }
    return drain(s);
}

// ── Adres yardımcıları ──────────────────────────────────────────────────────

bool parseHost(const std::string& host, int port, sockaddr_storage& ss,
               socklen_t& len, std::string& err) {
    std::memset(&ss, 0, sizeof(ss));
    std::string h = host;
    if (h.empty() || h == "*") h = "0.0.0.0";
    if (h == "localhost") h = "127.0.0.1";

    auto* v4 = reinterpret_cast<sockaddr_in*>(&ss);
    if (::inet_pton(AF_INET, h.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port   = htons((uint16_t)port);
        len = sizeof(sockaddr_in);
        return true;
    }
    auto* v6 = reinterpret_cast<sockaddr_in6*>(&ss);
    if (::inet_pton(AF_INET6, h.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port   = htons((uint16_t)port);
        len = sizeof(sockaddr_in6);
        return true;
    }
    err = "invalid listen address '" + host + "' (numeric IPv4/IPv6 or localhost expected)";
    return false;
}

std::string formatAddr(const sockaddr_storage& ss) {
    char buf[INET6_ADDRSTRLEN] = {};
    if (ss.ss_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
        ::inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
        return std::string(buf) + ":" + std::to_string(ntohs(a->sin_port));
    }
    if (ss.ss_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
        ::inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
        return "[" + std::string(buf) + "]:" + std::to_string(ntohs(a->sin6_port));
    }
    return "";
}

std::string errnoText(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

} // namespace

// ============================================================================
// Genel API
// ============================================================================

int listenTcp(const std::string& host, int port, bool reusePort, int tlsContext,
              std::string& err) {
    netInit();
    if (port < 0 || port > 65535) { err = "port out of range: " + std::to_string(port); return -1; }

    std::shared_ptr<TlsCtx> tls;
    if (tlsContext != 0) {
        Sock* c = findKind(tlsContext, Kind::TlsContext, err);
        if (!c) return -1;
        tls = c->tls;
    }

    sockaddr_storage ss;
    socklen_t len = 0;
    if (!parseHost(host, port, ss, len, err)) return -1;

    const int fd = ::socket(ss.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { err = errnoText("socket"); return -1; }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (reusePort) ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&ss), len) < 0) {
        err = errnoText(("bind " + host + ":" + std::to_string(port)).c_str());
        ::close(fd);
        return -1;
    }
    if (::listen(fd, SOMAXCONN) < 0) { err = errnoText("listen"); ::close(fd); return -1; }

    auto s  = std::make_unique<Sock>();
    s->kind = Kind::Listener;
    s->fd   = fd;
    s->loop = &currentLoop();
    s->tls  = std::move(tls);
    if (!epollAdd(s.get(), EPOLLIN)) { err = errnoText("epoll_ctl"); return -1; }
    return insert(std::move(s));
}

int acceptSocket(int listener, std::string& err) {
    Sock* l = findKind(listener, Kind::Listener, err);
    if (!l) return -1;

    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    int fd;
    do {
        fd = ::accept4(l->fd, reinterpret_cast<sockaddr*>(&ss), &len,
                       SOCK_NONBLOCK | SOCK_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        // Bekleyen yok ya da istemci kabulden önce vazgeçti: hata değil.
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) return 0;
        err = errnoText("accept");
        return -1;
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    auto s  = std::make_unique<Sock>();
    s->kind = Kind::Conn;
    s->fd   = fd;
    s->loop = &currentLoop();
    s->peer = ss;
    if (l->tls) {
        s->ssl = SSL_new(l->tls->ctx);
        if (!s->ssl || SSL_set_fd(s->ssl, fd) != 1) {
            err = "TLS: " + sslErrorString();
            return -1;
        }
        SSL_set_accept_state(s->ssl);
        s->handshaking = true;
    }
    if (!epollAdd(s.get(), EPOLLIN | EPOLLRDHUP)) { err = errnoText("epoll_ctl"); return -1; }
    return insert(std::move(s));
}

bool pollSockets(int timeoutMs, std::vector<int>& ready, std::string& err) {
    Loop& L = currentLoop();
    if (L.epfd < 0) { err = "epoll unavailable"; return false; }

    // Kapanmakta olan bağlantıların süresi dolacaksa bekleme ona göre kısalır.
    int wait = timeoutMs;
    if (!L.closing.empty()) {
        const auto now = Clock::now();
        auto nearest = L.closing.front()->deadline;
        for (const auto& c : L.closing) nearest = std::min(nearest, c->deadline);
        const long ms = std::max<long>(0, (long)std::chrono::duration_cast<
                                            std::chrono::milliseconds>(nearest - now).count() + 1);
        if (wait < 0 || ms < wait) wait = (int)std::min<long>(ms, INT_MAX);
    }

    epoll_event evs[kMaxEvents];
    int n = ::epoll_wait(L.epfd, evs, kMaxEvents, wait);
    if (n < 0) {
        if (errno != EINTR) { err = errnoText("epoll_wait"); return false; }
        n = 0;
    }

    for (int i = 0; i < n; ++i) {
        Sock* s = static_cast<Sock*>(evs[i].data.ptr);
        const uint32_t e = evs[i].events;

        if (s->closeState != CloseState::Open) {
            // Bitti: süresini geçmişe çek, aşağıdaki süpürme yok eder.
            if (progressClosing(s)) s->deadline = Clock::time_point::min();
            continue;
        }
        if (s->kind == Kind::Listener) { ready.push_back(s->id); continue; }

        // İç işler: bekleyen çıkışı boşalt, TLS el sıkışmasını ilerlet.
        if ((e & EPOLLOUT) || s->tlsRetryLen > 0) flush(s);
        bool handshakeFinished = false;
        if (s->handshaking) {
            driveHandshake(s);
            if (s->handshaking && !s->eof) continue;   // kullanıcıya henüz görünmez
            handshakeFinished = !s->eof;
        }
        if ((e & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) || handshakeFinished || s->eof)
            ready.push_back(s->id);
    }

    // Biten ya da süresi dolan kapanışları yok et. Olay dizisi tamamen
    // işlendikten SONRA yapılır: evs[] içindeki bir işaretçi, aynı turda
    // yok edilmiş bir Sock'a sarkmasın.
    if (!L.closing.empty()) {
        const auto now = Clock::now();
        for (size_t i = 0; i < L.closing.size();) {
            if (L.closing[i]->deadline > now) { ++i; continue; }
            destroy(L.closing[i]);
            L.closing[i] = std::move(L.closing.back());
            L.closing.pop_back();
        }
    }
    return true;
}

ReadStatus readSocket(int conn, size_t maxBytes, std::string& out, std::string& err) {
    Sock* s = findKind(conn, Kind::Conn, err);
    if (!s) return ReadStatus::Error;
    if (maxBytes == 0) { err = "maxBytes must be positive"; return ReadStatus::Error; }
    if (s->avail() == 0) fill(s, maxBytes);
    if (s->avail() > 0) {
        const size_t n = std::min(maxBytes, s->avail());
        out.assign(s->in, s->inPos, n);
        s->inPos += n;
        return ReadStatus::Data;
    }
    return s->eof ? ReadStatus::Eof : ReadStatus::None;
}

ReadStatus readSocketUntil(int conn, const std::string& delimiter, size_t maxBytes,
                           std::string& out, std::string& err) {
    Sock* s = findKind(conn, Kind::Conn, err);
    if (!s) return ReadStatus::Error;
    if (delimiter.empty()) { err = "delimiter must not be empty"; return ReadStatus::Error; }

    auto search = [&]() -> size_t {
        const size_t from = std::max(s->scanPos, s->inPos);
        const size_t pos  = s->in.find(delimiter, from);
        if (pos == std::string::npos) {
            // Ayraç sınırdan bölünmüş olabilir: son (len-1) baytı yeniden tara.
            const size_t keep = delimiter.size() - 1;
            s->scanPos = std::max(s->inPos, s->in.size() > keep ? s->in.size() - keep : 0);
        }
        return pos;
    };

    size_t pos = search();
    if (pos == std::string::npos) {
        fill(s, maxBytes + delimiter.size());
        pos = search();
    }
    if (pos != std::string::npos) {
        const size_t end = pos + delimiter.size();
        if (end - s->inPos > maxBytes + delimiter.size()) {
            err = "frame exceeds maxBytes (" + std::to_string(maxBytes) + ")";
            return ReadStatus::Error;
        }
        out.assign(s->in, s->inPos, end - s->inPos);
        s->inPos   = end;
        s->scanPos = end;
        return ReadStatus::Data;
    }
    if (s->avail() > maxBytes + delimiter.size()) {
        err = "frame exceeds maxBytes (" + std::to_string(maxBytes) + ")";
        return ReadStatus::Error;
    }
    return s->eof ? ReadStatus::Eof : ReadStatus::None;
}

ReadStatus readSocketExact(int conn, size_t size, std::string& out, std::string& err) {
    Sock* s = findKind(conn, Kind::Conn, err);
    if (!s) return ReadStatus::Error;
    if (size == 0) { out.clear(); return ReadStatus::Data; }
    if (s->avail() < size) fill(s, size);
    if (s->avail() >= size) {
        out.assign(s->in, s->inPos, size);
        s->inPos += size;
        return ReadStatus::Data;
    }
    return s->eof ? ReadStatus::Eof : ReadStatus::None;
}

bool writeSocket(int conn, const char* data, size_t len, std::string& err) {
    Sock* s = findKind(conn, Kind::Conn, err);
    if (!s) return false;
    if (len == 0 || s->eof) return true;   // kopmuş bağlantıya yazma sessizce atılır

    // Düz TCP ve boş tampon: önce doğrudan gönder, yalnız kalanı kopyala.
    if (!s->ssl && s->pending() == 0) {
        ssize_t n;
        do { n = ::send(s->fd, data, len, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { s->eof = true; return true; }
        const size_t sent = n > 0 ? (size_t)n : 0;
        if (sent == len) return true;
        s->out.append(data + sent, len - sent);
        setWantWrite(s, true);
        return true;
    }
    s->out.append(data, len);
    flush(s);
    return true;
}

bool closeSocket(int handle, std::string& err) {
    std::unique_ptr<Sock> s = take(handle);
    if (!s) { err = "invalid socket handle " + std::to_string(handle); return false; }

    if (s->kind != Kind::Conn) {
        // Dinleyici ya da TLS bağlamı: hemen kapanır (bağlam shared_ptr ile
        // ondan türeyen bağlantılar yaşadıkça yaşar).
        destroy(s);
        return true;
    }
    if (s->eof) { destroy(s); return true; }

    Loop* loop = s->loop;
    if (s->pending() > 0 || s->tlsRetryLen > 0) {
        s->closeState = CloseState::Flushing;
        s->deadline   = Clock::now() + kFlushTimeout;
        flush(s.get());
        if (s->eof) { destroy(s); return true; }
        if (s->pending() == 0) startDrain(s.get());
    } else {
        startDrain(s.get());
    }
    loop->closing.push_back(std::move(s));
    return true;
}

int socketPort(int handle, std::string& err) {
    Sock* s = find(handle);
    if (!s || s->fd < 0) { err = "invalid socket handle " + std::to_string(handle); return -1; }
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (::getsockname(s->fd, reinterpret_cast<sockaddr*>(&ss), &len) < 0) {
        err = errnoText("getsockname");
        return -1;
    }
    if (ss.ss_family == AF_INET)  return ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
    if (ss.ss_family == AF_INET6) return ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
    err = "unknown address family";
    return -1;
}

bool socketPeer(int conn, std::string& out, std::string& err) {
    Sock* s = findKind(conn, Kind::Conn, err);
    if (!s) return false;
    out = formatAddr(s->peer);
    return true;
}

long socketPending(int conn, std::string& err) {
    Sock* s = findKind(conn, Kind::Conn, err);
    if (!s) return -1;
    return (long)s->pending();
}

int tlsServerContext(const std::string& certPath, const std::string& keyPath,
                     std::string& err) {
    netInit();
    sslInit();
    auto ctx = std::make_shared<TlsCtx>();
    ctx->ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx->ctx) { err = "TLS: " + sslErrorString(); return -1; }

    // RFC 8446: yalnız TLS 1.3 (ürün kararı: en düşük sürüm 1.3).
    if (SSL_CTX_set_min_proto_version(ctx->ctx, TLS1_3_VERSION) != 1) {
        err = "TLS: cannot set minimum version 1.3: " + sslErrorString();
        return -1;
    }
    // Kısmi yazma + yer değiştiren tampon: flush() çıkış tamponunu sıkıştırıp
    // büyütebilir, WANT_WRITE sonrası tekrar aynı içerikle farklı adresten gelir.
    SSL_CTX_set_mode(ctx->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                               SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    if (SSL_CTX_use_certificate_chain_file(ctx->ctx, certPath.c_str()) != 1) {
        err = "TLS: cannot load certificate '" + certPath + "': " + sslErrorString();
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx->ctx, keyPath.c_str(), SSL_FILETYPE_PEM) != 1) {
        err = "TLS: cannot load private key '" + keyPath + "': " + sslErrorString();
        return -1;
    }
    if (SSL_CTX_check_private_key(ctx->ctx) != 1) {
        err = "TLS: certificate and private key do not match: " + sslErrorString();
        return -1;
    }

    auto s  = std::make_unique<Sock>();
    s->kind = Kind::TlsContext;
    s->tls  = std::move(ctx);
    return insert(std::move(s));
}

} // namespace saqut::net
