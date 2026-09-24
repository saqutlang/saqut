#!/usr/bin/env bash
# saQut test koşucusu — birim testler + golden testler
# Kullanım: bash tests/run.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-g++}"
FLAGS=(-std=c++20 -Wall -Wextra -I"$ROOT/src")
# SAQUT ortam değişkeniyle başka bir build (Debug / TSan) sınanabilir.
SAQUT="${SAQUT:-$ROOT/build/saqut}"

# ── Birim testler ─────────────────────────────────────────────────────────────
for t in test_type test_diagnostic test_opcode test_value_rep_contract test_cfg test_host_abi test_decimal_core; do
    echo "=== $t ==="
    # test_cfg buildCFG gerçeklemesini (ir_cfg.cpp) da derler — hata sınıfı
    # testi için implementasyon gerekli (#218).
    # test_host_abi StringObject/Heap gerçeklemesini (object.cpp) gerektirir —
    # sınır temsili string'i pointer olarak taşır (#222).
    extra=""
    # test_cfg buildCFG/liveness gerçeklemesini (ir_cfg.cpp + ir_liveness.cpp)
    # da derler — hata sınıfı testi için implementasyon gerekli (#218 + altyapı
    # denetimi: exception kenarı, unreachable temizliği, dominance, liveness).
    [ "$t" = "test_cfg" ] && extra="$ROOT/src/ir/ir_cfg.cpp $ROOT/src/ir/ir_liveness.cpp"
    # test_host_abi gerçek registry'yi çağırır (rt_host_call) — host gövdeleri
    # ve object.cpp gerekir. SAQUT_VERSION normalde CMake'ten gelir.
    # #223: registry built-in metodları src/data/ modüllerinden alır.
    # Host gövdeleri src/ffi/functions/ altında bölünmüştür (organizasyon, #115).
    [ "$t" = "test_host_abi" ] && extra="$ROOT/src/core/utf8.cpp $ROOT/src/gc/gc_heap.cpp $ROOT/src/ffi/host_registry.cpp $ROOT/src/ffi/host_functions.cpp $ROOT/src/ffi/functions/math.cpp $ROOT/src/ffi/functions/fs.cpp $ROOT/src/ffi/functions/sys.cpp $ROOT/src/ffi/functions/date.cpp $ROOT/src/ffi/functions/core.cpp $ROOT/src/ffi/functions/process.cpp $ROOT/src/ffi/functions/io.cpp $ROOT/src/ffi/functions/path.cpp $ROOT/src/ffi/functions/utf8.cpp $ROOT/src/ffi/functions/os.cpp $ROOT/src/data/data_registry.cpp $ROOT/src/data/string.cpp $ROOT/src/data/array.cpp $ROOT/src/data/struct.cpp $ROOT/src/data/date.cpp -DSAQUT_VERSION=\"test\""
    "$CXX" "${FLAGS[@]}" "$ROOT/tests/$t.cpp" $extra -o "/tmp/saqut_$t"
    "/tmp/saqut_$t"
done

# ── Taşınabilirlik denetimi (#224) ────────────────────────────────────────────
# decimal saQut'un sentetik tipidir: aritmetiği C++'a özgü hiçbir şeye
# dayanamaz, çünkü WASM/JS/PHP hedeflerinde aynı sonucu vermek zorundadır.
echo "=== tasinabilirlik: decimal cekirdegi ==="
if grep -n "__int128" "$ROOT/src/data/decimal_core.hpp" "$ROOT/src/core/decimal.hpp" | grep -v "^[^:]*:[0-9]*: *//" | grep -v "//.*__int128"; then
    echo "HATA: decimal cekirdeginde __int128 var — WASM/JS/PHP'ye tasinamaz"
    exit 1
fi
echo "  decimal cekirdegi tasinabilir (yalniz int64/uint64)"

# ── Golden testler ────────────────────────────────────────────────────────────
echo "=== golden ==="

if [ ! -x "$SAQUT" ]; then
    echo "HATA: $SAQUT bulunamadı — önce derleyin (cmake --build build)"
    exit 1
fi

