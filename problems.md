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

## 0. Öncelikli GERÇEK problemler (ürün kararı bekleyen)

- **P-6 — Import sistemi yollar arası tutarsız.** Gereklilik: import semantiği
  (bağlama, takma ad, çakışma, görünürlük) FFI ve kaynak-dosya importunda
  **eşit** olmalı; şu anda iki ayrı kod yolu farklı davranıyor.
- **P-33 — `>>>` sessizce yanlış sonuç veriyor.** Mantıksal kaydırma operatörü
  yok; `>>` + `>` diye ayrışıp 0 üretiyor. Sessiz yanlış değer kabul edilemez;
  ya operatör olarak uygulanmalı ya da sözdizimi hatası verilmeli.
- **P-31/P-32 — Sessiz yutma.** `check`/`run` parse hatasını görmezden geliyor;
  eksik operand (`5 +`) yanlış değer (10) üretiyor.
- **P-1 — `float`/`double` `%` her zaman hata** (VM sıfıra-bölme, JIT MIR hatası).
- **P-2 — `date as longint` karşılaştırması** VM'de int32'ye kırpılıyor (VM≠JIT).

---

## 0b. Sade dille: her problem ne demek, hangi issue'da

Her madde için GitHub issue açıldı (giriş/gelişme/sonuç, derleyici bilmeyen
bir yazılımcının anlayacağı dille). Eşleme:

- **P-1** ondalık `%` hiç çalışmıyor → **#241**
- **P-2** tarih→longint karşılaştırması VM'de yanlış → **#242**
- **P-3** `abs(INT_MIN)` sessizce negatif → **#243**
- **P-4** dizi yazdırılamıyor/serileştirilemiyor → **#244**
- **P-5** "null olabilen elemanlı dizi" yazılamıyor → **#245**
- **P-6** import sistemi FFI ile kaynak arasında tutarsız → **#246**
- **P-31/P-32** eksik ifade sessizce yutulup yanlış değer veriyor → **#247**
- **P-33** `>>>` sessizce 0 üretiyor → **#248**
- noktalı virgül fiilen isteğe bağlı → **#249**
- başlatılmamış yerel değişken bayat değer taşıyor → **#250**
- bileşik atama `d *= k` çöp değer → **#251**
- import dosya eşleşmesi suffix ile (yanlış dosya) → **#252**
- global nullable daraltma güvensiz → **#253**
- özyineleme sınırı yok (VM askı, JIT segfault) → **#254**
- null struct erişimi yakalanamıyor → **#255**
- bilinmeyen kaçış dizisi yutuluyor → **#256**
- CLI bilinmeyen bayrak/argümanı yutuyor → **#257**
- hata mesajları TR/EN karışık → **#258**
- tanı `'assignment'` diyor (değişken adı yok) → **#259**
- `Error.toJson()` alan adlarını kaybediyor → **#260**
- ondalık literal varsayılanı float (belge double) → **#261**
- büyük int literal longint bağlamını almıyor → **#262**
- JIT: `string?` dönüşü bozuk → **#263**
- JIT: nullable FIELD_SET/STORE_GLOBAL/STRING_CONCAT yok → **#264**
- JIT: float alan/global host çağrısı MIR hatası → **#265**
- `saqut ir` F32 opcode'larını göstermiyor → **#266**
- `date>int` derlenmez ama `date==int` derlenir → **#267**
- `throw` edilen Error.code boş → **#268**
- docs: `int↔bool` cast sapması → **#269**
- docs: "her tip → string" yanlış → **#270**
- docs: struct yapıcı `Point(1,2)` yok → **#271**
- docs: `e.code` numeric deniyor, string → **#272**
- docs: `longint/byte` cast yolları eksik → **#273**
- example: csv `.push()` notu bayat → **#274**
- docs: kaydırma semantiği belgesiz → **#275**
- docs: `tokens/ast/symbols` çıktı biçimi → **#276**
- docs: çıkış kodu matrisi → **#277**
- `print(bool)`/float biçimi → **#278**
- varsayılan parametre/overload kararı → **#279**
- string sıralaması kararı → **#280**
- `auto` kararı → **#281**
- examples + knowledge-base güncelliği / CI → **#282**
- `normalize` sondaki `/` → **#283**

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

