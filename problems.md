# problems.md — saQut 0.9.9 kök-neden problem listesi

**Amaç:** gerçek uygulamalar + sistematik yüzey taramasıyla bulunan hataları ve
kök nedenlerini kaydetmek. Issue açılmaz; düzeltme başka bir ajanın işi.
Burada belirti + kanıt + kök-neden hipotezi + sınıf verilir.

- **Sürüm:** 0.9.9, dal `0.9.9`. **Normatif backend:** VM (`run`).
- **Kim çalıştırdı:** aşağıdaki tüm komutlar bu oturumda **çalıştırıldı**;
  birkaç FFI probu paralel ajanlarca yazıldı ama çıktıları burada
  **kendim yeniden koşturup doğruladım**. Bu dosya benim gözlemimdir.
- **Ham kanıt:** `apps/BULGULAR.md` (B-01..B-12), `apps/feature-sweep/`
  (a/b/c/core probları + results.txt).

Kapsam: tipler/cast'ler, operatörler, kontrol akışı, diziler, stringler,
struct/enum, modüller, nullable, **tüm FFI modülleri** (core, math, path, utf8,
os, terminal, fs, process, sys, date, stdin/stdout/stderr), CLI ve GC.

---

## 1. Gerçek hatalar (correctness)

### P-1 — `float`/`double` `%` HER ZAMAN hata veriyor (VM≢JIT)

**Belirti:** `10.0 % 3.0` çalışma-zamanı hatası veriyor.

```c
double a = 10.0; double b = 3.0;
print(a % b);          // runtime error: sıfıra bölme (mod)   rc=70
```
```
$ ./build/saqut run  mod_iso.sqt   → runtime error: sıfıra bölme (mod)   rc=70
$ ./build/saqut run --jit mod_iso.sqt
    func main: in instruction 'bne': unexpected operand mode for operand #2.
    Got 'double', expected 'int'
```

**Kapsam:** yalnız float/double. `int % int` (10%3=1), `decimal % decimal`
(10.0%3.0=1) ve `double % int` → biri hariç hepsi; `double % int` de hata.

**Kök neden (hipotez, kanıt güçlü):** VM'in float modül yolu, böleni
kontrol ederken **tamsayı bölen sıfır denetimini** (`d == 0`) kullanıyor;
çalıştırılan opcode float operanda tamsayı `bne` üretiyor. JIT tarafında ise
MIR indirgemesi double operand için tamsayı karşılaştırma (`bne`) yayıyor —
hata mesajı bunu birebir gösteriyor. İki backend de aynı yerde kırık, farklı
biçimde.

**Etki:** ondalık `%` gerektiren her program (aralık sarma, hash indeksi,
zaman dilimi) tamamen çalışmaz.

---

### P-2 — `date as longint` değeri VM'de int32 gibi karşılaştırılıyor (VM≢JIT)

**Belirti:** değer ve aritmetik doğru; **yalnız karşılaştırma** bozuk.

```c
date d = fromEpochMillis(1790092890942);
longint a = d as longint;
print(a as string);   // 1790092890942   (doğru)
print(a > 0);         // VM: 0   JIT: 1
longint b = toEpochMillis(d);
print(b > 0);         // 1  (doğru)
```

**Eşik ölçüldü (`c/date_cast.sqt`):**

```
CAST_SMALL=1000        GT0=1     <- < 2^31: DOĞRU
CAST_DAY=86400000      GT0=1     <- DOĞRU
CAST_2P32=4294967296   GT0=0     <- >= 2^31: YANLIŞ
CAST_NOW=1790101791663 GT0=0     <- YANLIŞ
```

