#!/usr/bin/env bash
# #291 — CLI komut tanımı (CliCommand) tek kaynaktır: seçenek geçerliliği,
# konumsal argüman sayısı ve yardım metni oradan türetilir.
#
# Hata sınıfı: bir seçeneğin/argümanın, onu kullanmayan bir komutta sessizce
# kabul edilmesi (eskiden `saqut tokens a.sqt --jit --runs=3` exit 0 veriyordu).
set -eu

binary=$1
root=$2
f="$root/tests/general/cli_dead_option_removal.sqt"

out=$(mktemp); err=$(mktemp)
trap 'rm -f "$out" "$err"' EXIT

# expect_usage_error <stderr'de aranacak metin> <argümanlar...>
expect_usage_error() {
    local needle=$1; shift
    set +e
    "$binary" "$@" >"$out" 2>"$err"
    local actual=$?
    set -e
    if [ "$actual" -ne 64 ]; then
        echo "FAIL: 'saqut $*' beklenen exit 64, gercek $actual" >&2
        cat "$err" >&2
        exit 1
    fi
    if [ -s "$out" ]; then
        echo "FAIL: 'saqut $*' stdout bos olmali: $(cat "$out")" >&2
        exit 1
    fi
    if ! grep -qF -- "$needle" "$err"; then
        echo "FAIL: 'saqut $*' stderr '$needle' icermiyor: $(cat "$err")" >&2
        exit 1
    fi
}

# N1: her komut, kabul etmediği seçeneği reddeder.
expect_usage_error "not valid for 'tokens'"  tokens "$f" --jit
expect_usage_error "not valid for 'tokens'"  tokens "$f" --runs=3
expect_usage_error "not valid for 'check'"   check "$f" --dont-optimize
expect_usage_error "not valid for 'ir'"      ir "$f" --jit
expect_usage_error "not valid for 'ast'"     ast "$f" --gc-stats
expect_usage_error "not valid for 'exec'"    exec "1 + 2" --profile
expect_usage_error "not valid for 'bench'"   bench "$f" --cfg
expect_usage_error "not valid for 'lsp'"     lsp --jit
expect_usage_error "not valid for 'dap'"     dap --verbose
expect_usage_error "not valid for 'check'"   check "$f" -- a b
# Hata mesajı geçerli seçenekleri sayar (#145: symbols için --jsonl yönlendirmesi).
expect_usage_error "--jsonl"                 symbols --json "$f"
expect_usage_error "it takes no options"     tokens "$f" --cfg

# N2: konumsal argüman sayısı komut tanımından.
expect_usage_error "unexpected argument 'x'" lsp x
expect_usage_error "unexpected argument"     tokens "$f" "$f"
expect_usage_error "no input file"           ir
expect_usage_error "no expression given"     exec

# N3: değer alan seçenek değersiz / değer almayan seçenek değerli.
expect_usage_error "requires a value"        ast "$f" --json -o
expect_usage_error "takes no value"          run --jit=1 "$f"
expect_usage_error "invalid value for --runs" bench "$f" --runs=0

# N4: kaldırılmış eski önekli sözdizimi (`file:`) artık dosya yolu değil.
set +e
"$binary" ir "file:$f" >"$out" 2>"$err"
actual=$?
set -e
if [ "$actual" -eq 0 ]; then
    echo "FAIL: 'ir file:<yol>' artik basarili olmamali" >&2
    exit 1
fi

# P1: değer alan seçenekler iki biçimi de kabul eder.
"$binary" bench "$f" --runs 1 --compile-only >/dev/null 2>&1
"$binary" bench "$f" --runs=1 --compile-only >/dev/null 2>&1
"$binary" run --gc-threshold -1 "$f" >/dev/null 2>&1

# P2: LSP/DAP istemcilerinin taşıma bayrağı kabul edilir (no-op).
set +e
"$binary" lsp --stdio </dev/null >/dev/null 2>"$err"
actual=$?
set -e
if [ "$actual" -eq 64 ]; then
    echo "FAIL: 'lsp --stdio' usage error vermemeli: $(cat "$err")" >&2
    exit 1
fi

# P3: yardım komut ve seçenek tablolarından türer — her komut listelenir,
# seçenek satırı onu kabul eden komutları sayar.
"$binary" --help >"$out"
for cmd in run tokens ast symbols check ir exec lsp dap bench; do
    grep -q "^  saqut $cmd" "$out" || { echo "FAIL: yardimda '$cmd' yok" >&2; exit 1; }
done
grep -q -- "--jit .*(run, exec, bench)$" "$out" \
    || { echo "FAIL: --jit satiri (run, exec, bench) demiyor" >&2; exit 1; }
grep -q -- "--json .*(ast)$" "$out" \
    || { echo "FAIL: --json satiri (ast) demiyor" >&2; exit 1; }
if grep -q -- "--optimized\|--stdio" "$out"; then
    echo "FAIL: geriye uyum no-op'lari yardimda gorunmemeli" >&2
    exit 1
fi

echo "cli_command_options: TUM TESTLER GECTI"
