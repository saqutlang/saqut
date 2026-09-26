#!/usr/bin/env bash
# ============================================================================
# saQut Stage Harness — #207 (STAB-001-CENTRAL-VALIDATION-HARNESS)
# ============================================================================
#
# AMAÇ:
#   Tek bir .sqt kaynağını pipeline'ın HER aşamasından ayrı ayrı geçirip
#   sonuçları tek kayıtta toplar. Mevcut golden testler stdout black-box'tır —
#   tek katman proxy'si (AGENTS.md §8). Bu araç aşamaları YAN YANA koyup
#   aralarındaki ÇELİŞKİYİ arar:
#
#     tokens → ast → check → ir → run (VM) → run --jit (JIT)
#
#   Aranan çelişki sınıfları:
#     C1  parser kabul etti ama semantic reddetti  (parse başarısı ≠ derleme)
#     C2  semantic kabul etti ama IR üretilemedi
#     C3  IR üretildi ama VM çöktü
#     C4  VM ile JIT farklı stdout/exit üretti     (parity — normatif: VM)
#     C5  aşama çöktü (signal/timeout)
#     C6  bir backend timeout'a düştü, diğeri bitti (parity DEĞİL: perf farkı)
#
# TASARIM NOTU — ctest'e BAĞLANMAZ:
#   Bu bir keşif aracıdır, regresyon kapısı değil. Kırmızı bulgular atomik
#   issue'ya bölünür; bir aday YEŞİL olduğunda tests/golden/ altına fixture
#   olarak terfi eder ve ASIL o zaman ctest'e girer. Harness'i ctest'e bağlamak
#   "şu an kırmızı olan her şey suite'i kırar" demek olurdu.
#
# KULLANIM:
#   tests/harness/stage_harness.sh <dosya.sqt> [...]        # tek/çok dosya
#   tests/harness/stage_harness.sh --corpus                 # golden + examples
#   tests/harness/stage_harness.sh --corpus --jsonl rapor.jsonl
#
# SEÇENEKLER:
#   --corpus          tests/golden/**/*.sqt + examples/*.sqt taranır
#   --jsonl <dosya>   makine-okunur JSONL rapor (issue'ya eklenebilir)
#   --timeout <sn>    aşama başına zaman sınırı (varsayılan 20)
#   --quiet           yalnız çelişkili dosyaları bas
#   --binary <yol>    saqut binary yolu (varsayılan build/saqut)
#
# ÇIKIŞ KODU:
#   0  hiç çelişki yok
#   1  en az bir çelişki bulundu
#   2  kullanım/ortam hatası
# ============================================================================

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BINARY="$ROOT/build/saqut"
TIMEOUT=20
QUIET=0
JSONL=""
CORPUS=0
FILES=()

while [ $# -gt 0 ]; do
    case "$1" in
        --corpus)  CORPUS=1; shift ;;
        --jsonl)   JSONL="${2:-}"; shift 2 ;;
        --timeout) TIMEOUT="${2:-20}"; shift 2 ;;
        --quiet)   QUIET=1; shift ;;
        --binary)  BINARY="${2:-}"; shift 2 ;;
        -h|--help) sed -n '2,45p' "${BASH_SOURCE[0]}"; exit 0 ;;
        -*)        echo "bilinmeyen seçenek: $1" >&2; exit 2 ;;
        *)         FILES+=("$1"); shift ;;
    esac
done

if [ ! -x "$BINARY" ]; then
    echo "HATA: binary bulunamadı/çalıştırılabilir değil: $BINARY" >&2
    echo "önce derleyin: cmake --build build" >&2
    exit 2
fi

if [ "$CORPUS" -eq 1 ]; then
    while IFS= read -r -d '' f; do FILES+=("$f"); done \
        < <(find "$ROOT/tests/golden" "$ROOT/examples" -name '*.sqt' -print0 2>/dev/null | sort -z)
fi

