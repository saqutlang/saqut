#!/usr/bin/env bash
# #141 SQ-090-IR-TTY-COLOR — tracked PTY/non-PTY regresyon.
set -eu

binary=$1
root=$2
f="$root/examples/fibonacci.sqt"

redirect_out=$(mktemp); redirect_err=$(mktemp)
trap 'rm -f "$redirect_out" "$redirect_err"' EXIT

# P1/N1: redirect edilen stdout VE stderr'de hiç ANSI CSI olmamalı.
# (stdin de /dev/null'a bağlanarak "yalnız stdout TTY durumu belirler" (N1)
# aynı komutla dolaylı olarak sağlanıyor.)
"$binary" ir "$f" > "$redirect_out" 2>"$redirect_err" < /dev/null
if grep -qP '\x1b\[' "$redirect_out"; then
    echo "FAIL: redirect edilen stdout ANSI CSI iceriyor" >&2
    exit 1
fi
if grep -qP '\x1b\[' "$redirect_err"; then
    echo "FAIL: redirect edilen stderr ANSI CSI iceriyor" >&2
    exit 1
fi

# P4: determinizm — üç kez byte-identical (redirect modunda).
r2=$(mktemp); r3=$(mktemp)
trap 'rm -f "$redirect_out" "$redirect_err" "$r2" "$r3"' EXIT
"$binary" ir "$f" > "$r2" 2>/dev/null
"$binary" ir "$f" > "$r3" 2>/dev/null
diff "$redirect_out" "$r2" >/dev/null
diff "$r2" "$r3" >/dev/null

# P2/P3: PTY simülasyonu (script aracı). Yoksa BLOCKED değil, NOT TESTED —
# yalnız bu iki senaryo atlanır, script bulunamaması derleyici davranışının
# hatası değildir.
if command -v script >/dev/null 2>&1; then
    pty_raw=$(mktemp)
    trap 'rm -f "$redirect_out" "$redirect_err" "$r2" "$r3" "$pty_raw"' EXIT
    script -qc "$binary ir $f" "$pty_raw" >/dev/null 2>&1 || true

    if ! grep -qP '\x1b\[' "$pty_raw"; then
        echo "FAIL: PTY (script) altinda ANSI CSI bulunamadi — TTY renk davranisi kayip" >&2
        exit 1
    fi

    # P3: ANSI + script'in kendi \r ve başlık/altlık satırlarını temizleyip
    # redirect çıktısıyla birebir karşılaştır (yalnız renk katmanı fark
    # etmeli — opcode/operand/sıra/whitespace aynı kalmalı).
    pty_clean=$(mktemp)
    trap 'rm -f "$redirect_out" "$redirect_err" "$r2" "$r3" "$pty_raw" "$pty_clean"' EXIT
    tail -n +2 "$pty_raw" \
        | sed -E 's/\x1b\[[0-9;]*m//g' \
        | sed 's/\r$//' \
        | grep -v '^Script done' \
        > "$pty_clean"
    # script'in kendi wrapper'ından kalan trailing boş satır(lar)ı kırp —
    # saqut'un kendi çıktısındaki tek trailing boş satır (END'den sonra) bu
    # sed ile silinmez, yalnız FAZLADAN eklenenler gider.
    sed -e :a -e '/^\n*$/{$d;N;ba' -e '}' "$pty_clean" > "$pty_clean.trimmed"
    mv "$pty_clean.trimmed" "$pty_clean"
    if ! diff "$redirect_out" "$pty_clean" >/dev/null; then
        echo "FAIL: PTY (ANSI temizlenmis) ile redirect ciktisi semantik olarak farkli" >&2
        diff "$redirect_out" "$pty_clean" >&2 || true
        exit 1
    fi
else
    echo "NOT TESTED: 'script' araci bulunamadi — P2/P3 (PTY) atlandi" >&2
fi
