#!/usr/bin/env bash
# #299 — Önceliği olan ama ikili anlamı olmayan token'lar (`?`, `:`, `!`, `~`)
# infix konumda sözdizimi hatasıdır. Eskiden BinaryExpression kurulup IR'da
# sessizce 0 üretiliyordu: `int a = 1 ? 2 : 3;` hatasız derlenip 0 basıyordu.
set -eu

binary=$1

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

expect_e901() {
    printf 'int main() {\n    int a = %s;\n    print(a);\n    return 0;\n}\n' "$1" >"$dir/t.sqt"
    set +e
    "$binary" check "$dir/t.sqt" >"$dir/out" 2>&1
    local st=$?
    set -e
    [ "$st" -eq 65 ] || { echo "FAIL '$1': exit 65 bekleniyordu, gercek $st" >&2; cat "$dir/out" >&2; exit 1; }
    grep -q '"code":"E901"' "$dir/out" || { echo "FAIL '$1': E901 yok" >&2; cat "$dir/out" >&2; exit 1; }
    # Tek tanı: kurtarma kapsayan ';'yi tüketmez, ikinci bir E905 doğmaz.
    grep -q '"errors":1,' "$dir/out" || { echo "FAIL '$1': tam 1 hata bekleniyordu" >&2; cat "$dir/out" >&2; exit 1; }
}

expect_e901 '1 ? 2 : 3'
expect_e901 '7 ? 5'
expect_e901 'true ? 1 : 2'
expect_e901 '5 ! 3'
expect_e901 '5 ~ 3'
expect_e901 '1 : 2'

# Pozitif: nullable soneki, cast'te `?`, önek !/~ ve bitsel bileşik atamalar.
cat >"$dir/ok.sqt" <<'EOF'
int main() {
    int? n = null;
    int? m = "12" as int?;
    bool b = !false;
    int x = 6;
    x &= 3;
    x <<= 2;
    print(~x);
    return 0;
}
EOF
out=$("$binary" run "$dir/ok.sqt") || { echo "FAIL: pozitif program derlenmedi" >&2; exit 1; }
[ "$out" = "-9" ] || { echo "FAIL: pozitif program beklenen -9, gercek '$out'" >&2; exit 1; }

echo "non_binary_operator: TUM TESTLER GECTI"
