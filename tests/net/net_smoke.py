#!/usr/bin/env python3
"""net/tls host modüllerinin protokol smoke testi.

tests/net/echo_server.sqt'yi başlatır ve gerçek soketlerle sınar. Her senaryo
bir hata SINIFINI hedefler (yalnız mutlu yol değil):

  get            temel istek/yanıt
  keepalive      aynı bağlantıda ardışık istekler
  pipelining     tek send() içinde 3 istek → 3 yanıt (giriş tamponunda kalan
                 istekler kaybolmamalı; pollSockets onları yeniden bildirmez)
  split          başlık bayt bayt gelir (ayraç sınırda bölünür)
  big_body       2 MB gövde, birçok read'e yayılır (readSocketExact + pending)
  slow_reader    2 MB yanıt, istemci yavaş okur (çıkış tamponu + EPOLLOUT)
  close_pending  Connection: close + büyük yanıt: kapanış tamponu boşaltmalı
  rst            istemci RST ile kopar; sunucu çalışmaya devam etmeli
  many           200 eşzamanlı bağlantı
  tls_*          aynı senaryoların bir kısmı TLS 1.3 üzerinden; TLS 1.2 reddi

Kullanım: net_smoke.py <saqut> <repo-kökü>
TLS senaryoları sistem `openssl` CLI'si ile geçici sertifika üretir; CLI
yoksa TLS kısmı açıkça SKIP yazar.
"""
import os
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time

SAQUT = sys.argv[1]
ROOT = sys.argv[2]
SERVER = os.path.join(ROOT, "tests", "net", "echo_server.sqt")
BIG = 2 * 1024 * 1024
# Çekirdek gönderme tamponu üst sınırından (tcp_wmem max, tipik 4 MB) büyük:
# yanıtın bir kısmı zorunlu olarak sunucunun KENDİ çıkış tamponunda kalır.
HUGE = 8 * 1024 * 1024
# Alma tamponu bağlantıdan ÖNCE küçültülür; sonradan küçültmek pencereyi
# daraltır ve çekirdeği RTO'lu yeniden gönderime iter (test yapaylığı).
SMALL_RCVBUF = 65536

failures = []


def check(name, cond, detail=""):
    if cond:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name} {detail}")
        failures.append(name)


def start_server(env_extra=None):
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    proc = subprocess.Popen([SAQUT, "run", SERVER, "--", "0"], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=env)
    line = proc.stdout.readline().decode()
    if not line.startswith("port "):
        proc.kill()
        err = proc.stderr.read().decode()
        raise RuntimeError(f"sunucu başlamadı: {line!r} {err}")
    return proc, int(line.split()[1])


def recv_response(sock):
    """Tek bir HTTP yanıtı oku: (başlık, gövde). Bağlantı erken kapanırsa None."""
    r = recv_response_buf(sock, b"")
    return None if r is None else (r[0], r[1])


