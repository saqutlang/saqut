#!/usr/bin/env python3
"""saQut LSP golden test sürücüsü.

Bir .jsonl senaryosundaki mesajları (Content-Length zarfıyla) `saqut lsp`
sürecinin stdin'ine sırayla yazar, stdin'i kapatır, süreç bitene kadar
stdout'tan gelen TÜM mesajları (response + notification, üretilme sırasıyla)
toplar ve bunları .expected.jsonl ile satır satır karşılaştırır.

Karşılaştırma JSON-yapısal yapılır (anahtar sırası önemsiz); metin/regex
karşılaştırması DEĞİLDİR.

Kullanım:
    lsp_test_driver.py --binary <saqut> --fixtures <dir> --scenario X.jsonl --expected X.expected.jsonl
    lsp_test_driver.py --binary <saqut> --fixtures <dir> --scenario X.jsonl --record X.expected.jsonl
        (--record: gerçek çıktıyı expected dosyası olarak yazar — yeni senaryo eklerken kullanılır)

Expected dosyasında `{"$any": true}` satırı o konumdaki mesajı içeriğine
bakmadan kabul eder (initialize yanıtı gibi senaryonun konusu olmayan mesajlar).

Senaryo/expected dosyalarında `%FIXDIR%` yer tutucusu, --fixtures ile verilen
mutlak dizinle değiştirilir (URI'ler ve dosya sistemi bağımsız olsun diye).
"""
import argparse
import json
import subprocess
import sys


def frame(obj) -> bytes:
    # Faz 6 (#84) dayanıklılık senaryoları için kaçış kapısı: {"__raw__": "..."}
    # gövdeyi JSON'a çevirmeden OLDUĞU GİBİ gönderir (bozuk JSON testi).
    if isinstance(obj, dict) and "__raw__" in obj:
        data = obj["__raw__"].encode("utf-8")
    else:
        body = json.dumps(obj)
        data = body.encode("utf-8")
    return f"Content-Length: {len(data)}\r\n\r\n".encode("ascii") + data


def read_all_messages(data: bytes):
    msgs = []
    i = 0
    while True:
        hdr_end = data.find(b"\r\n\r\n", i)
        if hdr_end == -1:
            break
        header = data[i:hdr_end].decode("utf-8", errors="replace")
        length = None
        for line in header.split("\r\n"):
            if line.lower().startswith("content-length:"):
                length = int(line.split(":", 1)[1].strip())
        if length is None:
            break
        body_start = hdr_end + 4
        body = data[body_start:body_start + length]
        msgs.append(json.loads(body.decode("utf-8")))
        i = body_start + length
    return msgs


def load_jsonl(path: str, fixdir: str):
    items = []
    with open(path, "r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, start=1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            line = line.replace("%FIXDIR%", fixdir)
            try:
                items.append(json.loads(line))
            except json.JSONDecodeError as e:
                raise SystemExit(f"{path}:{lineno}: geçersiz JSON: {e}")
    return items


def dump_jsonl(items, fixdir: str) -> str:
    lines = []
    for obj in items:
        s = json.dumps(obj, ensure_ascii=False, sort_keys=True)
        s = s.replace(fixdir, "%FIXDIR%")
        lines.append(s)
    return "\n".join(lines) + "\n"


def run_scenario(binary: str, fixdir: str, scenario_path: str, timeout: float):
    messages = load_jsonl(scenario_path, fixdir)
    proc = subprocess.Popen(
        [binary, "lsp"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    try:
        # Tüm çerçeveler communicate'e girdi olarak verilir (stdin'i elle
        # kapatıp communicate çağırmak Python 3.11'de "flush of closed file"
        # hatası veriyor).
        data = b"".join(frame(m) for m in messages)
        out, err = proc.communicate(input=data, timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
        raise SystemExit(
            f"HATA: senaryo zaman aşımına uğradı ({timeout}s): {scenario_path}\n"
            f"stderr:\n{err.decode('utf-8', errors='replace')}"
        )
    if proc.returncode not in (0, None):
        sys.stderr.write(
            f"UYARI: saqut lsp exit code {proc.returncode}\n"
            f"stderr:\n{err.decode('utf-8', errors='replace')}\n"
        )
    return read_all_messages(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--fixtures", required=True)
    ap.add_argument("--scenario", required=True)
    ap.add_argument("--expected")
    ap.add_argument("--record")
    ap.add_argument("--timeout", type=float, default=15.0)
    args = ap.parse_args()

    if not args.expected and not args.record:
        raise SystemExit("--expected veya --record ver")

    fixdir = args.fixtures.rstrip("/")
    actual = run_scenario(args.binary, fixdir, args.scenario, args.timeout)

    if args.record:
        with open(args.record, "w", encoding="utf-8") as f:
            f.write(dump_jsonl(actual, fixdir))
        print(f"Kaydedildi: {args.record} ({len(actual)} mesaj)")
        return 0

    expected = load_jsonl(args.expected, fixdir)

    if len(actual) != len(expected):
        print(f"FAIL: mesaj sayısı uyuşmuyor — beklenen {len(expected)}, gerçek {len(actual)}")
        print("--- GERÇEK (tümü) ---")
        print(dump_jsonl(actual, fixdir))
        return 1

    ok = True
    for idx, (exp, act) in enumerate(zip(expected, actual)):
        # {"$any": true}: bu konumda bir mesaj gelmeli ama içeriği bu senaryonun
        # konusu değil (ör. initialize yanıtının capability listesi — onu
        # yalnız 01_initialize doğrular; her yeni özellik 25 golden'ı kırmasın).
        if exp == {"$any": True}:
            continue
        if exp != act:
            ok = False
            print(f"FAIL: mesaj #{idx} uyuşmuyor")
            print("--- BEKLENEN ---")
            print(json.dumps(exp, indent=2, ensure_ascii=False, sort_keys=True))
            print("--- GERÇEK ---")
            print(json.dumps(act, indent=2, ensure_ascii=False, sort_keys=True))

    if not ok:
        return 1

    print(f"OK: {args.scenario} ({len(actual)} mesaj eşleşti)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
