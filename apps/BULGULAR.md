# BULGULAR — 1.0 öncesi gerçek uygulama testi

Bu dosya, gerçek terminal uygulamaları yazılırken karşılaşılan hataları,
doküman-gerçek sapmalarını ve ölçülmüş kısıtları kaydeder.

- **Aktif sürüm:** `0.9.9`, dal `0.9.9`
- **Normatif backend:** VM (`./build/saqut run`); `--jit` yalnız karşılaştırma
- **Başlangıç HEAD:** `387006e` (temizlik commit'i: `09d0d04`)
- **Durum etiketleri:** Doğrulandı / Doğrulanmadı / Karar bekliyor
- Her bulgu, en ucuz deneyle (küçük `.sqt` + gözlenen çıktı) kanıtlanmıştır.
  Issue açmak ürün sahibi kararıdır; burada yalnız kayıt tutulur.

Bu turdaki uygulamalar: `apps/xml-tool/` (XML CLI), `apps/sha256-tool/`
(SHA-256 CLI) ve `apps/btree-kv/` (B-tree KV + REPL). Ayrıca izole doğrulama
probları (`/tmp/sqt-probe`) kullanıldı; kalıcı kanıt komutları aşağıdadır.

---

## B-01 — `date as longint` değeri VM'de int32 gibi karşılaştırılıyor (VM ≢ JIT)

**Sınıf:** gerçeklik hatası / VM ≢ JIT (determinizm sözleşmesi, ADR-038)
**Durum:** Doğrulandı, karar bekliyor
**İlişkili:** #113 (kapalı — sayısal genişlik tutarsızlıkları, artık), #127
(date dilimi), #130 (tip temsili backend patlaması)

### Gözlem

`date` tipindeki bir değeri `longint`'e çevirip karşılaştırınca VM yanlış
sonuç veriyor; JIT doğru veriyor. Değerin kendisi ve aritmetiği doğru.

```c
// /tmp/sqt-probe/dcast.sqt
import { fromEpochMillis, toEpochMillis } from date;
int main() {
    date d = fromEpochMillis(1790092890942);
    longint a = d as longint;
    print("a = ");        print(a as string); print("\n");
    print("a > 0 -> ");   print(a > 0);        print("\n");
    print("a < 0 -> ");   print(a < 0);        print("\n");
    print("a + 1 = ");    print((a + 1) as string); print("\n");
    return 0;
}
```

```
── VM ──                          ── JIT ──
a = 1790092890942                 a = 1790092890942
a > 0 -> 0                        a > 0 -> 1
a < 0 -> 1                        a < 0 -> 0
a + 1 = 1790092890943             a + 1 = 1790092890943
```

`date as longint` derlenirken ayrıca işlem üretmiyor (no-op):

```
$ ./build/saqut ir dcast.sqt
    0  LOAD_LONG
    1  CALLHOST        s1 = ffi::DATE_FROM_EPOCH_MS(s0)
    2  LOAD_SLOT       s2 = s1          <-- 'as longint' = düz kopya
    ...
   11  LOAD_LONG                        <-- 0 literalı
   12  GREATER         s8 = s2 > s7
```

`a as string` (`CAST_LONG_TO_STR`) ve `a + 1` (`LADD`) 64-bit değeri doğru
kullanıyor; yalnız karşılaştırma (`GREATER`/`LESS`) 32-bit yola düşüyor.
Sapma `date` tipine özgüdür:

- `fileSize(...)` (longint döner) → `> 0` **doğru**
- `modifiedTime(...)` (longint döner) → `> 0` **doğru**
- `fromEpochMillis(...)` (date döner) → `as longint` + `> 0` **VM'de yanlış**

`now() as longint` aynı hatayı verir (ilk gözlem:
`tests/dogfood-0.9.9/07-stdlib/n1.sqt`, silindi; kayıt `387006e` geçmişinde).

### Teknik yorum

`DATE_*` host fonksiyonlarının döndürdüğü `Value`, çalışma-zamanı etiketi
açısından `LongInt` değil gibi davranıyor; değer-tipi tabanlı karşılaştırma
opcode'u 32-bit dalı seçerken, slot'un statik tipi `longint` olduğu için
`LADD`/`CAST_LONG_TO_STR` 64-bit çalışıyor. JIT, indirgemeyi statik slot
tipinden yaptığı için doğru.

**Alternatif açıklama:** hata `date as longint` cast'ında değil, `GREATER`
opcode'unun VM'deki etiket dispatch'inde olabilir. Her iki durumda da düzeltme
VM tarafındadır; kök neden ayırımı için VM `Value`/karşılaştırma yolunun
incelenmesi gerekir. **Bu ayrım yapılmadı** (kapsam: tanı, kanıt).

### Neden önemli

ADR-038 determinizm sözleşmesi "MIR-kabul edilen program VM ile aynı gözlenebilir
çıktıyı vermeli" der. Burada **fark var**; üstelik VM — normatif backend —
yanlış olan taraf. `date`/`longint` dönüşümü zaman damgası işleyen her programda
doğal olarak geçer.

---

## B-02 — `string.length()` / `charAt()` çağrı başına O(n); döngüde O(n²)

**Sınıf:** performans (correctness değil)
**Durum:** Doğrulandı (ölçüm), karar bekliyor
**İlişkili:** #119 (string erişim seviyeleri/memory temsili), #236 (perf işleri)

### Gözlem

```c
// yalnız length(), karakter erişimi yok
string s = "a".repeat(200000);
int i = 0;
while (i < s.length()) { i = i + 1; }
```

```
length-only n=200000: 34.16s
```

```c
// charAt(i) döngüsü
string s = "a".repeat(50000);        -> 5.30s
```

`length()` çağrısı başına tüm string taranıyor (kod noktası sayımı), `charAt(i)`
de baştan i'ye kadar tarıyor. İkisi de O(n) → doğal döngü O(n²).

### Karşı ölçüm (çözüm kanıtı)

`byte[]` indeksleme O(1)'dir. Aynı 1.3 MB dosyada bayt taraması:

```c
byte[] raw = readFile(path);
while (i < raw.length()) { int b = raw[i] as int; ... i = i + 1; }
```

```
bytes=1321948 '<' sayisi=120003     0.09s
```

### Etki (gerçek uygulamada)

`apps/xml-tool` string API'siyle yazıldı:

- 35 KB (`derin.xml`, 5000 derinlik): ~8.9 s
- 1.3 MB (`genis.xml`): 20 sn'de **tamamlanmaz** (`timeout 20` → rc=124)

Yani dokümanlarda "karakter indeksi" olarak tanıtılan `length`/`charAt` ile
yazılan doğal karakter-bazlı ayrıştırıcı, MB ölçeğinde kullanılamaz. Doc, bu
metotların maliyetinden söz etmiyor (doc-gerçek çelişkisi değil, **belgelenmemiş
kısıt**).

---

## B-03 — Döküman: `e.code` "numeric" diyor, gerçekte string

**Sınıf:** doküman-gerçek sapması
**Durum:** Doğrulandı
**Kaynak doc:** `saqutwebside/src/content/docs/error-handling.md` ("`e.code`,
numeric error code")

### Gözlem

```c
try { int x = 10 / 0; } catch (Error e) { print(e.code); print("\n"); }
```

```
E_DIVZERO
```

`e.code` bir **string** (ör. `E_DIVZERO`, `E_OOB`). Ayrıca `Error` struct'ının
kaynak sırası `[line, col, message, trace, code]` (interpreter.cpp) ve
`e.col` alanı var ama dokümanda listelenmemiş.

---

## B-04 — Döküman: `Point(1, 2)` struct yapıcı sözdizimi gösteriyor, derleyici reddediyor

**Sınıf:** doküman-gerçek sapması
**Durum:** Doğrulandı
**Kaynak doc:** `saqutwebside/src/content/docs/type-casting.md` ("Struct
conversions" bölümü: `Point p = Point(1, 2);`)

### Gözlem

```c
struct Point { int x; int y; }
int main() { Point p = Point(1, 2); print(p.x); return 0; }
```

```
error [E003]: not callable: struct Point
```

Struct yapıcı çağrısı yok; alan alan atama gerekiyor. (Aynı doc bölümü
"struct'lar arası cast yok" der; o kısım doğru.)

---

## B-05 — Bayat örnek: struct alanına `.push()` "çalışmıyor" deniyor, artık çalışıyor

**Sınıf:** bayat doküman/örnek (yanlış rehberlik)
**Durum:** Doğrulandı
**Kaynak:** `examples/csv-crud-parser/csv_types.sqt` yorumu ("`rec.fields`
üzerinde `.push()` runtime'da 'expected array' ile düşer … alan dizisi her yerde
önce local bir değişkende doldurulup `rec.fields = dizi` ile geri atanır")

### Gözlem

```c
struct R { int[] xs; }
int main() { R r; r.xs = []; r.xs.push(5); print(r.xs.length()); return 0; }
```

```
direct push len=1
```

#226 ("struct-içinde-array alanına .push()") kapanmış ve düzeltme gelmiş; örnek
hâlâ eski kaçınma desenini öneriyor. Kullanıcıyı gereksiz karmaşıklığa itiyor.

---

## B-06 — Küçük tutarsızlıklar: `Error.toJson()` alan adı kaybı; `throw`'da boş `code`

**Sınıf:** tutarlılık (düşük öncelik)
**Durum:** Doğrulandı

```c
} catch (Error e) { print(e.toJson()); }
```

```
{"field0":3,"field1":20,"field2":"division by zero","field3":"main (...)\n","field4":"E_DIVZERO"}
```

Kullanıcı struct'ları `toJson()`'da alan adlarını koruyor
(`{"x":1,"y":2,"ad":"orijin"}`); yerleşik `Error` struct'ı `field0..field4`
üretiyor. Ayrıca `throw "ozel hata"` ile fırlatılan hatada `e.message` doluyken
`e.code` boş string. (Not: dogfood I-01'in bıraktığı yerde, `bool` gösterimi
kararı bu turda kapsam dışı.)

---

## B-07 — `longint` bağlamda aritmetik literal int sayılıyor

**Sınıf:** derleyici (literal aralık denetimi / sabit katlama körlüğü)
**Durum:** Doğrulandı
**İlişkili:** #114 (sayısal genişlik kör noktaları — constant folding)

### Gözlem

```c
longint arith = 4000000000 + 1000000000;
```

```
error [E003]: integer literal 4000000000 is out of int range (-2147483648 to 2147483647)
```

Ama `longint x = 4000000000;` (düz atama) kabul ediliyor. Yani hedef tip
`longint` olsa bile **aritmetik ifade içindeki** literal int bağlamında
denetleniyor ve reddediliyor. Kullanıcı `longint` hesabı yazamıyor. Aynı
sınıf SHA-256 yazılırken de çıktı:

```c
longint v = 1234567890123456;
longint lo = v & 4294967295;   // error [E003]: 4294967295 out of int range
```

`v & MASK` açıkça `longint` bağlamında olmasına rağmen maske literalı int
sayılıyor; sabiti ayrı bir `longint` değişkene almak gerekiyor.

---

## B-08 — Döküman: `>>` işaret davranışı belirtilmemiş (aritmetik çıktı)

**Sınıf:** doküman boşluğu (bit algoritmaları için correctness tuzağı)
**Durum:** Doğrulandı
**Kaynak doc:** `saqutwebside/src/content/docs/operators.md` (shift tablosu)

`operators.md` `<<`/`>>` operatörlerini listeler ama negatif işlenenlerde
davranışı söylemez. Ölçüm:

```c
int neg = 0 - 8;                 // 0xFFFFFFF8
print((neg >> 1) as string);     // -4      -> ARİTMETİK (işaret genişletir)
```

Bit-karıştırma algoritmaları (hash, checksum, sıkıştırma) mantıksal kaydırma
ister; `>>`'in aritmetik olduğunu bilmeyen kullanıcı sessizce yanlış sonuç
üretir. Doğru davranış tanımlanmalı ve belgeye yazılmalı.

---

## B-09 — `longint` doğrudan `byte`'a cast edilemiyor

**Sınıf:** doküman boşluğu / tutarlılık
**Durum:** Doğrulandı
**Kaynak doc:** `saqutwebside/.../type-casting.md` (yalnız `int`→`byte`)

```c
longint v = 1234567890123456;
byte b = (v >> 56) as byte;      // error: only 'int' can be cast to byte
```

Hata mesajı doğru yönlendiriyor (`as int as byte`), ama doc'taki cast tablosu
`longint` kaynağını içermiyor. `byte[]`'e yazmak isteyen kullanıcı bunu
deneyerek öğrenir.

---

## B-10 — Ölçüm (hata değil): SHA-256 VM'de ~0.35 MB/s, lineer

**Sınıf:** performans ölçümü (regresyon tabanı)
**Durum:** Doğrulandı

Saf saQut SHA-256, VM üzerinde:

```
1 MB rastgele -> 2.80 s
4 MB rastgele -> 10.39 s      (4x girdi, ~3.7x süre -> lineer)
6 MB ikili    -> 17.1  s      (sha256sum: 0.028 s)
```

Süper-lineer davranış gözlenmedi; `byte[]` indeksleme lineer, dizi `push`
amortize O(1). Fark sabit çarpandır (yorumlanan VM + bayt-başına erişim/push).
Hata değil; ileride perf işleri için taban çizgisidir.

---

## B-11 — String sıralaması yok: sıralı yapılar elle karşılaştırma gerektiriyor

**Sınıf:** dil kısıtı (belgelenmiş, ama etkisi kayda değer)
**Durum:** Doğrulandı
**Kaynak doc:** `saqutwebside/.../strings.md` ("String ordering operators
(`<`, `>`, `<=`, `>=`) are not available")

B-tree / sıralı harita / indeks gibi temel veri yapıları anahtar sıralamasına
dayandığından, kullanıcı kendi karşılaştırıcısını yazmak zorunda:

```c
int cmpStr(string a, string b) {
    byte[] ba = encode(a);
    byte[] bb = encode(b);
    ... // bayt sözlük sırası
}
```

Bu **belgelenmiş bir kısıt** (B-03/B-04 gibi çelişki değil), ancak sonucu
kayda değer: `apps/btree-kv` gibi bir uygulama bu yüzden her karşılaştırmada
`utf8::encode` çağırıp iki geçici `byte[]` üretir (O(len), tahsisli). Sıralı
konteynerler dile doğal olarak oturmuyor; `cmpStr` benzeri yardımcı ya stdlib'de
olmalı ya da sıralı string karşılaştırması tasarım kararı olarak verilmeli.

---

## B-12 — Ölçüm (hata değil): B-tree VM'de kararlı ve lineer

**Sınıf:** performans ölçümü (uzun ömürlü çalışma tabanı)
**Durum:** Doğrulandı

`apps/btree-kv` (arena B-tree, t=2):

```
bench 5000   -> 155 ms,  yukseklik 11,  ~13 MB RSS
bench 20000  -> 884 ms,  yukseklik 13,  ~34 MB RSS
REPL stresi  -> 5000 set + 2000 get, 0.23 s, ~13 MB RSS (kararlı)
```

5000→20000 (4x anahtar) için 5.7x süre; logaritmik karşılaştırma etkisi
beklenen aralıkta. REPL döngüsünde bellek büyümesi/sızıntı gözlenmedi.
`Entry`/`BNode` başına bellek ~1.7 KB; struct+string+GC yükü için makul.
Hata değil; uzun ömürlü çalışma için taban çizgisidir.

---

## Doğrulanmayanlar / kapsam dışı

- B-01 için kök nedenin "host dönüş etiketi" mi yoksa "GREATER dispatch" mı
  olduğu **ayrıştırılmadı**; VM `Value`/karşılaştırma kodu incelenmedi.
- B-02 için düzeltmenin (byte-tabanlı ayrıştırıcı) uygulanmış hâli yazılmadı;
  yalnız `byte[]` indekslemenin O(1) olduğu ölçüldü.
- `--jit` tüm testlerde kabul etmeyebilir (whole-program gate); B-01'de kabul
  etti ve VM'den farklı sonuç verdi.
- Dogfood turundan kalan I-01..I-05 hâlâ "karar bekliyor"; silinen ağaçta
  yalnız git geçmişinde (`387006e`), GitHub issue'su açılmadı.
- `btree-kv` gerçek B-tree silme (birleştirme/ödünç alma) içermiyor;
  tombstone kullanıyor — kapsam dışı, belgelendi.
- Tüm uygulamalar `--jit` altında koşulmadı; JIT karşılaştırması yalnız
  `--jit`'in programı kabul ettiği B-01 deneyinde yapıldı.