### P-6 — Import sistemi yollar arası TUTARSIZ (FFI ≠ kaynak dosya) — GERÇEK PROBLEM

**Gereklilik (ürün kararı):** import semantiği her yerde **aynı** olmalı —
bağlama, takma ad (`as`), çakışma davranışı ve görünürlük gömülü FFI modülleri
ile kaynak-dosya importları arasında ayrışmamalı. Şu anda iki ayrı kod yolu
var ve davranışları farklı.

**Gözlenen farklar:**

- **Takma ad, farklı ad:**
  - FFI: `import { existsFile as ef } from fs;` → `ef` çalışır ✓
  - kaynak: `import { kare as sq } from "lib.sqt";` → `sq` **tanımsız** ✗,
    üstelik `kare` de erişilemez.
- **Takma ad, aynı ad:** ikisi de çalışır (`import { kare as kare } ...` ✓).
- **Takma ad çakışması (ad zaten var):**
  - FFI: `import { abs as print } from math;` → **sessizce yok sayılır**, tanı yok.
  - kaynak: benzer çakışma → `E002 'kare' already defined in this scope`.
- **Sembol sızıntısı:** kaynak modül sembolleri **global scope'a** tanımlanıyor;
  bu yüzden `import { kare } from "lib.sqt"` + yerel `int kare(...)` → E002
  (hata kaynağı lib.sqt satırına işaret ediyor). FFI'de böyle bir sızıntı yok.
- **Yinelenen import:** kaynak tarafta `import { kare } ...` iki kez → sessizce
  dedupe; takma adlı ikinci import ise yine bozuk (`alias_dup`).

**Kök neden (kaynak okundu — hipotez değil):**
- FFI yolu `SymbolCollector::resolveFfiImport` yerel adı
  `table_.define(local, ...)` ile **bağlıyor** → görünürlük doğal.
- Kaynak yolu (`src/symbol/symbol_collector.cpp`, `validateImports`) yalnız
  `moduleImports_[moduleId].insert(localName)` yapıyor (satır ~406); sembolü
  yerel adla **yeniden tanımlamıyor**. Sembol, kaynak modül toplanırken
  orijinal adıyla — ve global scope'ta — kalıyor. Görünürlük kapısı
  (`symbol_collector.cpp:697`) yerel adı arıyor; sembol orada olmadığı için
  takma ad çözülemiyor, orijinal ad da erişim listesinde olmadığı için düşüyor.

**Düzeltme yönü:** kaynak-import yolu da FFI gibi **yerel (takma) adı bu
modülde bağlamalı**; kaynak sembollerinin global scope'a sızması bırakılmalı;
iki tür import tek bir bağlama rutinini paylaşmalı.

**Kanıt:** `apps/feature-sweep/core/mod/` → `ffi_alias.sqt` (FFI ✓),
`alias4.sqt` (kaynak ✗), `alias_same.sqt` (aynı ad ✓), `alias_dup.sqt`,
`ffi_dup.sqt` (FFI çakışma sessiz), `local_shadow.sqt` (E002 sızıntı),
`dup_import.sqt`.

---

### P-7 — `finally` ayrılmış ama uygulanmamış (v3'te planlı)

```c
try { ... } catch (Error e) { ... } finally { ... }
// error [E901]: unexpected token 'finally' — expected a statement
// + E013 cascade
```
**Kök neden:** `finally` tokenizer'da anahtar kelime, parser/AST'de karşılığı
yok. **Ürün bilgisi:** `finally` v3'te planlanıyor — yani eksik, hata değil;
bugünkü durum belgeye "ayrılmış, henüz yok" diye yazılmalı ki kullanıcı
denemesin.

---

### P-8 — `switch` case'lerinde `break` yoksa fallthrough YOK (davranış doğru, kayda değer)

`case 1: sw+=1; case 2: sw+=2; break;` → `sw=1`. Belgelenen davranışla
("Switch lowering is designed without fallthrough") uyumlu; sorun değil, ama
C alışkanlığıyla yazan biri için sessiz tuzak.

