#!/usr/bin/env bash
# #218 — saqut ir --cfg bayrağı (yalnız ir komutunda): flat liste yerine
# CFG (BasicBlock + kenarlar) basar. Tracked: bayrağın varlığı + CFG
# doğruluğu (fall-through kenar, blok ID semantiği) + determinizm.
set -eu

binary=$1
root=$2
f="$root/examples/fibonacci.sqt"

out=$(mktemp); err=$(mktemp)
trap 'rm -f "$out" "$err"' EXIT

# 1) --cfg: üç fonksiyon da CFG olarak basılıyor, exit 0.
set +e
"$binary" ir --cfg "$f" > "$out" 2>"$err"
status=$?
set -e
[ "$status" -eq 0 ] || { echo "FAIL: ir --cfg exit 0 olmali, gercek $status" >&2; cat "$err" >&2; exit 1; }
grep -q "^fibonacci$" "$out"  || { echo "FAIL: fibonacci adı yok" >&2; exit 1; }
grep -q "^fibonacciIterative$" "$out" || { echo "FAIL: fibonacciIterative adı yok" >&2; exit 1; }
grep -q "^main$" "$out" || { echo "FAIL: main adı yok" >&2; exit 1; }

# 2) Fall-through kenar: init bloğu (düz talimatla biter) koşul bloğuna
#    akmalı — eksik kenar hatası burada yakalanır.
grep -q "BB_0 \[0..3\] preds:{} succs:{BB_1} term=LOAD_CONST" "$out" \
    || { echo "FAIL: fibonacciIterative BB_0 → BB_1 fall-through kenarı yok" >&2; exit 1; }

# 3) Blok ID semantiği: JMP geri kenarı BB_1'e (blok ID) gidiyor — talimat
#    indeksi (3) DEĞİL; JIF_FALSE da blok ID basıyor (4 değil).
grep -q "BB_2 \[5..12\] preds:{BB_1} succs:{BB_1} term=JMP ->BB_1" "$out" \
    || { echo "FAIL: JMP geri kenarı blok ID göstermiyor" >&2; exit 1; }
grep -q "BB_0 \[0..3\] preds:{} succs:{BB_1,BB_2} term=JIF_FALSE ->BB_2" "$out" \
    || { echo "FAIL: fibonacci BB_0 JIF_FALSE blok ID (BB_2) göstermiyor" >&2; exit 1; }

# 4) Determinizm: iki koşu bayt-bayt aynı.
set +e
"$binary" ir --cfg "$f" > "$out.2" 2>/dev/null
set -e
diff "$out" "$out.2" >/dev/null || { echo "FAIL: ir --cfg determinizm bozuldu" >&2; exit 1; }

# 5) Bayraksız çağrı hâlâ flat IR basıyor (regresyon yok).
set +e
"$binary" ir "$f" > "$out" 2>/dev/null
status=$?
set -e
[ "$status" -eq 0 ] || { echo "FAIL: bayraksız ir exit 0 olmali" >&2; exit 1; }
grep -q "LOAD_CONST" "$out" || { echo "FAIL: flat IR LOAD_CONST içermiyor" >&2; exit 1; }
if grep -q "^fibonacci$" "$out"; then
    echo "FAIL: bayraksız ir fonksiyon adı basmamali (CFG modu kapalı)" >&2
    exit 1
fi

echo "ir_cfg_flag: TUM TESTLER GECTI"
