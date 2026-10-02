#!/usr/bin/env bash
# #296 — Token modeli: tür (kind) ve sınıf (category) tokenizer'da belirlenir,
# sözcüksel hatalar tokenizer'ın tanı kanalından gelir, keyword listesi tek
# tablodur (tokenizer/token_kind.hpp KEYWORD_MAP).
#
# Hata sınıfı: token türünün metin karşılaştırmasıyla yeniden keşfedilmesi —
# eskiden `EOL` adlı bir değişken dosya sonu sanılıp tokenizasyonu sessizce
# kesiyordu; keyword tablolarının ayrışması parser'da tanımsız davranıştı.
set -eu

binary=$1

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# P1: EOL adlı değişken programı kesmez.
cat >"$dir/eol.sqt" <<'EOF'
int main() {
    int EOL = 5;
    print(EOL + 1);
    return 0;
}
EOF
out=$("$binary" run "$dir/eol.sqt") || fail "EOL adlı değişkenli program derlenmedi"
[ "$out" = "6" ] || fail "EOL programı beklenen 6, gercek '$out'"

# P2: `date` / `longint` keyword değildir (tip adı identifier yolundan);
# `while` keyword'dür. `saqut tokens` etiketi category'den gelir.
cat >"$dir/kw.sqt" <<'EOF'
int main() {
    int date = 3;
    while (date > 0) { date = date - 1; }
    return date;
}
EOF
tokens=$("$binary" tokens "$dir/kw.sqt")
grep -q '\[identifier\] "date"' <<<"$tokens" || fail "date identifier olmali"
grep -q '\[keyword\] "while"' <<<"$tokens" || fail "while keyword olmali"
grep -q '\[operator\] ">"' <<<"$tokens" || fail "operator etiketi yok"
grep -q '\[delimiter\] "{"' <<<"$tokens" || fail "delimiter etiketi yok"
"$binary" run "$dir/kw.sqt" >/dev/null || fail "date adlı değişkenli program çalışmadı"

# N1: sözcüksel hatalar (tokenizer tanı kanalı) — check JSONL'de kod + konum.
printf 'int main() {\n    print("a\\qb");\n    return 0;\n}\n' >"$dir/esc.sqt"
set +e
"$binary" check "$dir/esc.sqt" >"$dir/out" 2>&1; st=$?
set -e
[ "$st" -eq 65 ] || fail "E906 beklenen exit 65, gercek $st"
grep -q '"code":"E906","column":11' "$dir/out" || fail "E906 konumlu raporlanmadi: $(cat "$dir/out")"

printf 'int main() {\n    print("abc);\n    return 0;\n}\n' >"$dir/unterm.sqt"
set +e
"$binary" check "$dir/unterm.sqt" >"$dir/out" 2>&1; st=$?
set -e
[ "$st" -eq 65 ] || fail "E907 beklenen exit 65, gercek $st"
grep -q '"code":"E907","column":11' "$dir/out" || fail "E907 konumlu raporlanmadi: $(cat "$dir/out")"
# E907 sözdizimi hatalarından önce gelir (tokenizer parse'tan önce çalışır).
first=$(grep -o '"code":"E9[0-9]*"' "$dir/out" | head -1)
[ "$first" = '"code":"E907"' ] || fail "ilk tanı E907 olmali, gercek $first"

echo "token_model: TUM TESTLER GECTI"
