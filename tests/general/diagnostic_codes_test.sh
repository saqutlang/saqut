#!/usr/bin/env bash
# #295 — Tanı / hata kodlarının tek kaydı (src/diagnostic/diagnostic.hpp).
#
# Hata sınıfı: kaynakta kayıtsız bir kodla rapor üretmek (yazım hatası, belgede
# olmayan kod) ya da kayıtta hiç üretilmeyen ölü kod bırakmak.
#
#   1) src/ altında (vendor hariç) geçen her "E…"/"W…" kod literali kayıtlı.
#   2) Kayıttaki her kod kaynakta en az bir kez üretiliyor.
# Kayıt listesi gen_diagnostic_docs çıktısından okunur (belgeyle aynı kaynak).
set -eu

gen=$1
root=$2

catalog=$(mktemp); source_codes=$(mktemp)
trap 'rm -f "$catalog" "$source_codes"' EXIT

# Tablo satırları: "| E003 | ..." ve "| `E_DIVZERO` | ..."
"$gen" | grep -oE '^\| `?(E[0-9]{3}|E_[A-Z_]+|W[0-9]{3})`? \|' \
       | grep -oE '(E[0-9]{3}|E_[A-Z_]+|W[0-9]{3})' | sort -u >"$catalog"

grep -rhoE '"(E[0-9]{3}|E_[A-Z_]+|W[0-9]{3})"' "$root/src" \
     --include='*.cpp' --include='*.hpp' \
     --exclude-dir=vendor --exclude=diagnostic.hpp \
    | tr -d '"' | sort -u >"$source_codes"

[ -s "$catalog" ] || { echo "FAIL: kayıt listesi okunamadı" >&2; exit 1; }

unregistered=$(comm -23 "$source_codes" "$catalog")
if [ -n "$unregistered" ]; then
    echo "FAIL: kaynakta kullanılan ama kayıtta olmayan kodlar:" >&2
    echo "$unregistered" >&2
    echo "→ src/diagnostic/diagnostic.hpp tablosuna ekleyin" >&2
    exit 1
fi

unused=$(comm -13 "$source_codes" "$catalog")
if [ -n "$unused" ]; then
    echo "FAIL: kayıtta olup kaynakta hiç üretilmeyen kodlar:" >&2
    echo "$unused" >&2
    exit 1
fi

echo "diagnostic_codes: TUM TESTLER GECTI ($(wc -l <"$catalog") kod)"