---

## 2. Doküman-gerçek sapmaları (docs "böyle olur" diyor, olmuyor)

### P-9 — `int ↔ bool` cast'i dokümanda var, derleyici reddediyor (şu anda ekleniyor)

`saqutwebside/src/content/docs/type-casting.md` tablosu: `int → bool` (0→false)
ve `bool → int`
(0/1) "destekli" diyor. Gerçek:

```c
bool b = (5 as bool);   // error [E003]: bool as target type not allowed
int  i = (true as int); // error [E003]: bool can only be cast to string
```
**Kök neden:** type-casting dokümanı ile `TypeChecker` cast matrisi
ayrışmış; checker `bool` hedefini ve `bool` kaynağını (string hariç) reddediyor.
**Ürün bilgisi:** `int → bool` dönüşümü **şu anda ekleniyor** — yani bu madde
geçiş dönemi sapmasıdır; ekleme landığında belgeyle uyumlu olacak.

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
- **P-17 — `char` pratikte yok (tasarım: string).** `char c;` tip olarak var
  ama `'A'` literalı parse edilmiyor. **Ürün bilgisi:** karakterler için
  `string` düşünülmüş; `char` bu nedenle minimal/bilinçli. Belgeye "karakter =
  1 uzunluklu string" diye yazılmalı ki `char` aranmasın.
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
  `3.141592654` (10 basamak), `E()` → `2.718281828`. **Ürün bilgisi:** `print`
  başlangıçta yalnız string gösteriyordu; int/longint/float gösterecek şekilde
  **yeni genişletildi**. `print(bool)` → `1/0` (dogfood I-01; karar bekliyor).
  Yani buradaki notlar gelişmekte olan bir yüzeyin gözlemidir.
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

## 9. İkinci tur — ek tuhaflıklar (sessiz yutma ve belge sapmaları)

### P-31 — Derleyici, parse hatalarını SESSİZCE yutuyor (`check` ve `run`)

**Belirti:** bazı sözdizimi hataları ne hata veriyor ne reddediliyor; program
çalışıyor ve rc=0. `check` de `errors:0` diyor.

```
$ ./build/saqut run bad.sqt      # int main(){ int x = ; return 0; }
$ echo $?                        # 0   <- hata yok
$ ./build/saqut check bad.sqt
{"record":"check.end","errors":0,"warnings":0}
rc=0
```

Sessizce kabul edilenler (rc=0): `int x = ;`, `int x = 1 print(x);` (eksik
`;`), `for (int i=0 i<3; i++)` (for başlığında eksik `;`), `return 0 }`.
Doğru reddedilenler (rc=65): kapanmayan parantez/blok, geçersiz token `@`,
tanımsız tip `Foo`. Yani bazı üretimler tanı üretiyor, bazıları hiç
üretmiyor — **kayıp hata sınıfı**.

**Kök neden (hipotez):** parser panic-mode toparlanmasında `ErrorNode`
üretiyor ama bu üretimler için `DiagnosticEngine` beslenmiyor; hata kanalına
hiç düşmüyor. AGENTS §8 ve #219 ("sessiz kaçış taraması") tam olarak bu sınıfı
hedefliyor. **Tooling için ağır sonuç:** `check` temiz der, `LSP` tanı
yayınlamaz, ama aynı kaynak sessizce yanlış çalışır.

---

### P-32 — Eksik operand sessizce YANLIŞ DEĞER üretiyor

