# Oturum kaydı — GC çekirdeği, optimizasyon doğruluğu, JIT performansı

**Tarih:** 2026-08-27 → 2026-09-22
**Başlangıç:** `8d85b96` (dal `0.9.6`)
**Bitiş:** `3502985` (dal `0.9.8`, push'lu)
**Dallar:** `0.9.6` → PR #234, #235 → `0.9.7` → `0.9.8`

Bu dosya yeni bir oturumun hızlı bağlam alması içindir. Bağlayıcı kaynak
`AGENTS.md` ve ADR'lerdir; burada anlatılanların kanıtı commit mesajlarında
ve issue yorumlarındadır.

---

## 1. Ne yapıldı

### GC çekirdeği yeniden yazıldı (PR #234)

Toplayıcı `src/vm/` altından `src/gc/`'ye taşındı — backend'lerden bağımsız
katman. Gerekçe ölçüldü: `vm/object.hpp`'yi çeken 10 dosyanın 7'si zaten
`src/vm/` dışındaydı.

- **VM ve JIT aynı Heap'i paylaşıyor.** `static Heap jitHeap` kalktı; heap
  ömrü koşuya bağlandı (süreç ömrüne değil).
- **Kök kaydı ters çevrildi.** Heap kökleri bilmez; kökü olan `RootSource`
  olarak kaydolur (`src/gc/gc_roots.hpp`). Per-thread heap yolu açık.
- **Tempo canlı bayta bağlandı** (eskiden nesne sayısı). 20k canlı kayıtta
  **2115ms → 72ms**; ölçek süper-lineerden düze döndü (40k'da 6204→100ms).
- **#217 incremental marking KALDIRILDI.** Yarım kalmıştı: allocate-black
  yoktu, write barrier String'i kapsamıyordu, ve ölçümde marking hiçbir zaman
  birden fazla safepoint'e yayılmıyordu. Issue kapatıldı (supersede: ADR-022).
- **Mark özyinelemesiz** (açık iş listesi) — 300.000 derinlikte zincir
  doğrulandı.
- **ADR-022 yazıldı** — repoda yoktu, dört belge ona atıf yapıyordu.

### Optimizasyon doğruluğu — production'a dokunan iki hata

**`&&` / `||` optimizasyonla farklı sonuç veriyordu:**

| ifade | `run` | `run --optimized` |
|---|---|---|
| `5 && 3` | 3 | 1 |
| `0 \|\| 33` | 33 | 1 |

Sabit katlama 1/0 üretirken IRGenerator operandın değerini üretiyordu. Ürün
sahibi kararıyla **C modeli** seçildi: kısa devre korundu, sonuç 1/0 oldu.
ADR-008 revize edildi.

**Sabit katlama UB kullanıyordu** — UBSan ile kanıtlandı
(`signed integer overflow: 2147483647 + 1`). Aritmetik
`src/core/int_arithmetic.hpp`'de tek kaynağa toplandı; VM ve katlama artık
aynı fonksiyonları çağırıyor.

**Sonuç:** `--optimized` **varsayılan açık** yapıldı, `--dont-optimize`
kapatıyor. `--optimized` no-op olarak kabul ediliyor (geriye uyum).

### JIT performansı — 1.9x'ten 57x'e

Ürün sahibi kriteri: *saf hesapta VM'in en az 20 katı.* Ölçüm 1.9x çıktı.

Kök neden: **hata yayılım kontrolü fonksiyon bazındaydı.** Fonksiyonda tek
bir `print` bulunması, o fonksiyondaki HER talimattan sonra native çağrı
yayılmasına yol açıyordu. Kanıt: aynı döngü, tek fark döngü DIŞINDA bir
`print` → 30ms vs 914ms.

| program | önce | sonra |
|---|---|---|
| saf toplama | 1.9x | **57.4x** |
| çarpma | — | 49.0x |
| DIV'li | 9.6x | 41.9x |
| bölme-yoğun | 5.6x | 19.9x |

### JIT dizi performansı (0.9.8)

| iş | önce | sonra |
|---|---|---|
| `a[i]` okuma | 2.7x* | **~10x** |
| `a[i] = x` | 4.7x | **6.3x** |
| `a.push(x)` | 1.24x | **2.98x** |

\* `float[]` okuma JIT'te hiç çalışmıyordu

**En büyük tek kazanç `rt()` guard'ıydı** — fonksiyon-içi `static`, profilde
**%25.86**. Tek bir `rt_jit_host_call` gövdesinde 28 kez çağrılıyordu.
Namespace kapsamına alındı (thread_local notu korunarak).

### byte semantiği — ADR-040 Faz 4 kapatıldı

```
byte ⊕ byte  → byte, 8 bite SARAR (& 0xFF)
byte ⊕ int   → int  (sarma yok)
int as byte  → aralık denetimli (dış veri doğrulaması)
```

Gerekçe: aynı ifade iki niyete hizmet ediyordu — aritmetik sarma (kripto)
ve dış veri doğrulaması. C/Rust ayrımı alındı. Ergonomi kazancı:
`byte c = a + b;` artık doğrudan yazılabiliyor.

`checkedAdd` ailesi **eklenmeyecek** (ürün sahibi kararı, ADR-040'ta kayıtlı).

### Tanı iyileştirmeleri

- Nullable alan erişimi hint'i çalışmayan kalıp gösteriyordu (#232)
- `300 as byte` çalışma zamanına kalıyordu → derleme zamanında yakalanıyor,
  sarma yolunu gösteriyor
- `string as bool` hint'i `value != 0` diyordu — derlenir ama anlamsız;
  artık kaynak tipe göre ayrışıyor

---

## 2. Bulunan ve düzeltilen hatalar

Hepsi ölçümle bulundu, hepsinin regresyon koruması var:

1. **JIT'te nesne üreten opcode'lar köklenmiyordu** (string cast'leri,
   `STRING_CONCAT`, decimal aritmetiği) → sessiz use-after-free
2. **`&&`/`||` optimizasyonla ayrışıyordu** → production tehlikesi
3. **Sabit katlamada UB** (signed overflow, shift maskeleme, INT_MIN/-1)
4. **`float[]` okuma JIT'te çöküyordu** (VM doğruydu)
5. **`arrayElemKind` iki opcode'da doldurulmuyordu** → inline yol tamamen
   ölü koddu, `Int`/`Byte` bile trampoline gidiyordu
6. **Sınır hatası inline yolda yakalanamıyordu** (koşulsuz RET)
7. **Host dizi dönüşlerinde view senkronu eksikti** → `readFile`'ın
   döndürdüğü `byte[]` JIT'te uzunluk 0 gösteriyordu
8. **`errorCapable` listesi eksikti** (`DADD`/`DSUB`/`DMUL` decimal overflow)
9. **`tests/run.sh`'ta iki gate `set -e` altında sessizce script'i
   sonlandırıyordu** — sonraki tüm testler atlanıyordu

---

## 3. Kanıt durumu

```
bash tests/run.sh          golden 136, diferansiyel VM≡JIT 118,
                           "optimizasyon sonucu degistirmiyor" 306,
                           GC gate'leri (4+1+2+16). Hepsi geçti.

ASan + --gc-threshold=1    VM 139/139, JIT 133/133 temiz
UBSan                      145/145 temiz
Soak (VM)                  110M tur, RSS sabit, canlı küme 40 dilim
                           boyunca 6554, toplama 30008 deterministik
JIT builtin kapsamı        146/147 (%99.3), 0 ayrışma
```

Yeni kalıcı gate'ler: döngüsel referans (sızıntı + yanlış toplama), agresif
eşik kökleme, optimizasyon eşitliği, inline cast, packed indeks erişimi,
hint kalıpları. **Her birinin gerçekten yakaladığı, düzeltme geçici geri
alınarak doğrulandı.**

---

## 4. Açık kalanlar

### Kararı bekleyenler

- **#232** — null daraltma alan erişiminde çalışmıyor. Üç seçenek issue'da;
  önerim safe-call (`d.ic?.deger`). Hint düzeltmesi yapıldı, asıl karar açık.
- **`nogc` / `agc`** — ürün sahibi: "önce mükemmel bir GC gerekli, en sona".
  Çekirdek bastırma sayacına hazır bırakıldı.

### Ölçülmüş ama yapılmamış (#236)

1. **`ARRAY_LEN` opcode'u kullanılmıyor** — `a.length()` `CALLHOST`'a
   gidiyor. Maliyetin %58'i köprüde. **Muhtemelen en ucuz kalan kazanç.**
2. Builtin thunk gövdeleri — VM'i de hızlandırır
3. `hostRegistry()` guard'ı (%1.45, başlatma sırası riski)
4. Ref/Str/Decimal elemanlı diziler inline değil
5. `arrayElemKind` sözleşmesi yazılı değil — hata sınıfı tekrar edebilir
6. `pendingError` adresini gömmek (thread_local geçişini bozar)

### Diğer

- **JIT soak koşusu yapılmadı** — prompt hazır: `jit_soak.md`
- **mprotect W|X** → 1.2 security (#223)
- **`STORE_GLOBAL <nullable operand>`** — JIT'in tek gerçek program sınıfı
  eksiği. Nullable *global* yazma; yerel çalışıyor.
- **#193 / #206 KAPANMADI** — ölçüldü, GC'nin sorunu değil (gerekçe:
  `docs/gc-threading-altyapi-denetimi.md` §D)

---

## 5. Sürüm durumu

| | |
|---|---|
| Yayınlanmış son release | **0.9.4** |
| Aktif dal | **0.9.8** (`saqut --version` doğru) |
| 0.9.5 / 0.9.6 / 0.9.7 | release EDİLMEYECEK — ara çalışma alanı |

Ürün sahibi kararı: **JIT ve VM 1.0'a birlikte stabil girecek.**

**0.9.7'nin kırıcı değişiklikleri** (release notuna girmeli):
- `5 && 3` artık **1** (önce 3), `4 || 8` artık 1
- `-a ^ b` artık **-8** (önce -6); `^` üs değil XOR
- optimizasyon varsayılan açık
- `byte + byte` artık byte döner ve sarar

---

## 6. Ürün kararları (bu oturumda verildi)

- **Thread 1.0'a girmeyecek** (başka bir oturumda karara bağlandı)
- JIT `[EXPERIMENTAL]` etiketi "yapmayacağız" değil "varsayılan
  vermeyeceğiz" demek
- İncremental marking: **B** (sil, STW ilan et)
- `&&`/`||`: **C modeli** (kısa devre var, sonuç 1/0)
- `byte`: tip içi aritmetik sarar, açık cast doğrular
- `checkedAdd` ailesi: **eklenmeyecek**
- Optimizasyon: **varsayılan açık**
- mprotect W|X: **1.2 security**

---

## 7. Çalışma notları (bir sonraki oturuma)

- **`/tmp` kullanma** — proje içinde `temp/` aç (`.gitignore`'da).
- **Adlandırma:** açık ve okunaklı (`gcAllocList` gibi), kısaltma değil.
- **Ölçmeden optimize etme.** Bu oturumda iki kez darboğaz tahmin edildi ve
  ikisinde de yanlış çıktı: köprü sanıldı, `rt()` guard'ı çıktı; thunk
  gövdesi sanıldı, `arrayElemKind` eksikliği çıktı.
- **Gate'lerin yakaladığını doğrula** — düzeltmeyi geçici geri alıp test et.
  Bu oturumda bir fixture'ın koruyamadığı böyle anlaşıldı ve kayda geçti.
- `git push` sırasında kimlik düşebiliyor: `gh auth switch --user
  abdussamedulutas` gerekebiliyor.
