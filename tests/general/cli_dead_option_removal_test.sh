#!/usr/bin/env bash
# #147 SQ-090-CLI-DEAD-OPTION-REMOVAL — tracked negative + regression suite.
set -eu

binary=$1
root=$2
f="$root/tests/general/cli_dead_option_removal.sqt"

out=$(mktemp); err=$(mktemp)
trap 'rm -f "$out" "$err"' EXIT

check_exit64_empty_stdout() {
    # $@ = extra args before the source file
    set +e
    "$binary" run "$@" "$f" >"$out" 2>"$err"
    actual=$?
    set -e
    if [ "$actual" -ne 64 ]; then
        echo "FAIL: '$*' beklenen exit 64, gercek $actual" >&2
        cat "$err" >&2
        exit 1
    fi
    if [ -s "$out" ]; then
        echo "FAIL: '$*' stdout bos olmali, gercek: $(cat "$out")" >&2
        exit 1
    fi
    if ! grep -qi "format" "$err"; then
        echo "FAIL: '$*' stderr acik --format mesaji icermiyor: $(cat "$err")" >&2
        exit 1
    fi
}

# N1/N2: uzun form ve esitlikli form, run uzerinde.
check_exit64_empty_stdout --format json
check_exit64_empty_stdout --format=json

# N1b: --format global parseArgs seviyesinde reddediliyor, yalniz "run"a
# ozel degil — ikinci bir komutta da (ast) ayni sekilde reddedildigini
# dogrula.
set +e
"$binary" ast --format=json "$f" >"$out" 2>"$err"
actual=$?
set -e
test "$actual" -eq 64
test ! -s "$out"

# N3: bayraksiz regresyon.
set +e
"$binary" run "$f" >"$out" 2>"$err"
actual=$?
set -e
test "$actual" -eq 0
test -s "$out"

# N4 (#145 SQ-100-SYMBOLS-JSONL): symbols --json preview kaldirildi —
# kullanim hatasi (64) dondurur; makine yuzeyi --jsonl'dir.
set +e
"$binary" symbols --json "$f" >"$out" 2>"$err"
actual=$?
set -e
if [ "$actual" -ne 64 ]; then
    echo "FAIL: 'symbols --json' beklenen exit 64, gercek $actual" >&2
    exit 1
fi
if [ -s "$out" ]; then
    echo "FAIL: 'symbols --json' stdout bos olmali, gercek: $(cat "$out")" >&2
    exit 1
fi
if ! grep -qi "jsonl" "$err"; then
    echo "FAIL: 'symbols --json' stderr --jsonl yonlendirmesi icermiyor: $(cat "$err")" >&2
    exit 1
fi

# N5 (#145): --compact JSONL'de anlamsiz — sessizce yutulmaz (exit 64).
set +e
"$binary" symbols --jsonl --compact "$f" >"$out" 2>"$err"
actual=$?
set -e
if [ "$actual" -ne 64 ]; then
    echo "FAIL: 'symbols --jsonl --compact' beklenen exit 64, gercek $actual" >&2
    exit 1
fi

# P1 (K-20): --output korunuyor. NOT: --output + varsayilan (JSON olmayan)
# ast modu ayrica saqutlang/saqut#160'ta kayitli, bu task'in kapsami disinda
# bagimsiz bir bug (displayAst->log() std::cout'a hardcode, --output
# yok sayiliyor) icerir; burada K-20'nin gercekten calistigi --json yolu
# test edilir.
outfile=$(mktemp)
rm -f "$outfile"
"$binary" ast --json --output "$outfile" "$f" >/dev/null 2>&1
test -s "$outfile"
rm -f "$outfile"

# P2: canli option matrisi — 9/9. Her biri en az bir smoke komutuyla,
# usage error (64) URETMEDEN calisir.
run_live() {
    # $1 = beklenen komut, kalan = args
    cmd=$1; shift
    set +e
    "$binary" "$cmd" "$@" "$f" >"$out" 2>"$err"
    actual=$?
    set -e
    if [ "$actual" -eq 64 ]; then
        echo "FAIL: canli secenek '$cmd $*' beklenmedik usage error (64) verdi: $(cat "$err")" >&2
        exit 1
    fi
}
run_live symbols --compact
run_live symbols --jsonl
run_live run --dont-optimize
run_live run --optimized   # geriye uyum: no-op ama usage error vermemeli
run_live run --jit
run_live run --profile
run_live run --gc-stats
run_live bench --verbose

# N6 (#257): kaldırılmış capability bayrakları (ADR-043) ve tanınmayan
# bayraklar artık sessizce yutulmaz — kullanım hatası (64).
for opt in "run --allow-fs" "ir --capabilities" "run --jitt" "run --gc-treshold=5"; do
    set +e
    # shellcheck disable=SC2086
    "$binary" $opt "$f" >"$out" 2>"$err"
    actual=$?
    set -e
    if [ "$actual" -ne 64 ]; then
        echo "FAIL: '$opt' beklenen exit 64, gercek $actual" >&2
        exit 1
    fi
done

# N7 (#257): dosyadan sonra `--` olmadan gelen argüman düşürülmez — hata.
set +e
"$binary" run "$f" foo >"$out" 2>"$err"
actual=$?
set -e
test "$actual" -eq 64
grep -q -- "--" "$err"