```
print(5 +)      -> 10     # 5 + 5  (eksik sağ operand sol tarafı tekrarlıyor)
print(5 * 2 +)  -> 20     # 10 + 10
print(1 +)      -> 2
```
Hata yok, rc=0. **Kök neden (hipotez):** eksik ifade toparlanınca RHS, LHS'e
yeniden bağlanıyor (sol taraf iki kez kullanılıyor). Sessiz yanlış sonuç — en
tehlikeli sınıf (P-31'in değer üreten yüzü).

---

### P-33 — `>>>` sessizce YANLIŞ sonuç veriyor — GERÇEK PROBLEM

```c
int a = 8;
print(a >>> 1);   // 0     (mantıksal kaydırma beklenir)
print(a >> 1);    // 4     (doğru)
print(8 >>> 2);   // 0
print(16 >>> 2);  // 0
```
`saqut ast --json` gösteriyor: `8 >>> 1`, `>>` ve `>` token'larına ayrılıp
`BinaryExpression(">")` içinde `BinaryExpression(">>")` olarak ayrışıyor.
Yani `>>>` diye bir operatör yok; iki operatör olarak sessizce yanlış ayrışıyor
ve 0 üretiyor.

**Neden gerçek problem:** sessiz yanlış sonuç — sözdizimi hatası da verilmiyor,
çalışma-zamanı hatası da; kullanıcı 0'ı geçerli sonuç sanıyor. Üstelik
`>>` **aritmetiktir** (P-20), yani dilde **mantıksal sağ kaydırma operatörü
yok**; bit-karıştırma algoritması yazan kullanıcı maskelemek zorunda ve
`>>>`'ün var olduğunu sanıp yanlış sonuç alabiliyor.

**Gereklilik:** `>>>` ya gerçek bir mantıksal kaydırma operatörü olarak
uygulanmalı ve belgelenmeli, ya da `>>` + `>` diye sessizce ayrışmak yerine
**açık sözdizimi hatası** vermeli. Sessiz yanlış değer kabul edilemez.

**Kanıt:** `apps/feature-sweep/core/parse-silence.sh` (>>> satırı), `edges.sqt`.

---

### P-34 — CLI çıktı biçimi belge sapması (tokens/ast/symbols)

`cli-reference.md` üçü için de "Print ... as JSON" diyor. Gerçek:

```
$ saqut tokens  f.sqt       -> "Tokenler (114 adet):" ...   (düz metin)
$ saqut tokens --json f.sqt -> aynı düz metin (--json YOK SAYILIYOR, hata yok)
$ saqut ast     f.sqt       -> "Program" ...                (düz metin)
$ saqut ast --json f.sqt    -> { ... }                       (JSON)
$ saqut symbols f.sqt       -> düz metin
$ saqut symbols --json f.sqt-> error: --json is removed for symbols; use --jsonl
$ saqut symbols --jsonl     -> {"record":"symbols.header",...}   (JSONL)
```
Yani `tokens` hiç JSON vermiyor (`--json` sessizce yutuluyor), `ast` yalnız
`--json` ile JSON, `symbols` makine çıktısı için `--jsonl` istiyor. Belge üçü
için de yanlış.

---

### P-35 — Çıkış kodu belge sapması ve gözlenen matris

`cli-reference.md`: "Most usage, compilation, and runtime failures return 1."
Gözlenen (sysexits tarzı):

```
kullanım hatası      -> 64
derleme/parse hatası -> 65
okunamayan modül     -> 65 (E_MODULE_NOT_FOUND)
runtime hatası       -> 70 (division by zero, OOB, cast, double% ...)
main dönüşü          -> programın döndürdüğü değer (return 3 -> rc=3)
başarı                -> 0
```
Belge "1" diyor; gerçek matris yukarıdaki gibi. (Ayrıca `run -` stdin modu →
rc=64 "no input file".)

---

### P-36 — Varsayılan parametre ve fonksiyon aşırı yüklemesi YOK

```c
int f(int a, int b = 2) { return a + b; }   // E905 + E901
int f(int a) {...} int f(int a,int b) {...} // E002: 'f' already defined
```
FFI fonksiyonlarında isteğe bağlı argüman var (`readFile(path, seek?, size?)`)
ama kullanıcı fonksiyonlarında yok. Dil kısıtı; belgeye göre teyit edilecek.

---

### P-37 — Yerleşik `Error.toJson()` alan adlarını kaybediyor

```c
catch (Error e) { print(e.toJson()); }
// {"field0":3,"field1":20,"field2":"division by zero","field3":"...\n","field4":"E_DIVZERO"}
```
Kullanıcı struct'ları `toJson()`'da adları koruyor (`{"x":1,"y":2}`); yerleşik
`Error` `field0..4` üretiyor. (B-06'nın problems.md'ye eksik kalan kısmı.)

---

### P-38 — `normalize` sondaki `/`'i koruyor

```
normalize("/tmp/a/") -> "/tmp/a/"
```
std::filesystem davranışıyla uyumlu olabilir; belirsiz, kayda değer.

---

### P-39 — Başlatılmamış değişkenler sessizce sıfır-benzeri

```
int x;      -> 0
string s;   -> ""
bool b;     -> 0
P p; p.x;   -> 0
try { throw 42; } catch (Error e) { e.code == "" }
```
Bu **doğru olabilir** (tasarım: güvenli varsayılanlar, #184 ile uyumlu);
kayda değer çünkü dilde `uninitialized` uyarısı yok ve hata da yok.

---

### Eksik gördüğüm özellikler ("bu neden yok?" listesi)

**Kabul edilen kısıtlar (ürün kararı — hata değil):**
- `map`/`set`/`tuple`/generic/closure → **v1'den sonra** planlı.
- `finally` → **v3'te** planlı (P-7).
- `char` → string tabanlı karakter modeli (P-17).
- Dizi `toJson` yok, dizi `toString`/`print` çalışmıyor (P-4) — bilinen boşluk.

**Gerçek boşluklar (kayda değer):**
- **Mantıksal sağ kaydırma operatörü** (`>>>`) yok; `>>` aritmetik ve `>>>`
  sessizce yanlış sonuç veriyor (P-33/P-20).
- **Nullable elemanlı dizi** `T?[]` ifade edilemiyor (P-5).
- **`import { X as Y }` import yolları tutarsız** (FFI'de çalışıyor,
  kaynak-dosyada bozuk); semantik her yerde eşit olmalı (P-6).
- **String sıralaması** yok → sıralı konteynerler için elle karşılaştırma (P-16).
- **Varsayılan parametre / aşırı yükleme** yok (P-36).
- **`auto`** yok (P-18).
- **Formatter (`saqut fmt`) ve paket yöneticisi** yok (KB "planned").

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

---

## 10. Kök-neden denetimi ve iş listesi (kod okundu, 2026-09-22)

Bu bölüm yukarıdaki P-maddelerini **kaynak kodu okuyarak** doğrular ve
listede olmayan yeni bulguları (⚑) ekler. Her madde tek bir issue'ya
karşılık gelecek biçimde yazıldı: `problem — kaynak` / `düzeltme özeti`.
Mevcut `tests/run.sh` takımının tamamı geçiyor; aşağıdakilerin hiçbiri bugün
bir testle yakalanmıyor.

Not: P-33 (`>>>`) ayrı bir hata değil, P-32'nin yansıması: `a >>> 1` →
`a >> (eksik) > 1`; eksik operand LHS ile doldurulduğu için `8 >> 8 = 0`.

### 10.1 Hemen (v1 öncesi): sessiz yanlış değer, çökme, güvenlik açığı

- [ ] ⚑ Başlatılmamış skaler yerel değişken için kod üretilmiyor (`int x;`, `double d;`) — `src/ir/ir_generator.cpp` VariableDecl son `else` dalı yalnız string/nullable/struct/dizi işliyor; VM'de slot önceki döngü turunun değerini taşıyor (3 yerine 6), JIT'te çöp değer.
  Her bildirimde tipe göre sıfır yükle (int/long/float/double/decimal/date/enum); struct alanları (`initNestedStructFields`) ve globaller de tipli sıfırla başlasın (bugün `double` alan VM'de `0`, JIT'te `0.0`).
- [ ] ⚑ Bileşik atama int RHS'yi genişletmiyor (`d *= k` → `1.48e-323`, `f += k` → `1.5`) — `ir_generator.cpp` bileşik atama yolu kendi opcode switch'ini kullanıyor, `generateBinaryArithmetic` dönüşümünü atlıyor.
  Bileşik atama ikili aritmetikle aynı tip-yükseltme rutinini paylaşsın.
- [ ] P-1: float/double `%` her zaman hata — `FMOD`/`F32MOD` opcode'u yok; `MOD` int yoluna düşüyor (VM float bitlerini int okuyup "sıfıra bölme", JIT `bne` hatası).
  `FMOD`/`F32MOD` ekle (`fmod`/`fmodf`), VM+JIT, `%=` dahil.
- [ ] P-31/P-32/P-33: Eksik ifade raporlanmıyor (`5 +` → 10, `int x = ;`, `>>>`) — `src/parser/parser.cpp` `parseLeftDenotation` `right == nullptr` için tanı üretmiyor; VarDecl ilk değeri ve `return` da aynı.
  `expectExpression()` yardımcısı: null → E901 + panic-mode kurtarma; `>>>` kendiliğinden sözdizimi hatası olur.
- [ ] ⚑ Noktalı virgül her yerde fiilen isteğe bağlı (`int x = 1 print(x);`, `return 0 }`, for başlığı) — `parser.cpp` genelinde `if (SEMICOLON) nextToken();` kalıbı.
  `expect(SEMICOLON)` yardımcısı E905 üretsin; tüm deyim sonları ve for başlığı.
- [ ] P-2: `date as longint` VM'de int32'ye kırpılıyor (VM≠JIT) — `src/vm/value.hpp` `asI64()` Date'i `(int)` okuyor; `as longint` IR'de yeniden etiketlemiyor.
  Cast yeniden etiketlesin; `asI64`/`asDouble` Date'i 64-bit okusun.
- [ ] P-3: `abs(INT_MIN)` — `src/ffi/functions/math.cpp` `std::abs(int)`, bu değerde C++ UB.
  Tanımlı davranış (yakalanabilir taşma hatası ya da sarma — ürün kararı).
- [ ] P-6 + ⚑: Modüller tek global sembol tablosunu paylaşıyor — iki modülde export edilmemiş aynı ad E002; FFI importu diğer modüllere sızıyor (`sqrt` import edilmeden kullanılabiliyor); kaynak dosyada `import {x as y}` çalışmıyor — `src/symbol/symbol_collector.cpp` `validateImports`/`resolveFfiImport`; IR/VM `findFunction(name)` düz ad.
  Modül başına kapsam; FFI ve kaynak import yerel adı tek rutinle bağlasın; IR'de modül-nitelikli fonksiyon/struct/global adları.
- [ ] ⚑ Import edilen dosya yol **soneki** ile eşleniyor (`"lib.sqt"` → `mylib.sqt`) — `validateImports` suffix karşılaştırması.
  Loader'ın canonical yolunu kullan, tam eşleşme.
- [ ] ⚑ Global nullable daraltma sağlam değil: `if (g != null) { clear(); g + 1 }` → sessizce `1`; `gs.length()` hiçbir şey basmadan rc=0 — type checker globalleri daraltıyor, çağrıda geçersiz kılmıyor.
  Globalleri alanlar gibi daraltma (yerel kopya iste); VM aritmetiğinde null operand E_NULL.
- [ ] ⚑ Sonsuz özyineleme: VM sınırsız büyüyüp askıda kalıyor, JIT segfault — çağrı derinliği sınırı yok.
  Yapılandırılabilir derinlik sınırı → yakalanabilir `E_STACK_OVERFLOW` (VM ve JIT aynı).
- [ ] ⚑ Null struct erişimi yakalanamıyor — `src/vm/interpreter.cpp` FIELD_GET/SET `std::runtime_error("not a struct")`; JIT farklı mesaj; non-nullable öz-referans alan (`Node next;`) başlatma zinciri kesilince null kalıyor.
  FIELD_GET/SET yakalanabilir `E_NULL`; non-nullable struct döngüsü için E010'u geri aç (`Node? next` zorunlu).
- [ ] P-21 + ⚑: Ondalık literal varsayılanı float32, belge double diyor; `double d = 1.0/3.0` → `0.3333333433` — type checker literal tiplemesi.
  Bağlamsız ondalık literal double; yalnız float bağlamında float.
- [ ] ⚑ Büyük int literal longint bağlamını almıyor (`l + 3000000000` → E003) — literal tiplemesi yalnız doğrudan ilk değer/atamada.
  Diğer operand longint ise literal longint tiplensin.
- [ ] ⚑ Bilinmeyen kaçış dizisi sessizce yutuluyor (`"\x41"` → `x41`) — tokenizer/lexer string kaçışları.
  Bilinmeyen `\?` derleme hatası.
- [ ] ⚑ CLI sessiz kabul: `run a.sqt foo bar` argümanları düşürüyor, `--jitt` yok sayılıyor, `--gc-treshold=5` modül adı sanılıyor, `--runs=abc` yutuluyor, argümansız `run` → `source.sqt` (P-27/28/29) — `src/cli/args.hpp`.
  Bilinmeyen bayrak/hatalı sayı → kullanım hatası 64; fazla konumsal argüman hata; `source.sqt` varsayılanı kaldır; `-` uygula ya da kaldır.
- [ ] ⚑ Kullanıcıya Türkçe mesajlar: `sıfıra bölme (mod)`, `negatif üs…`, JIT "bu programi tam olarak derleyemiyor" — `interpreter.cpp`, `mir_backend.cpp`.
  İngilizce; VM/JIT tek mesaj.
- [ ] ⚑ Tanılarda değişken adı yerine `'assignment'` (`int? assignment = null;`) — `src/semantic/type_checker.cpp` atama `checkAssign` çağrısında sabit bağlam.
  Hedefin adı/ifade metni geçirilsin.
- [ ] P-37: `Error.toJson()` `field0..4` üretiyor — yerleşik Error layout'u serileştirmede ad taşımıyor.
  Error, kullanıcı struct'larıyla aynı adlı layout yolunu kullansın.

### 10.2 JIT (deneysel) — v1'de VM≡JIT iddia edilecekse hemen, değilse sonra

- [ ] ⚑ Kullanıcı fonksiyonundan dönen `string?` JIT'te bozuk (`print` sayı basıyor, `==` başarısız; btree-kv demo FAIL) — `src/mir/mir_backend.cpp` nullable ref dönüş yolu.
  Nullable dönüşü Value olarak taşı; diff harness'e ekle.
- [ ] ⚑ Nullable operandlı `FIELD_SET`/`STORE_GLOBAL`/`STRING_CONCAT` JIT'te desteklenmiyor (xml-tool, linked-list örneği derlenmiyor) — `mir_backend.cpp`.
  Nullable Value yolu ekle.
- [ ] ⚑ Struct alanından/globalden okunan `float` host çağrısına verilince MIR tip hatası (float vs double) — `mir_backend.cpp` field/global yükleme.
  Çağrı öncesi float → double genişlet.

### 10.3 v1 sonrası

- [ ] P-4/P-15: `print(dizi)` `<ref>`, `toString` yalnız `byte[]`, `dump` iç içe `<ref>` — `src/data/array.cpp`, struct dump.
  `[1, 2, 3]` gösterimi, dizi `toJson`, özyinelemeli `dump`.
- [ ] P-5: `T?[]` ayrıştırılamıyor — parser tip ayrıştırma `?`'yi yalnız `[]` sonrası kabul ediyor.
  Eleman tipinden sonra `?`; eleman türü Value (boxed).
- [ ] `>>>` mantıksal kaydırma, string sıralaması (P-16), varsayılan parametre/aşırı yükleme (P-36), `auto`, `finally` — dilde yok.
  Ürün kararıyla ayrı özellikler (`>>>` P-32 düzeltmesiyle hemen sözdizimi hatası olur).
- [ ] ⚑ `saqut ir` dökümü `LOAD_FLOAT32` ve `F32*` operandlarını göstermiyor — `src/ir/ir_dump.hpp`.
  Eksik opcode'lar için operand biçimleri.
- [ ] Doküman/örnek sapmaları: P-9..13, P-20, P-22, P-23, P-34, P-35; `upper()` Unicode iddiası (gerçekte ASCII); `length` bayt/kod noktası çelişkisi (gerçekte kod noktası); knowledge-base; `examples/error.sqt` derlenmiyor; csv örneği `--allow` diyor ve her çalıştırmada kendi `data.csv`'sine kayıt ekliyor; library/csv workaround yorumları bayat (P-14).
  Tek geçişte doküman+örnek düzeltmesi; `examples/` VM ve JIT ile CI'da koşulsun.
