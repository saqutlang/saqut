#!/usr/bin/env bash
# RG-7 (#157): saqut ast ve saqut symbols, run/check/ir ile aynı diagnostic
# kapısından geçmeli — önceden ast HİÇBİR semantic hatayı kontrol etmiyordu
# (her zaman exit 0), symbols literal exit kodu kullanıyordu.
set -eu

binary=$1
out=$(mktemp); err=$(mktemp)
trap 'rm -f "$out" "$err"' EXIT

clean=$(mktemp --suffix=.sqt)
semantic_error=$(mktemp --suffix=.sqt)
trap 'rm -f "$out" "$err" "$clean" "$semantic_error"' EXIT

printf 'int main() { print(1); return 0; }\n' > "$clean"
printf 'int main() { print(undefined_thing); return 0; }\n' > "$semantic_error"

# ast: temiz girdi -> exit 0, AST basar.
set +e
"$binary" ast "$clean" > "$out" 2>"$err"
actual=$?
set -e
test "$actual" -eq 0
test -s "$out"

# ast: semantic hatalı girdi -> 360bad2 sözleşmesi: ast'in tanı akışı YOK.
# Hatalı kodda kısmi AST basılır, çıkış kodu 0, tanılar gösterilmez (tanı
# için `saqut check`). (Eski #157 beklentisi — exit 65, AST basılmaz —
# bu ürün kararıyla geçersizleşti.)
set +e
"$binary" ast "$semantic_error" > "$out" 2>"$err"
actual=$?
set -e
if [ "$actual" -ne 0 ]; then
    echo "FAIL: ast semantic hatada beklenen exit 0 (tanı akışı yok), gercek $actual" >&2
    exit 1
fi
if ! grep -q "undefined_thing" "$out"; then
    echo "FAIL: ast semantic hatada kismi AST basilmali, gercek: $(cat "$out")" >&2
    exit 1
fi
if grep -q "E001" "$err"; then
    echo "FAIL: ast tani basmamali, stderr: $(cat "$err")" >&2
    exit 1
fi

# symbols: temiz girdi -> exit 0.
set +e
"$binary" symbols "$clean" > "$out" 2>"$err"
actual=$?
set -e
test "$actual" -eq 0

# symbols: SymbolCollector'in kendi tespit ettigi hata (tanimsiz degisken)
# -> exit 65 (merkezi kDataError, artik literal 1 degil).
set +e
"$binary" symbols "$semantic_error" > "$out" 2>"$err"
actual=$?
set -e
if [ "$actual" -ne 65 ]; then
    echo "FAIL: symbols SymbolCollector hatasinda beklenen exit 65, gercek $actual" >&2
    exit 1
fi
