#!/usr/bin/env bash
# #295 — E003 bir hata sınıfına indirildi; eskiden E003 olan tip kuralları
# kendi kodlarına ayrıldı (E020–E027). Her kod için en küçük negatif örnek:
# `saqut check` JSONL'de BEKLENEN kodu ve konumu üretmeli, E003 üretmemeli.
# Ayrıca modül tanıları (E_MODULE_NOT_FOUND) import satırının konumunu taşır.
set -eu

binary=$1

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

# expect <kod> <gövde>  — gövde main() içine yazılır
expect() {
    local code=$1 body=$2
    printf 'struct P { int a; }\nint f() { return 1; }\nint main() {\n%s\n    return 0;\n}\n' "$body" >"$dir/t.sqt"
    set +e
    "$binary" check "$dir/t.sqt" >"$dir/out" 2>&1
    local st=$?
    set -e
    [ "$st" -eq 65 ] || { echo "FAIL [$code]: exit 65 bekleniyordu, gercek $st: $body" >&2; cat "$dir/out" >&2; exit 1; }
    grep -q "\"code\":\"$code\"" "$dir/out" \
        || { echo "FAIL [$code] üretilmedi: $body" >&2; cat "$dir/out" >&2; exit 1; }
    grep -q '"line":4' "$dir/out" \
        || { echo "FAIL [$code] konum satır 4 değil: $body" >&2; cat "$dir/out" >&2; exit 1; }
    if [ "$code" != "E003" ] && grep -q '"code":"E003"' "$dir/out"; then
        echo "FAIL [$code]: E003 de üretildi: $body" >&2; exit 1
    fi
}

expect E003 '    int x = "a";'                       # atama tipi
expect E020 '    int x = 2147483648;'                # literal aralık
expect E020 '    int x = 3.5;'                       # ondalık literal int bağlamında
expect E021 '    bool b = "a" < "b";'                # string sıralama
expect E022 '    int? n = null; int y = n + 1;'      # nullable işlenen
expect E023 '    bool b = 1 as bool;'                # geçersiz as
expect E024 '    switch (1) { case "a": break; }'   # case tipi
expect E025 '    int g = 3; g();'                    # çağrılamaz
expect E026 '    int[] a = [1]; string s = a.toString();'  # yanlış alıcı
expect E027 '    f()++;'                             # atanabilir konum

# Modül tanısı konumu: import edilen dosya yok → import satırı.
printf 'import {x} from "yok.sqt";\nint main() { return 0; }\n' >"$dir/m.sqt"
set +e
"$binary" check "$dir/m.sqt" >"$dir/out" 2>&1
set -e
grep -q '"code":"E_MODULE_NOT_FOUND","column":1,"file":"[^"]*m.sqt","level":"error","line":1' "$dir/out" \
    || { echo "FAIL: E_MODULE_NOT_FOUND import satırının konumunu taşımıyor: $(cat "$dir/out")" >&2; exit 1; }

echo "diagnostic_split: TUM TESTLER GECTI"
