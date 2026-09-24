#!/usr/bin/env python3
"""ADR-045 DAP thread duman testi.

Senaryo (examples/threading/producer_consumer.sqt):
  breakpoint (satır 37) -> stopped(allThreadsStopped) -> threads (main + 3
  tüketici) -> her işçinin stackTrace'i + scopes -> continue -> exited 0.
Ayrıca yaşam döngüsü olayları: 5 işçinin her biri için tam bir started ve
bir exited `thread` olayı (kısa ömürlü üreticiler dahil).

Olay sırası thread zamanlamasına bağlı olduğundan golden karşılaştırma değil,
iddia (assert) tabanlıdır.

Kullanım: dap_threads_smoke.py <saqut-ikilisi> <depo-kökü>
"""
import json
import os
import queue
import subprocess
import sys
import threading
import time

BIN, ROOT = sys.argv[1], sys.argv[2]
PROG = os.path.join(os.path.abspath(ROOT), "examples/threading/producer_consumer.sqt")
LINE = 37

p = subprocess.Popen([BIN, "dap"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
q = queue.Queue()
events = []


def reader():
    f = p.stdout
    while True:
        hdr = b""
        while not hdr.endswith(b"\r\n\r\n"):
            c = f.read(1)
            if not c:
                q.put(None)
                return
            hdr += c
        n = int(hdr.split(b":")[1].split(b"\r\n")[0])
        q.put(json.loads(f.read(n)))


threading.Thread(target=reader, daemon=True).start()
seq = 0


def send(cmd, args=None):
    global seq
    seq += 1
    body = json.dumps({"type": "request", "seq": seq, "command": cmd,
                       "arguments": args or {}}).encode()
    p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    p.stdin.flush()
    return seq


def fail(msg):
    p.kill()
    sys.exit("FAIL: " + msg)


def wait(pred, timeout=20):
    end = time.time() + timeout
    while True:
        left = end - time.time()
        if left <= 0:
            fail("zaman aşımı")
        try:
            m = q.get(timeout=left)
        except queue.Empty:
            fail("zaman aşımı")
        if m is None:
            fail("adapter beklenmedik şekilde kapandı")
        if m.get("type") == "event" and m["event"] == "thread":
            events.append((m["body"]["reason"], m["body"]["threadId"]))
        if pred(m):
            return m


def resp(s):
    return lambda m: m.get("type") == "response" and m.get("request_seq") == s


wait(resp(send("initialize", {"adapterID": "sqt", "linesStartAt1": True})))
wait(resp(send("launch", {"program": PROG, "stopOnEntry": False})))
r = wait(resp(send("setBreakpoints", {"source": {"path": PROG},
                                      "breakpoints": [{"line": LINE}]})))
if not r["body"]["breakpoints"][0]["verified"]:
    fail("breakpoint doğrulanmadı")
wait(resp(send("configurationDone")))
st = wait(lambda m: m.get("type") == "event" and m["event"] == "stopped")
if st["body"].get("reason") != "breakpoint" or not st["body"].get("allThreadsStopped"):
    fail("beklenen breakpoint/all-stop değil: %r" % st["body"])

th = wait(resp(send("threads")))["body"]["threads"]
ids = [t["id"] for t in th]
if ids[0] != 1 or len(ids) != 4:
    fail("thread listesi beklenmedik: %r" % th)
for t in th[1:]:
    if not t["name"].startswith("thread#%d @ producer_consumer.sqt:" % t["id"]):
        fail("thread adı beklenmedik: %r" % t["name"])
    s = wait(resp(send("stackTrace", {"threadId": t["id"]})))
    frames = s["body"].get("stackFrames", [])
    if not s["success"] or not frames:
        fail("işçi stackTrace boş: %r" % s)
    if "consume" not in frames[0]["name"]:
        fail("işçi üst çerçevesi beklenmedik: %r" % frames[0])
    sc = wait(resp(send("scopes", {"frameId": frames[0]["id"]})))
    names = [x["name"] for x in sc["body"]["scopes"]]
    if "Locals" not in names or "Shared" not in names:
        fail("işçi scope'ları beklenmedik: %r" % names)

wait(resp(send("continue", {"threadId": 1})))
ex = wait(lambda m: m.get("type") == "event" and m["event"] == "exited")
if ex["body"].get("exitCode") != 0:
    fail("çıkış kodu %r" % ex["body"])
wait(lambda m: m.get("type") == "event" and m["event"] == "terminated")
p.stdin.close()
p.wait(timeout=10)

for tid in range(2, 7):
    if events.count(("started", tid)) != 1 or events.count(("exited", tid)) != 1:
        fail("thread %d yaşam döngüsü olayları eksik/fazla: %r" % (tid, events))
    if events.index(("started", tid)) > events.index(("exited", tid)):
        fail("thread %d: exited, started'dan önce" % tid)
print("dap_threads_smoke: OK (%d thread olayı)" % len(events))
