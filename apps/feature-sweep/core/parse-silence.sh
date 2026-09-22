#!/usr/bin/env bash
# parse-silence.sh — problems.md P-31/P-32/P-33 için yeniden üretilebilir kanıt.
# Kullanım: bash apps/feature-sweep/core/parse-silence.sh [saqut-binary]
# Beklenen: "SESSIZ" işaretli satırlar rc=0 ile çalışır (hata verilmesi gerekirken).
set -u
S="${1:-./build/saqut}"
TMP="$(mktemp -d)"
run_case() { # etiket  kaynak
    printf '%s\n' "$2" > "$TMP/m.sqt"
    out="$("$S" run "$TMP/m.sqt" 2>&1)"; rc=$?
    if [ "$rc" = "0" ]; then tag="SESSIZ(rc=0)"; else tag="reddedildi(rc=$rc)"; fi
    printf '%-38s %-14s cikti=[%s]\n' "$1" "$tag" "$(printf '%s' "$out" | tr '\n' '|')"
}
echo "== parse sessiz-yutma (P-31) =="
run_case 'eksik init: int x = ;'      'int main(){ int x = ; print("alive"); return 0; }'
run_case 'eksik ;; int x = 1 print(x)' 'int main(){ int x = 1 print(x); return 0; }'
run_case 'for basliginda eksik ;'      'int main(){ for (int i=0 i<3; i++) {} return 0; }'
run_case 'return eksik ;'              'int main(){ return 0 }'
run_case '(kontrol) kapanmayan blok'   'int main(){ int x = 1;'
run_case '(kontrol) kapanmayan parantez' 'int main(){ print((1+2); return 0; }'
echo "== eksik operand sessiz yanlis deger (P-32) =="
run_case 'print(5 +)'    'int main(){ print(5 +); return 0; }'
run_case 'print(5 * 2 +)' 'int main(){ print(5 * 2 +); return 0; }'
echo "== >>> yanlis sonuc (P-33) =="
run_case 'a >>> 1 (a=8)' 'int main(){ int a=8; print(a >>> 1); print(" "); print(a >> 1); return 0; }'
rm -rf "$TMP"