**Kök neden (hipotez, yüksek güven):** `DATE_FROM_EPOCH_MS`/`now()` host
fonksiyonlarının döndürdüğü `Value`, çalışma-zamanı etiketi açısından 32-bit
tamsayı gibi; `GREATER`/`LESS` opcode'ları **değer etiketine** göre 32-bit dalı
seçiyor. Aynı slot `LADD`/`CAST_LONG_TO_STR`'de **statik slot tipi** (longint)
üzerinden 64-bit çalışıyor — bu yüzden print ve `+` doğru. `as longint` işlem
üretmiyor (IR'de düz `LOAD_SLOT`), yani cast yeniden etiketlemiyor. JIT statik
tipi kullandığı için doğru. (`fileSize`/`modifiedTime` longint döndüğü için
etkilenmiyor.)

**Etki:** ADR-038 determinizm sözleşmesi bu dilimde bozuk; üstelik VM —
normatif backend — yanlış taraf. Zaman damgası işleyen her program etkilenir.

---

### P-3 — `abs(INT_MIN)` sessiz taşma

```
abs(-2147483648) -> -2147483648     // hata yok, negatif döndü
```
**Kök neden:** 32-bit negasyon; `abs` iki-tümleyen taşmasını kontrol etmiyor.
**Etki:** sessiz yanlış değer (try/catch ile tutulamaz; hata üretilmiyor).

---

### P-4 — Diziler yazdırılamıyor/serileştirilemiyor

```c
int[] a = [1, 2, 3];
print(a);            // <ref>
print(a.toString()); // error [E003]: byte[]::toString requires a byte[] receiver
```

**Kök neden (kanıtlı çelişki):** `src/data/array.cpp` metot tablosu
`toString`'i `dpElemArray()` (her eleman tipi) için kayıtlı gösteriyor; ama
semantik katman yalnız `byte[]` alıcısını kabul ediyor
(`error: byte[]::toString requires a byte[] receiver`). Yani **kayıt
(registry) ile tip denetleyici çelişiyor**. `print(array)` ise `<ref>` basıyor.
Sonuç: düz bir diziyi ne print ne toString ile göremezsin; elle döngü gerekir.

**Etki:** teşhis ve loglama için temel bir yetenek yok.

---

### P-5 — `T?[]` (nullable elemanlı dizi) parse edilemiyor

```c
int?[] xs = [1, null, 3];   // error [E904]: expected variable name
string?[] names = ["a", null];  // aynı
int? sum(int?[] xs) {...}   // error [E905]: expected ')' after parameter list
```
Yalnız ters sıra çalışıyor: `int[]? xs` (nullable dizi, elemanlar non-null).

**Kök neden:** gramer diziden sonra `?` kabul ediyor, eleman tipinden sonra
kabul etmiyor. `#233` bu davranışla kapanmış görünüyor ama sonuç: **"null
olabilen eleman içeren dizi" tipi dilde ifade edilemiyor**. Delikli liste /
nullable-değerli harita gibi yaygın modeller yazılamıyor.

---

### P-6 — `import { X as Y }` takma adı çalışmıyor

```c
import { kare as sq } from "lib.sqt";
print(kare(6));   // error [E_SYMBOL_NOT_IMPORTED]: 'kare' ...
// 'sq' da tanımsız -> E001
```
**Kök neden:** parser import listesindeki `as` biçimini kabul ediyor ama
bağlama yapmıyor; ne orijinal adı ne takma adı bağlanıyor. Ayrıca
`src/internal/ffi.sqt` başlık yorumu bu biçimi **öneriyor** ("Çarpışma isteyen
`import {readFile as fileRead} from fs` ile kendi takma adını verir") — yani
yorumla gerçek çelişiyor.

---

### P-7 — `finally` ayrılmış ama uygulanmamış

```c
try { ... } catch (Error e) { ... } finally { ... }
// error [E901]: unexpected token 'finally' — expected a statement
// + E013 cascade
```
**Kök neden:** `finally` tokenizer'da anahtar kelime, parser/AST'de karşılığı
yok (dogfood I-05 ile aynı). Kullanıcıya görünen sonuç: `finally` değişken adı
olarak da kullanılamıyor ama iş de yapmıyor.

---

### P-8 — `switch` case'lerinde `break` yoksa fallthrough YOK (davranış doğru, kayda değer)

`case 1: sw+=1; case 2: sw+=2; break;` → `sw=1`. Belgelenen davranışla
("Switch lowering is designed without fallthrough") uyumlu; sorun değil, ama
C alışkanlığıyla yazan biri için sessiz tuzak.

---

## 2. Doküman-gerçek sapmaları (docs "böyle olur" diyor, olmuyor)

### P-9 — `int ↔ bool` cast'i dokümanda var, derleyici reddediyor

`saqutwebside/src/content/docs/type-casting.md` tablosu: `int → bool` (0→false)
ve `bool → int`
(0/1) "destekli" diyor. Gerçek:

```c
bool b = (5 as bool);   // error [E003]: bool as target type not allowed
int  i = (true as int); // error [E003]: bool can only be cast to string
```
**Kök neden:** type-casting dokümanı ile `TypeChecker` cast matrisi
ayrışmış; checker `bool` hedefini ve `bool` kaynağını (string hariç) reddediyor.

### P-10 — "Any type can become `string`" yanlış

Aynı doc "Any type can be cast to string" diyor. Gerçek: struct, enum ve dizi
`as string` reddediliyor (`error [E003]: 'struct P' cannot be cast...` /
`'K' cannot be cast` / `'int[]' cannot be cast`). Yalnız skaler tipler + bool.

### P-11 — Struct yapıcı `Point(1, 2)` dokümanda var, yok

`type-casting.md` "Struct conversions" örneği `Point p = Point(1, 2);`
gösteriyor → `error [E003]: not callable: struct Point`. Alan alan atama
gerekiyor.

### P-12 — `e.code` "numeric" deniyor, string

`error-handling.md`: "`e.code`, numeric error code". Gerçek: string
(`E_DIVZERO`, `E_OOB`, `E_HOST`). Ayrıca `e.col` alanı var ama belgelenmemiş.

### P-13 — `longint` interop cast'leri dokümanda yok

`longint as byte` → `error: only 'int' can be cast to byte`; `byte as longint`
→ `error: 'byte' can only be cast to int or string`. `longint as int` ise
**çalışıyor** ama aralık dışıysa çalışma-zamanı hatası (rc=70):
`5000000000 as int` → `runtime error: longint value ... out of int range`.
Doc cast tablosu bu yolları içermiyor.

### P-14 — Bayat örnek: struct alanına `.push()`

`examples/csv-crud-parser/csv_types.sqt` yorumu "`rec.fields.push()` runtime'da
düşer" diyor ve kaçınma deseni öneriyor. Gerçek: #226 kapanmış, doğrudan
`r.xs.push(5)` **çalışıyor** (`r.xs.length()==1`). Örnek kullanıcıyı gereksiz
karmaşıklığa itiyor.

### P-15 — `dump` iç içe alanlarda `<ref>`

`Kisi.dump()` → `struct{ad=Ayşe, yas=30, adres=<ref>, etiketler=<ref>}`.
İç içe struct/dizi içeriği görünmüyor (toJson doğru serileştiriyor).

---

## 3. Dil/tasarım kısıtları (hata değil; kök nedeniyle kayda değer)

- **P-16 — String sıralaması yok.** `<`,`>` string'de derleme hatası
  (`err_string_order.sqt`). Sıralı yapı (B-tree/harita) için elle
  UTF-8 karşılaştırma yazmak gerekiyor; `apps/btree-kv` bunu her
  karşılaştırmada `utf8::encode` ile yapıyor (tahsisli, O(len)).
- **P-17 — `char` pratikte yok.** `char c;` tip olarak var ama `'A'` literalı
  parse edilmiyor; `c = 65` "cannot assign int to char" diyor. KB "partial"
  diyor, gerçek: kullanılamaz.
- **P-18 — `auto` yok.** `auto x = 5;` → `error [E901]: unexpected token
  'auto'`. KB "Partially Implemented" diyor; gerçekte parser reddediyor.
- **P-19 — `date` ile `int` karşılaştırma tutarsız.** `d > 0` derleme hatası
  ("comparison operator only works with numeric types: date") ama `d == 0`
  **derleniyor** ve kipleniyor (epoch 0 için true). Eşitlik ve sıralama farklı
  kurallara tabi.
- **P-20 — Kaydırma sayısı 32'ye maskeleniyor.** `1<<32 == 1`, `1<<33 == 2`
  (x86 davranışı). `>>` **aritmetik** (`-8 >> 1 == -4`). İkisi de dokümanda
  belirtilmemiş; bit algoritmaları için sessiz tuzak.
- **P-21 — Kayan nokta literalı varsayılanı `float`.** `1.0/3.0` → `0.333333343`
  (float32); `double d = 1.0/3.0;` **W004 "float → double implicit widening"**
  uyarısı veriyor. Yani `1.0` double değil float.
- **P-22 — `print` biçim tuhaflıkları.** `sqrt(-1.0)` → `-nan.0`; `PI()` →
  `3.141592654` (10 basamak), `E()` → `2.718281828`. `print` bool'u `1/0`
  basıyor (dogfood I-01; karar bekliyor).
- **P-23 — `throw` ile fırlatılan `Error.code` boş.** `throw "ozel"` →
  `e.message="ozel"`, `e.code=""`. `throw 42` → message `"42"`.

---

## 4. Performans ölçümleri (hata değil; regresyon tabanı)

- **P-24 — `string.length()`/`charAt()` çağrı başına O(n).** 200.000 karakterde
  yalnız `length()` döngüsü **34 s**; 50.000'de `charAt` **5,3 s**. Karakter
  bazlı ayrıştırma O(n²). Karşı ölçüm: aynı 1,3 MB dosyada `byte[]` indeksli
  tarama **0,09 s**. (`apps/xml-tool` 1,3 MB'de 20 s'de bitmiyor.)
- **P-25 — SHA-256 (saf saQut) ~0,35 MB/s, lineer.** 1 MB→2,80 s, 4 MB→10,39 s,
  6 MB→17,1 s. `sha256sum` aynı 6 MB'ı 0,028 s'de işliyor. Doğru sonuç.
- **P-26 — B-tree/REPL.** 5000 kayıt→155 ms (yükseklik 11), 20000→884 ms;
  5000 komutluk REPL 0,23 s / ~13 MB RSS, kararlı (sızıntı gözlenmedi).

---

## 5. CLI / tooling

- **P-27 — `run` argümansız → `source.sqt` açmaya çalışıyor.**
  `saqut run` → `error [E_MODULE_NOT_FOUND]: cannot open module 'source.sqt'`
  (rc=65). Kullanım hatası yerine gizli varsayılan dosya.
- **P-28 — Kaldırılmış komutlar "module" gibi yorumlanıyor.**
  `saqut compile x.sqt` → `cannot open module 'compile'` (rc=65). `compile/parse/
  transpile` artık komut değil; hata mesajı "module" diyor, kafa karıştırıcı.
- **P-29 — stdin modu `-` yok.** `saqut run -` → `error: no input file` (rc=64).
- **P-30 — GC ve araçlar çalışıyor.** `--gc-stats` (`collections/freed/live`),
  `--gc-threshold=4096` (333 toplama), `--profile`, `--optimized`,
  `--dont-optimize`, `run/check/ast/symbols/ir/tokens/exec/bench` hepsi rc=0 ve
  beklenen çıktıyı veriyor.

---

## 6. Doküman altyapısı (knowledge-base/) sapmaları

`knowledge-base/` (ajan hafızası) birkaç yerde güncel kaynakla çelişiyor:

- `src/builtin/` diyor; gerçek dizin `src/data/` (metot tabloları orada).
- `compile/parse/transpile/interpret` "stub, 1 döner" diyor; gerçekte komut
  olarak yok (module-not-found davranışı).
- `**` "left grouping yapar" diyor (02_Language); gerçek ve public doküman:
  **right-associative** (`2**3**2=512`).
- `auto` "partially implemented" diyor; gerçekte parse hatası.

---

## 7. Doğrulanan ÇALIŞAN yüzey (özet)

Aşağıdakiler bu oturumda koşulup **beklendiği gibi** çalıştı (ayrıntı:
`apps/feature-sweep/*/results.txt` ve core probları):

- **FFI fs (17/17):** writeFile/appendFile/readFile (tek-argüman ve
  `(seek,size)` **kısmi okuma dahil**), existsFile, fileSize, isEmpty,
  isDirectory, isFile, createFile, copyFile, renameFile, createDirectory,
  removeDirectory (recursive), removeFile, list (sıralı), walk (recursive).
  UTF-8 round-trip 18 bayt birebir.
- **FFI process (4/4):** cwd, pid, chdir, exit(7)→rc=7.
- **FFI sys (5/5):** random∈[0,1), randomInt (yarı-açık `[lo,hi)`, **doğru**),
  env (dolu/null), sleep(200)=0,20 s, args.
- **FFI math/path/utf8/os/terminal/core:** hepsi çalışıyor (P-3, P-21, P-22
  biçim notları hariç).
- **FFI date:** fromEpochMillis(0)→1970-01-01, add*/diffMillis doğru, parse/format
  (yyyy-MM-dd, HH:mm:ss, dd/MM/yyyy, `.SSS`), parse hatası→null. (Z'siz ISO
  parse→null.)
- **G/Ç:** readLine (satır + EOF null), readAll, readBytes (UTF-8 bayt),
  stdout/stderr **ayrı akışlar** (write/writeBytes).
- **Dil:** tüm dizi metotları (push/pop/insert/remove/slice[from,to)/
  reverse/concat/contains/indexOf/clear; OOB→E_OOB), string metotları
  (unicode kod-noktası, toBuffer→byte[]→toString), struct (nested, dizi alanı,
  toJson), enum (açık değer, switch, karşılaştırma), modüller (döngü tespiti,
  export denetimi, çok modül), kontrol akışı (if/while/for/do-while/switch/
  break/continue/try-catch-throw), operatörler (aritmetik/bit/karşılaştırma/
  mantık/atama/++--; `**` sağ-birleşmeli), 2B dizi, blok kapsamı/gölgeleme,
  global değişkenler, struct dönüşü.
- **VM≡JIT:** 10 çekirdek probda (arrays/strings/structs/enums/control/ops...)
  birebir; farklar P-1 ve P-2'de.

---

## 8. Doğrulanmayanlar / kapsam dışı

- P-1, P-2 için VM `Value`/opcode kaynağı okunmadı; kök neden **hipotezdir**
  (IR dökümü ve eşik ölçümüyle desteklendi, kod okumasıyla değil).
- `--jit` tüm prob yüzeyinde koşulmadı; yalnız çekirdek 10 prob + P-1/P-2.
- `apps/feature-sweep/c/` için ajan raporu tamamlanmadı (bütçe); probları
  kendim çalıştırdım, `c/results.txt` yok.
- `sys::sleep`/`now`/`random`/`modifiedTime` doğal olarak değişken; değerler
  kayıtlı koşumdandır.
- LSP/DAP protokol davranışı bu taramada kapsanmadı.