if [ ${#FILES[@]} -eq 0 ]; then
    echo "HATA: dosya verilmedi (--corpus veya dosya yolu gerekli)" >&2
    exit 2
fi

# ── Provenance — hangi binary, hangi commit ölçtü ───────────────────────────
COMMIT="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
DIRTY=""
[ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null)" ] && DIRTY=" (dirty)"
BIN_SHA="$(sha256sum "$BINARY" 2>/dev/null | cut -d' ' -f1)"

echo "=== saQut Stage Harness (#207) ==="
echo "commit    : $COMMIT$DIRTY"
echo "binary    : $BINARY"
echo "sha256    : $BIN_SHA"
echo "dosya     : ${#FILES[@]}"
echo "timeout   : ${TIMEOUT}s / aşama"
echo

[ -n "$JSONL" ] && : > "$JSONL"

# ── Tek aşamayı koştur: exit kodunu döndür, stdout/stderr'i dosyaya al ──────
# $1 çıktı dosyası öneki, geri kalanı komut. Global STAGE_EXIT'i set eder.
run_stage() {
    local out="$1"; shift
    # Bir aşama SIGSEGV alırsa bash çocuğun ölümünü KENDİ stderr'ine
    # ("Parçalama arızası") bildirir; bu harness raporuna karışır ve
    # bastırılamaz çünkü mesajı yazan kabuğun kendisidir. Çözüm: komutu ayrı
    # bir `bash -c` altında koştur — bildirim o kabuğun stderr'ine gider ve
    # orada yutulur. Çökme BİLGİSİ kaybolmuyor: exit kodu (>=128) korunur ve
    # C5 olarak raporlanır.
    bash -c 'timeout "$1" "${@:2}" >"$0.out" 2>"$0.err"' \
         "$out" "$TIMEOUT" "$@" 2>/dev/null
    STAGE_EXIT=$?
}

jesc() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr -d '\000-\037'; }

TOTAL=0; CLEAN=0; CONFLICT=0
declare -A CLASS_COUNT

for f in "${FILES[@]}"; do
    TOTAL=$((TOTAL+1))
    rel="${f#$ROOT/}"
    tmp="$(mktemp -d)"

    # BASE.flags — golden fixture'ların gerektirdiği --allow-fs vb. (ADR-036)
    extra=()
    flags_file="${f%.sqt}.flags"
    if [ -f "$flags_file" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] && extra+=($line)
        done < "$flags_file"
    fi

    run_stage "$tmp/tokens" "$BINARY" tokens "$f";              e_tok=$STAGE_EXIT
    run_stage "$tmp/ast"    "$BINARY" ast    "$f";              e_ast=$STAGE_EXIT
    run_stage "$tmp/check"  "$BINARY" check  "$f";              e_chk=$STAGE_EXIT
    run_stage "$tmp/ir"     "$BINARY" ir     "$f";              e_ir=$STAGE_EXIT
    run_stage "$tmp/vm"     "$BINARY" run "${extra[@]}" "$f";   e_vm=$STAGE_EXIT
    run_stage "$tmp/jit"    "$BINARY" run --jit "${extra[@]}" "$f"; e_jit=$STAGE_EXIT

    # JIT bu fixture'ı derlemiyorsa parity karşılaştırması ANLAMSIZDIR —
    # UNSUPPORTED'dır, PASS değil (#207 kabul kriteri 3).
    jit_state="ok"
    if grep -qi "desteklenmeyen" "$tmp/jit.err" 2>/dev/null; then
        jit_state="unsupported"
    fi

    conflicts=()

    # C1 — parser kabul, semantic ret
    [ "$e_ast" -eq 0 ] && [ "$e_chk" -ne 0 ] && conflicts+=("C1:parser-ok/semantic-ret")
    # C2 — semantic kabul, IR üretilemedi
    [ "$e_chk" -eq 0 ] && [ "$e_ir" -ne 0 ] && conflicts+=("C2:semantic-ok/ir-hata")
    # C3 — IR üretildi ama VM beklenmedik şekilde çöktü (>=128: signal)
    [ "$e_ir" -eq 0 ] && [ "$e_vm" -ge 128 ] && conflicts+=("C3:ir-ok/vm-crash(exit=$e_vm)")
    # C6 — bir backend timeout'a düştü, diğeri bitti. Bu bir PARITY hatası
    # DEĞİLDİR: kesilen koşunun çıktısı zaten eksiktir, karşılaştırmak sahte
    # C4 üretir. Ama sessizce geçilecek bir şey de değil — backend'ler
    # arasında en az timeout kadar performans farkı var demektir.
    if { [ "$e_vm" -eq 124 ] && [ "$e_jit" -ne 124 ]; } ||
       { [ "$e_jit" -eq 124 ] && [ "$e_vm" -ne 124 ]; }; then
        slow="vm"; [ "$e_jit" -eq 124 ] && slow="jit"
        conflicts+=("C6:$slow-timeout(>${TIMEOUT}s, diğeri bitti)")
    # C4 — VM≡JIT parity (yalnız JIT gerçekten derlediyse; VM normatif)
    elif [ "$jit_state" = "ok" ] && [ "$e_vm" -ne 124 ] && [ "$e_jit" -ne 124 ]; then
        if ! cmp -s "$tmp/vm.out" "$tmp/jit.out"; then
            conflicts+=("C4:vm!=jit-stdout")
        elif [ "$e_vm" -ne "$e_jit" ]; then
            conflicts+=("C4:vm!=jit-exit($e_vm/$e_jit)")
        fi
    fi
    # C5 — timeout (124) veya signal herhangi bir aşamada
    for pair in "tokens:$e_tok" "ast:$e_ast" "check:$e_chk" "ir:$e_ir"; do
        st="${pair%%:*}"; ec="${pair##*:}"
        [ "$ec" -eq 124 ] && conflicts+=("C5:$st-timeout")
        [ "$ec" -ge 128 ] && conflicts+=("C5:$st-signal($ec)")
    done

    if [ ${#conflicts[@]} -eq 0 ]; then
        CLEAN=$((CLEAN+1))
        [ "$QUIET" -eq 0 ] && printf '  [temiz] %s\n' "$rel"
    else
        CONFLICT=$((CONFLICT+1))
        printf '  [ÇELİŞKİ] %s\n' "$rel"
        for c in "${conflicts[@]}"; do
            printf '            %s\n' "$c"
            CLASS_COUNT["${c%%:*}"]=$(( ${CLASS_COUNT["${c%%:*}"]:-0} + 1 ))
        done
        # İlk hata satırını göster — kök nedene hızlı işaret
        for s in check ir vm; do
            if [ -s "$tmp/$s.err" ]; then
                printf '            %s.err: %s\n' "$s" "$(head -1 "$tmp/$s.err" | cut -c1-100)"
                break
            fi
        done
    fi

    if [ -n "$JSONL" ]; then
        cj=$(IFS=,; echo "${conflicts[*]:-}")
        printf '{"file":"%s","tokens":%d,"ast":%d,"check":%d,"ir":%d,"vm":%d,"jit":%d,"jit_state":"%s","conflicts":"%s","commit":"%s","binary_sha256":"%s"}\n' \
            "$(jesc "$rel")" "$e_tok" "$e_ast" "$e_chk" "$e_ir" "$e_vm" "$e_jit" \
            "$jit_state" "$(jesc "$cj")" "$COMMIT" "$BIN_SHA" >> "$JSONL"
    fi

    rm -rf "$tmp"
done

echo
echo "=== Özet ==="
echo "toplam    : $TOTAL"
echo "temiz     : $CLEAN"
echo "çelişkili : $CONFLICT"
if [ "${#CLASS_COUNT[*]}" -gt 0 ]; then
    echo "sınıf dağılımı:"
    for k in "${!CLASS_COUNT[@]}"; do
        echo "  $k: ${CLASS_COUNT[$k]}"
    done | sort
fi
[ -n "$JSONL" ] && echo "jsonl     : $JSONL"

[ "$CONFLICT" -gt 0 ] && exit 1
exit 0
