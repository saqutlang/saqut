// ============================================================================
// saQut — TCP/TLS çalışma zamanı (net / tls host modüllerinin motoru)
// ============================================================================
//
// DİZİN:   src/net/net_runtime.hpp
// KATMAN:  runtime/net — VM tiplerini bilmez; FFI thunk'ları
//          (src/ffi/functions/net.cpp) bu API'yi çağırır.
//
// HANDLE MODELİ: dile yalnız int gider. Soket, dinleyici ve TLS bağlamı
// süreç-global bir tabloda (id → Sock) yaşar; saQut kodu id ile konuşur.
// id'ler 1'den başlar, yeniden kullanılmaz (taşmada canlı id'ler atlanır).
//
// G/Ç MODELİ: her şey non-blocking. Tek bloklayan çağrı pollSockets'tir:
// çağıran thread'in epoll örneğinde bekler ve okunabilir hale gelen
// (yeni veri, yeni bağlantı, EOF/hata) handle'ların id'lerini döndürür.
//   - Yazma tamponludur: writeSocket gönderebildiğini hemen gönderir, kalanı
//     bağlantının çıkış tamponunda tutar ve pollSockets içinde EPOLLOUT ile
//     boşaltır. Kullanıcı EAGAIN/kısmi yazma görmez.
//   - Okuma çekerek yapılır: read* çağrıları çekirdekten (TLS'te SSL'den)
//     giriş tamponuna okur ve istenen parçayı verir. Dönüş üç durumludur:
//     Data, None (şimdilik veri yok), Eof (karşı taraf kapattı / bağlantı
//     koptu). pollSockets bir bağlantıyı YENİ veri geldiğinde bildirir;
//     tamponda kalan veri tekrar bildirilmez → kullanıcı None alana dek okur.
//   - TLS el sıkışması kullanıcıya görünmez: acceptSocket TCP bağlantısını
//     hemen verir, el sıkışması pollSockets/read içinde ilerler; bitene dek
//     read* None döner, başarısız olursa bağlantı Eof olur.
//   - closeSocket kullanıcı id'sini hemen geçersiz kılar; bağlantı çıkış
//     tamponu boşalana dek yaşar, sonra RFC 9112 §9.6 kapanışı yapılır
//     (SHUT_WR + karşı tarafın FIN'ini bekleyerek okuyup atma). Bu iç iş
//     de pollSockets içinde yürür.
//
// THREAD MODELİ: epoll örneği THREAD BAŞINADIR (thread_local). Bir dinleyici
// ya da bağlantı, onu oluşturan thread'in epoll'una kaydolur ve yalnız o
// thread'in pollSockets'i onu bildirir. Çok thread'li ölçek için her thread
// kendi dinleyicisini reusePort=true ile açar (çekirdek bağlantıları dağıtır).
// Aynı handle'ı iki thread'den eşzamanlı kullanmak DESTEKLENMEZ; tablo
// erişimi kilitlidir ama Sock'un kendisi tek sahiplidir.
// TODO(net): isolate bitince o isolate'in handle'larını süpür.
//
// SİNYAL: ilk kullanımda SIGPIPE süreç genelinde yok sayılır (SSL_write alttaki
// sokete MSG_NOSIGNAL olmadan yazar; kopan istemci süreci öldürmemeli).
// ============================================================================

#ifndef SAQUT_NET_RUNTIME
#define SAQUT_NET_RUNTIME

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace saqut::net {

// Okuma sonucu: Data → out dolu; None → şimdilik veri yok; Eof → bağlantı
// kapandı (kalan kısmi veri atılır), kullanıcı closeSocket çağırmalı.
enum class ReadStatus : uint8_t { Data, None, Eof, Error };

// Hata mesajı `err`'e yazılır; dönüş < 0 (id üreten çağrılar) ya da false.

// host: sayısal IPv4/IPv6 adresi ("0.0.0.0", "127.0.0.1", "::") ya da
// "localhost". DNS çözümü YOK (statik glibc'de NSS kullanılamaz).
// port 0 → çekirdek boş bir port seçer (socketPort ile öğrenilir).
// tlsContext 0 değilse kabul edilen bağlantılar TLS sunucu bağlantısıdır.
int  listenTcp(const std::string& host, int port, bool reusePort,
               int tlsContext, std::string& err);

// >0: yeni bağlantı id'si; 0: bekleyen bağlantı yok; <0: hata.
int  acceptSocket(int listener, std::string& err);

// timeoutMs < 0 → süresiz bekle; 0 → bekleme. ready'ye hazır id'ler eklenir.
bool pollSockets(int timeoutMs, std::vector<int>& ready, std::string& err);

ReadStatus readSocket(int conn, size_t maxBytes, std::string& out, std::string& err);
// Ayraç (dahil) ile biten ilk çerçeve. Tamponda ayraçsız maxBytes aşılırsa Error.
ReadStatus readSocketUntil(int conn, const std::string& delimiter, size_t maxBytes,
                           std::string& out, std::string& err);
// Tam `size` bayt birikince verir.
ReadStatus readSocketExact(int conn, size_t size, std::string& out, std::string& err);

bool writeSocket(int conn, const char* data, size_t len, std::string& err);
bool closeSocket(int handle, std::string& err);

int  socketPort(int handle, std::string& err);          // yerel port; <0 hata
bool socketPeer(int conn, std::string& out, std::string& err);
long socketPending(int conn, std::string& err);         // gönderilmeyi bekleyen bayt

// TLS 1.3 sunucu bağlamı (en düşük sürüm 1.3). certPath zincir PEM'i
// (sertifika + ara sertifikalar), keyPath PEM özel anahtar.
int  tlsServerContext(const std::string& certPath, const std::string& keyPath,
                      std::string& err);

} // namespace saqut::net

#endif // SAQUT_NET_RUNTIME