def tcp_connect(port, rcvbuf=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if rcvbuf:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.settimeout(10)
    s.connect(("127.0.0.1", port))
    return s


def request(path="/", body=b"", close=False):
    h = f"POST {path} HTTP/1.1\r\nHost: x\r\nContent-Length: {len(body)}\r\n"
    if close:
        h += "Connection: close\r\n"
    return h.encode() + b"\r\n" + body


def run_suite(prefix, connect):
    # get
    s = connect()
    s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    r = recv_response(s)
    check(f"{prefix}get", r is not None and r[1] == b"merhaba\n", repr(r))

    # keepalive
    ok = True
    for i in range(3):
        s.sendall(request(body=f"istek{i}".encode()))
        r = recv_response(s)
        ok = ok and r is not None and r[1] == f"istek{i}".encode()
    check(f"{prefix}keepalive", ok)
    s.close()

    # pipelining: üç istek tek sendall'da
    s = connect()
    s.sendall(request(body=b"a") + request(body=b"bb") + request(body=b"ccc"))
    got = []
    buf = b""
    for _ in range(3):
        r = recv_response_buf(s, buf)
        if r is None:
            break
        got.append(r[1])
        buf = r[2]
    check(f"{prefix}pipelining", got == [b"a", b"bb", b"ccc"], repr(got))
    s.close()

    # split: başlık bayt bayt, ayraç sınırda bölünerek
    s = connect()
    raw = request(body=b"parca")
    for b in raw:
        s.sendall(bytes([b]))
        time.sleep(0.001)
    r = recv_response(s)
    check(f"{prefix}split", r is not None and r[1] == b"parca", repr(r))
    s.close()

    # big_body
    s = connect()
    payload = os.urandom(BIG // 2).hex().encode()   # ASCII, BIG bayt
    s.sendall(request(body=payload))
    r = recv_response(s)
    check(f"{prefix}big_body", r is not None and r[1] == payload,
          f"len={len(r[1]) if r else None}")
    s.close()

    # slow_reader: 8 MB yanıt, istemci geç okur → sunucu çıkış tamponu +
    # EPOLLOUT ile boşaltma
    huge = os.urandom(HUGE // 2).hex().encode()
    s = connect(SMALL_RCVBUF)
    s.sendall(request(body=huge))
    time.sleep(0.3)
    r = recv_response(s)
    check(f"{prefix}slow_reader", r is not None and r[1] == huge)
    s.close()

    # close_pending: Connection: close + 8 MB yanıt. closeSocket çağrıldığında
    # yanıtın en az ~4 MB'ı hâlâ sunucunun çıkış tamponundadır; kapanış onu
    # boşaltmadan soketi kapatırsa istemci eksik gövde/RST görür.
    s = connect(SMALL_RCVBUF)
    s.sendall(request(body=huge, close=True))
    time.sleep(0.3)
    r = recv_response(s)
    tail = s.recv(1) if r is not None else None
    check(f"{prefix}close_pending", r is not None and r[1] == huge and tail == b"",
          f"len={len(r[1]) if r else None} tail={tail!r}")
    s.close()

    # rst: istemci yarım istekle RST gönderip kopar; sunucu ayakta kalmalı
    s = connect()
    s.sendall(b"POST / HTTP/1.1\r\nContent-Length: 100\r\n\r\nyarim")
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()
    time.sleep(0.1)
    s = connect()
    s.sendall(b"GET / HTTP/1.1\r\n\r\n")
    r = recv_response(s)
    check(f"{prefix}rst", r is not None and r[1] == b"merhaba\n")
    s.close()

    # many: 200 eşzamanlı bağlantı
    socks = [connect() for _ in range(200)]
    for i, c in enumerate(socks):
        c.sendall(request(body=f"n{i}".encode()))
    ok = all((lambda r, i: r is not None and r[1] == f"n{i}".encode())(recv_response(c), i)
             for i, c in enumerate(socks))
    for c in socks:
        c.close()
    check(f"{prefix}many", ok)


def recv_response_buf(sock, buf):
    """recv_response'un önceki artığı hesaba katan hali: (başlık, gövde, artık)."""
    buf = bytearray(buf)
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(65536)
        if not chunk:
            return None
        buf += chunk
    head, rest = bytes(buf).split(b"\r\n\r\n", 1)
    length = 0
    for line in head.split(b"\r\n")[1:]:
        k, _, v = line.partition(b":")
        if k.strip().lower() == b"content-length":
            length = int(v.strip())
    rest = bytearray(rest)
    while len(rest) < length:
        chunk = sock.recv(262144)
        if not chunk:
            return None
        rest += chunk
    return head.decode(), bytes(rest[:length]), bytes(rest[length:])


def main():
    print("=== net (TCP) ===")
    proc, port = start_server()
    try:
        run_suite("", lambda rcvbuf=None: tcp_connect(port, rcvbuf))
        check("server_alive", proc.poll() is None)
    finally:
        proc.kill()
        proc.wait()

    print("=== tls ===")
    openssl = shutil.which("openssl")
    if not openssl:
        print("  SKIP tls: sistem openssl CLI yok (sertifika üretilemiyor)")
    else:
        with tempfile.TemporaryDirectory() as d:
            cert, key = os.path.join(d, "cert.pem"), os.path.join(d, "key.pem")
            subprocess.run([openssl, "req", "-x509", "-newkey", "ec", "-pkeyopt",
                            "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", key,
                            "-out", cert, "-days", "1", "-subj", "/CN=localhost",
                            "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"],
                           check=True, capture_output=True)
            proc, port = start_server({"SAQUT_TLS_CERT": cert, "SAQUT_TLS_KEY": key})
            try:
                ctx = ssl.create_default_context(cafile=cert)

                def tls_connect(rcvbuf=None):
                    return ctx.wrap_socket(tcp_connect(port, rcvbuf),
                                           server_hostname="localhost")

                s = tls_connect()
                check("tls_version_1_3", s.version() == "TLSv1.3", s.version())
                s.close()
                run_suite("tls_", tls_connect)

                old = ssl.create_default_context(cafile=cert)
                old.maximum_version = ssl.TLSVersion.TLSv1_2
                try:
                    raw = socket.create_connection(("127.0.0.1", port), timeout=10)
                    old.wrap_socket(raw, server_hostname="localhost")
                    check("tls_rejects_1_2", False, "TLS 1.2 kabul edildi")
                except ssl.SSLError:
                    check("tls_rejects_1_2", True)

                # El sıkışmayı yarıda bırakan istemci sunucuyu tıkamamalı.
                raw = socket.create_connection(("127.0.0.1", port), timeout=10)
                raw.sendall(b"\x16\x03\x01\x00\x05hello")   # bozuk ClientHello
                raw.close()
                s = tls_connect()
                s.sendall(b"GET / HTTP/1.1\r\n\r\n")
                r = recv_response(s)
                check("tls_bad_handshake_isolated", r is not None and r[1] == b"merhaba\n")
                s.close()
                check("tls_server_alive", proc.poll() is None)
            finally:
                proc.kill()
                proc.wait()

    if failures:
        print(f"BAŞARISIZ: {len(failures)} senaryo: {', '.join(failures)}")
        sys.exit(1)
    print("net_smoke: TUM SENARYOLAR GECTI")


if __name__ == "__main__":
    main()