PASS=0; FAIL=0
while IFS= read -r -d '' sqt; do
    dir=$(dirname "$sqt")
    base=$(basename "$sqt" .sqt)
    exp="$dir/$base.expected"
    cerr="$dir/$base.compile_error"
    rerr="$dir/$base.runtime_error"
    xexit="$dir/$base.expected_exit"
    if [ ! -f "$exp" ] && [ ! -f "$cerr" ] && [ ! -f "$rerr" ] && [ ! -f "$xexit" ]; then
        continue
    fi

    # ADR-036 (#76): BASE.flags — --allow-fs vb. gerektiren testler.
    extra_flags=()
    flags_file="$dir/$base.flags"
    if [ -f "$flags_file" ]; then
        mapfile -t extra_flags < "$flags_file"
    fi

    if [ -f "$cerr" ]; then
        # Derleme-hatası fixture'ı: program derlenMEMELİ. stderr beklenen
        # tanıyı içermeli (E-kodu `[$want]` biçiminde ya da sabit mesaj
        # parçası) ve exit code sıfırdan farklı olmalı (.expected_exit
        # varsa tam değeriyle). Sessiz-kabul sınıfını yakalar.
        want=$(cat "$cerr")
        set +e
        err=$("$SAQUT" run "${extra_flags[@]}" "$sqt" 2>&1 >/dev/null)
        rc=$?
        set -e
        if [[ "$want" =~ ^E[0-9]+$ ]]; then
            echo "$err" | grep -q "\[$want\]" && found=1 || found=0
        else
            echo "$err" | grep -qF "$want" && found=1 || found=0
        fi
        if [ -f "$xexit" ]; then
            want_rc=$(cat "$xexit")
            rc_ok=$([ "$rc" -eq "$want_rc" ] && echo 1 || echo 0)
        else
            rc_ok=$([ "$rc" -ne 0 ] && echo 1 || echo 0)
        fi
        if [ "$found" -eq 1 ] && [ "$rc_ok" -eq 1 ]; then
            PASS=$((PASS + 1))
        else
            echo "  FAIL (compile_error): ${sqt#"$ROOT"/}"
            echo "    beklenen : derleme hatası [$want], exit=${want_rc:-nonzero}"
            echo "    gerçek   : exit=$rc, ilk satır: $(echo "$err" | head -1)"
            FAIL=$((FAIL + 1))
        fi
        continue
    fi

    if [ -f "$rerr" ]; then
        # Runtime-hata fixture'ı: program koşar ama tanımlı runtime hatasıyla
        # sonlanır. exit code .expected_exit ile (yoksa 70), stderr
        # .runtime_error regex'iyle eşleşmeli. Sessiz-yanlış-sonuç sınıfını
        # yakalar (ör. sıfıra bölme 0 dönmemeli).
        want_rc=$( [ -f "$xexit" ] && cat "$xexit" || echo 70 )
        want_re=$(cat "$rerr")
        set +e
        out=$("$SAQUT" run "${extra_flags[@]}" "$sqt" 2>/tmp/saqut_rerr)
        rc=$?
        set -e
        err=$(cat /tmp/saqut_rerr)
        # .expected aynı fixture'da varsa: hatadan önceki stdout tam eşit
        # olmalı (ör. hata satırından sonrası çalışmamalı).
        out_ok=1
        if [ -f "$exp" ] && [ "$out" != "$(cat "$exp")" ]; then out_ok=0; fi
        if [ "$out_ok" -eq 1 ] && [ "$rc" -eq "$want_rc" ] && echo "$err" | grep -Eq "$want_re"; then
            PASS=$((PASS + 1))
        else
            echo "  FAIL (runtime_error): ${sqt#"$ROOT"/}"
            echo "    beklenen : exit=$want_rc, stderr ~ /$want_re/"
            echo "    gerçek   : exit=$rc, stderr: $(echo "$err" | head -1)"
            FAIL=$((FAIL + 1))
        fi
        continue
    fi

    actual=$("$SAQUT" run "${extra_flags[@]}" "$sqt" 2>/dev/null) || true
    expected=$(cat "$exp")

    if [ "$actual" = "$expected" ]; then
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${sqt#"$ROOT"/}"
        echo "    beklenen : $(echo "$expected" | head -1)"
        echo "    gerçek   : $(echo "$actual"   | head -1)"
        FAIL=$((FAIL + 1))
    fi
done < <(find "$ROOT/tests/golden" "$ROOT/examples/threading" -name "*.sqt" -print0 | sort -z)

echo "  $PASS geçti, $FAIL başarısız"
[ "$FAIL" -eq 0 ] || exit 1

# ── ADR-045: thread uyarı fixture'ları (.warning) ────────────────────────────
# Uyarı programı bloklamaz; stderr beklenen W-kodunu `[Wxxx]` biçiminde
# içermeli. (E-kodları yukarıdaki .compile_error mekanizmasıyla sınanır.)
echo "=== thread uyarilari ==="
WPASS=0; WFAIL=0
while IFS= read -r -d '' wfile; do
    wsqt="${wfile%.warning}.sqt"
    want=$(cat "$wfile")
    set +e
    werr=$("$SAQUT" run "$wsqt" 2>&1 >/dev/null)
    set -e
    if echo "$werr" | grep -q "\[$want\]"; then
        WPASS=$((WPASS + 1))
    else
        echo "  FAIL (warning): ${wsqt#"$ROOT"/} — beklenen [$want]"
        WFAIL=$((WFAIL + 1))
    fi
done < <(find "$ROOT/tests/golden" -name "*.warning" -print0 | sort -z)
echo "  $WPASS geçti, $WFAIL başarısız"
[ "$WFAIL" -eq 0 ] || exit 1

# ── Diferansiyel test (#92): VM ≡ MIR JIT ────────────────────────────────────
# Her golden fixture'ı hem VM hem JIT ile koşup stdout+exit code'u bayt-bayt
# karşılaştırır. JIT şu an MIR dilimlerinin kapsadığı opcode alt kümesiyle
# sınırlı (mir_backend.hpp) — bir fixture bu kümenin dışına çıkan bir opcode
# içeriyorsa (struct/array/global/try-catch/nullable vb.) JIT programın
# TAMAMINI reddeder (kısmi JIT yok); bu durum parity hatası DEĞİL, henüz
# kapsanmamış bir dilim demektir → SKIP sayılır, FAIL sayılmaz.
echo "=== diferansiyel (VM≡JIT) ==="
DPASS=0; DFAIL=0; DSKIP=0
while IFS= read -r -d '' sqt; do
    dir=$(dirname "$sqt")
    base=$(basename "$sqt" .sqt)
    exp="$dir/$base.expected"
    [ -f "$exp" ] || continue

    extra_flags=()
    flags_file="$dir/$base.flags"
    if [ -f "$flags_file" ]; then
        mapfile -t extra_flags < "$flags_file"
    fi

    set +e
    jit_err=$("$SAQUT" run --jit "${extra_flags[@]}" "$sqt" 2>&1 >/dev/null)
    set -e
    if echo "$jit_err" | grep -q "unsupported opcode"; then
        DSKIP=$((DSKIP + 1))
        continue
    fi

    set +e
    vm_out=$("$SAQUT" run "${extra_flags[@]}" "$sqt" 2>/dev/null); vm_exit=$?
    jit_out=$("$SAQUT" run --jit "${extra_flags[@]}" "$sqt" 2>/dev/null); jit_exit=$?
    set -e

    if [ "$vm_out" = "$jit_out" ] && [ "$vm_exit" = "$jit_exit" ]; then
        DPASS=$((DPASS + 1))
    elif [ -f "$dir/$base.jit_known_broken" ]; then
        # CMakeLists.txt'teki WILL_FAIL mekanizmasıyla aynı fikir: JIT bu
        # fixture'da bilinen, ayrı issue'da kayıtlı bir crash/parity hatası
        # üretiyor. VM (normatif) doğru; JIT [EXPERIMENTAL], parity bu
        # release kapısının şartı değil (AGENTS.md §9).
        DSKIP=$((DSKIP + 1))
    else
        echo "  FAIL (parity): ${sqt#"$ROOT"/} (vm_exit=$vm_exit jit_exit=$jit_exit)"
        echo "    VM  : $(echo "$vm_out"  | head -1)"
        echo "    JIT : $(echo "$jit_out" | head -1)"
        DFAIL=$((DFAIL + 1))
    fi
done < <(find "$ROOT/tests/golden" "$ROOT/examples/threading" -name "*.sqt" -print0 | sort -z)

echo "  $DPASS geçti, $DFAIL başarısız, $DSKIP atlandı (JIT henüz desteklemiyor)"
[ "$DFAIL" -eq 0 ] || exit 1

# ── Modül döngüsü testleri (ADR-031, #78) ────────────────────────────────────
# Döngüsel bağımlılık E_MODULE_CYCLE tanısı + sıfır-dışı exit üretmeli.
echo "=== modül döngüsü ==="
for f in cycle_a self_import; do
    if out=$("$SAQUT" check "$ROOT/tests/module/$f.sqt" 2>/dev/null); then
        echo "  FAIL: $f.sqt — döngüde exit 0 döndü"; exit 1
    fi
    if ! echo "$out" | grep -q "E_MODULE_CYCLE"; then
        echo "  FAIL: $f.sqt — E_MODULE_CYCLE tanısı yok"; exit 1
    fi
done
echo "  2 geçti, 0 başarısız"

# ── GC testleri (ADR-022) ────────────────────────────────────────────────────
# Eşik BAYT tabanlıdır: toplama, canlı ayak izi eşiği aşınca tetiklenir.
# (Nesne SAYISI tabanlı eşik, 10 baytlık string ile 10 MB'lık byte[]'i aynı
# ağırlıkta saydığı için tempoyu programın şekline göre bozuyordu.)
#
# 1) Eşik düşürülünce GC gerçekten tetikleniyor (collections >= 1)
# 2) Toplama çıktıyı DEĞİŞTİRMEZ — hangi eşikte olursa olsun aynı sonuç
# 3) --gc-threshold=-1 otomatik toplamayı kapatıyor (collections=0)
# 4) VM ve JIT aynı GC çekirdeğini kullanır: ikisi de sayaç raporlar
echo "=== gc ==="
GC_SQT="$ROOT/tests/golden/gc/liveness.sqt"
gcout=$("$SAQUT" run --gc-threshold=4096 --gc-stats "$GC_SQT" 2>&1 >/dev/null)
if ! echo "$gcout" | grep -Eq "gc: collections=[1-9]"; then
    echo "  FAIL: düşük eşikte GC hiç koşmadı: $gcout"; exit 1
fi
normal=$("$SAQUT" run "$GC_SQT" 2>/dev/null)
for th in 1 4096 65536; do
    stress=$("$SAQUT" run --gc-threshold=$th "$GC_SQT" 2>/dev/null)
    if [ "$stress" != "$normal" ]; then
        echo "  FAIL: --gc-threshold=$th çıktıyı değiştirdi (toplama gözlemlenebilir olmamalı)"; exit 1
    fi
done
gcoff=$("$SAQUT" run --gc-threshold=-1 --gc-stats "$GC_SQT" 2>&1 >/dev/null)
if ! echo "$gcoff" | grep -q "collections=0"; then
    echo "  FAIL: --gc-threshold=-1 toplamayı kapatmadı: $gcoff"; exit 1
fi
jitgc=$("$SAQUT" run --jit --gc-stats "$GC_SQT" 2>&1 >/dev/null)
if ! echo "$jitgc" | grep -q "gc: collections="; then
    echo "  FAIL: JIT yolunda GC sayaçları raporlanmıyor: $jitgc"; exit 1
fi
echo "  4 geçti, 0 başarısız"

# ── GC kök daraltma (liveness) ───────────────────────────────────────────────
# narrow_proof.sqt: erken kullanılıp ÖLEN (üzerine yazılmayan) 5 array +
# 3000 turluk tahsis döngüsü. Liveness tabanlı kök daraltma etkinse ölü
# array'ler toplanır; daraltma kırılırsa sona kadar kök kalırlar.
#
# Sayım tabanlı iddia: koşu sonunda CANLI kalan nesne sayısı, ölü array'ler
# toplanmadığı durumdan kesin olarak azdır. Zamana değil olaya bağlıdır,
# dolayısıyla makineden ve backend'den bağımsız tekrarlanabilir.
echo "=== gc kok daraltma ==="
NARROW_SQT="$ROOT/tests/golden/gc/narrow_proof.sqt"
nout=$("$SAQUT" run --gc-threshold=4096 --gc-stats "$NARROW_SQT" 2>&1 >/dev/null)
nfreed=$(echo "$nout" | grep -oE "freed=[0-9]+" | head -1 | cut -d= -f2)
nlive=$(echo "$nout" | grep -oE "live=[0-9]+" | head -1 | cut -d= -f2)
if [ -z "$nfreed" ] || [ "$nfreed" -lt 2046 ]; then
    echo "  FAIL: kök daraltma etkin değil (freed=${nfreed:-yok}, beklenen >= 2046)"; exit 1
fi
if [ -z "$nlive" ] || [ "$nlive" -gt 64 ]; then
    echo "  FAIL: ölü slot'lar canlı tutuluyor (live=${nlive:-yok}, beklenen <= 64)"; exit 1
fi
echo "  1 geçti, 0 başarısız (freed=$nfreed live=$nlive)"

# ── Döngüsel referans: sızıntı YOK, yanlış toplama YOK ───────────────────────
# Mark-sweep'in ref-count'a göre asıl üstünlüğü: erişilebilirlik tabanlı
# olduğu için döngü özel bir durum değildir.
#
# SAYIM TABANLI iddia: aynı programı farklı TUR sayılarıyla koştuğumuzda
# canlı küme SABİT kalmalı. Sızıntı olsaydı tur sayısıyla orantılı büyürdü.
# Zamana değil olaya bağlı olduğu için makineden bağımsız tekrarlanabilir.
echo "=== gc dongusel referans ==="
CYC_SQT="$ROOT/tests/golden/gc/dongusel_sizinti.sqt"
prev_live=""
for tur in 5 10 20; do
    sed "s/tur < 20/tur < $tur/" "$CYC_SQT" > "$ROOT/tests/golden/gc/.dongu_tmp.sqt"
    live=$("$SAQUT" run --gc-stats "$ROOT/tests/golden/gc/.dongu_tmp.sqt" 2>&1 >/dev/null \
           | grep -oE "live=[0-9]+" | head -1 | cut -d= -f2)
    if [ -z "$live" ]; then
        echo "  FAIL: canlı küme okunamadı (tur=$tur)"; exit 1
    fi
    if [ -n "$prev_live" ] && [ "$live" != "$prev_live" ]; then
        echo "  FAIL: döngüsel graf sızıyor — tur=$tur'de live=$live, öncekinde $prev_live"
        rm -f "$ROOT/tests/golden/gc/.dongu_tmp.sqt"; exit 1
    fi
    prev_live="$live"
done
rm -f "$ROOT/tests/golden/gc/.dongu_tmp.sqt"
# Canlı döngü YANLIŞLIKLA toplanmamalı: iki backend de doğru çıktı vermeli
cycexp=$(cat "$ROOT/tests/golden/gc/dongusel_referans.expected")
for backend in "" "--jit"; do
    got=$("$SAQUT" run $backend --gc-threshold=1 "$ROOT/tests/golden/gc/dongusel_referans.sqt" 2>/dev/null)
    if [ "$got" != "$cycexp" ]; then
        echo "  FAIL: canlı döngü ${backend:-vm} agresif eşikte bozuldu (beklenen '$cycexp', gelen '$got')"; exit 1
    fi
done
echo "  2 geçti, 0 başarısız (canlı küme sabit: $prev_live)"

# ── GC agresif eşik: nesne üreten opcode köklemesi ───────────────────────────
# --gc-threshold=1 ile HER tahsis bir toplama tetikler. Bu, "nesne üretti ama
# GC'ye görünür kılmadı" sınıfını açığa çıkaran en dar penceredir: köklenmemiş
# bir nesne, üretildiği talimat ile okunduğu talimat arasında süpürülür.
#
# Varsayılan eşikte bu hata GÖRÜNMEZ — bulunduğunda da öyleydi (CAST_FLOAT32_TO_STR,
# tests/golden/ir/wide_cast_operands.sqt). Bu yüzden gate agresif eşik kullanır.
echo "=== gc agresif esik koklemesi ==="
agpass=0; agfail=0
for gf in "$ROOT"/tests/golden/gc/*.sqt "$ROOT"/tests/golden/ir/wide_cast_operands.sqt; do
    [ -f "$gf" ] || continue
    exp="${gf%.sqt}.expected"
    [ -f "$exp" ] || continue
    for backend in "" "--jit"; do
        got=$("$SAQUT" run $backend --gc-threshold=1 "$gf" 2>/dev/null)
        # JIT desteklemiyorsa bu fixture o backend'de atlanır
        if [ -z "$backend" ] || ! "$SAQUT" run --jit "$gf" 2>&1 | grep -q "cannot compile this program completely"; then
            if [ "$got" != "$(cat "$exp")" ]; then
                echo "  FAIL: $(basename "$gf") ${backend:-vm} agresif eşikte çıktı bozuldu"
                agfail=$((agfail+1))
            else
                agpass=$((agpass+1))
            fi
        fi
    done
done
if [ "$agfail" -gt 0 ]; then exit 1; fi
echo "  $agpass geçti, 0 başarısız"

# ── Optimizasyon sonucu DEĞİŞTİRMEZ (ADR-038) ────────────────────────────────
# Optimizasyon VARSAYILAN OLARAK AÇIKTIR; --dont-optimize kapatır. Yani
# varsayılan koşu = production koşusu. Optimizasyonun gözlenen çıktıyı
# değiştirmemesi bir performans meselesi değil, DOĞRULUK sözleşmesidir:
# "aynı program, backend ve optimizasyon ne olursa olsun aynı sonuç".
#
# Bu gate gerçek bir ayrışma yakaladı: sabit katlama && / || için 1/0
# üretirken IRGenerator operandın değerini döndürüyordu (`5 && 3` → katlamada
# 1, VM'de 3). ADR-008 C modeline göre revize edildi; gate kalıcı korumadır.
#
# stdout karşılaştırılır — stderr değil: optimizasyon ek tanı üretebilir
# (W002 sıfıra bölme, W003 ulaşılamayan kod) ve bu KASITLIDIR.
echo "=== optimizasyon sonucu degistirmiyor ==="
optpass=0; optfail=0
while IFS= read -r f; do
    for backend in "" "--jit"; do
        # JIT desteklemiyorsa o backend'de atla
        if [ -n "$backend" ] && "$SAQUT" run --jit "$f" </dev/null 2>&1 | grep -q "cannot compile this program completely"; then
            continue
        fi
        # Kasten hata veren fixture'lar sıfırdan farklı exit döndürür
        # (mod_by_zero, cast hataları...). Karşılaştırdığımız şey stdout;
        # `|| true` olmadan set -e bu fixture'da script'i sonlandırır.
        optimized=$("$SAQUT" run $backend "$f" </dev/null 2>/dev/null || true)
        plain=$("$SAQUT" run $backend --dont-optimize "$f" </dev/null 2>/dev/null || true)
        if [ "$optimized" != "$plain" ]; then
            echo "  FAIL: $(basename "$f") ${backend:-vm} — optimizasyon stdout'u degistirdi"
            optfail=$((optfail+1))
        else
            optpass=$((optpass+1))
        fi
    done
done < <(find "$ROOT/tests/golden" "$ROOT/examples/threading" -name '*.sqt' | sort)
if [ "$optfail" -gt 0 ]; then exit 1; fi
echo "  $optpass geçti, 0 başarısız"

# ── Builtin sözdizimi testleri (ADR-033, #85) ────────────────────────────────
# 1) Eski ElemTip::metod sözdizimi W006 uyarısı verir ama çalışır (exit 0)
# 2) Struct alanı builtin'i gölgeler: k.length() alan varken derleme hatası
echo "=== builtin sözdizimi ==="
legout=$("$SAQUT" check "$ROOT/tests/semantic/legacy_builtin.sqt" 2>/dev/null) || {
    echo "  FAIL: legacy_builtin.sqt derlenmeliydi (yalnızca W)"; exit 1; }
if ! echo "$legout" | grep -q "W006"; then
    echo "  FAIL: eski sözdizimi W006 uyarısı üretmedi"; exit 1
fi
if out=$("$SAQUT" check "$ROOT/tests/semantic/field_shadow.sqt" 2>/dev/null); then
    echo "  FAIL: field_shadow.sqt derlenmemeliydi (alan gölgeleme)"; exit 1
fi
if ! echo "$out" | grep -q "is a field of struct"; then
    echo "  FAIL: alan gölgeleme tanısı beklenen mesajı içermiyor"; exit 1
fi
echo "  2 geçti, 0 başarısız"

# ── requires uyarısı (ADR-043, #229) ────────────────────────────────────────
# requires <cap> grameri parse edilir; capability enforcement kaldırıldığı
# için kullanım DERLEME ZAMANI uyarısıdır (W007) — program yine derlenir ve
# çalışır. Yeni .expected_warning marker'ı yalnız bu fixture kullanır;
# mevcut testler zayıflamaz.
echo "=== requires uyarısı ==="
RW_SQT="$ROOT/tests/golden/ffi/requires_warning.sqt"
rw_want=$(cat "${RW_SQT%.sqt}.expected_warning")
rw_out=$("$SAQUT" check "$RW_SQT" 2>&1)
if ! echo "$rw_out" | grep -q "W007"; then
    echo "  FAIL: requires uyarısı [$rw_want] görünmedi: $(echo "$rw_out" | head -1)"
    exit 1
fi
rw_run=$("$SAQUT" run "$RW_SQT" 2>/dev/null)
rw_exp=$(cat "${RW_SQT%.sqt}.expected")
if [ "$rw_run" != "$rw_exp" ]; then
    echo "  FAIL: requires fixture çıktısı farklı (beklenen: $rw_exp)"
    exit 1
fi
echo "  1 geçti, 0 başarısız"

echo "=== TUM TESTLER GECTI ==="
