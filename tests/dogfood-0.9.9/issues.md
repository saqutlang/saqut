# 0.9.9 doğrulama turu — açılacak issue'lar

Bu dosya, dogfooding sırasında bulunan ve **ürün sahibi kararı bekleyen**
konuların kaydıdır. Issue'lar daha sonra buradan açılacak.

Her madde: ne bulundu, ne ölçüldü, hangi seçenek onaylandı.

---

## I-01 — `bool` gösterimi tutarsız (`print` → `1`, `as string` → `true`)

**Durum:** karar verildi (seçenek 1 ve 2 onaylandı, 2026-09-22)
**Bulgu dosyası:** `01-degiskenler/BULGU-01-bool-gosterimi.md`
**Ölçüm:** `0.9.9`, commit `9adf302`, Release. VM ≡ JIT (backend kusuru değil).

### Sorun

Aynı `bool` değerinin iki gösterimi var ve hangisinin doğru olduğu yazılı
değil:

```c
bool t = true;
print(t);              // 1
print(t as string);    // true
```

| Bağlam | Bugünkü çıktı |
|---|---|
| `print(t)` | `1` |
| `t as string` | `true` |
| `bool[]` elemanı | `1` |
| struct alanı | `1` |
| `struct.toJson()` | `{"bayrak":1}` |

### En ciddi kısmı: `toJson()` tip kaybı

```
{"bayrak":1,"sayi":5}
```

Harici parser ile doğrulandı (python `json.loads`): sözdizimi geçerli ama
`bayrak` alanı **`int` olarak okunuyor**, `bool` değil. JSON'da boolean ayrı
bir tiptir; `1` üreten serileştirici, veri dışarı verildiğinde tip bilgisini
kaybediyor.

### Onaylanan çözüm (1 + 2)

1. **`print(bool)` → `true` / `false`** olacak.
2. **`toJson()` → `true` / `false`** olacak (JSON tipi doğru olsun).

Yani gösterim her yerde tek ve tutarlı olacak; `as string` zaten `true`
veriyor, diğerleri ona uyacak.

### Uygulama notları

- `print(1 < 2)` artık `1` değil `true` basacak. Karşılaştırma sonucu `bool`
  olduğu için bu beklenen sonuç, ama **mevcut fixture çıktıları değişir**.
- `bool` geçen golden fixture sayısı: **9**
  (`grep -rl bool tests/golden --include=*.sqt`). Hepsinin bool YAZDIRMADIĞI
  not edilmeli; gerçek etki daha küçük olabilir.
- `bool` dönen builtin metodlar **aynı yoldan geçiyor, doğrulandı**:
  `s.contains(...)`, `s.startsWith(...)`, `s.endsWith(...)`, `a.contains(...)`
  hepsi `1`/`0` basıyor. Düzeltme bunları da kapsamalı.
- `data-types.md` ve `tr/data-types.md`: "Stored internally as an `int`
  (1 for `true`, 0 for `false`)" cümlesi kalabilir (iç temsil doğru), ama
  gösterim artık `true`/`false` olduğu için o kısım netleştirilmeli.
- Belge örneklerinde `print(bool)` kullanan yerler yeniden koşulmalı.

### Kanıt sınıfı

Doğrulanan: altı bağlamın çıktısı VM ve JIT'te birebir; JSON çıktısı harici
parser ile okundu ve tipi ölçüldü.
Doğrulanmayan: 9 fixture'ın kaçının çıktısının gerçekten değişeceği tek tek
bakılmadı.

---
## I-02 — Runtime hata mesajlarında dil karışık (7 İngilizce, 2 Türkçe)

**Durum:** karar bekliyor
**Bulgu:** `02-ifadeler/03_hatalar.sqt` (sıfıra bölme denemesi sırasında)
**Ölçüm:** `0.9.9`, commit `9adf302`, Release. VM ≡ JIT.

### Sorun

Kullanıcıya görünen runtime hata mesajları iki dilde:

```
$ saqut run bol0.sqt
runtime error: division by zero          ← İngilizce

$ saqut run mod0.sqt
runtime error: sıfıra bölme (mod)        ← Türkçe
```

Aynı hata kodunun (`E_DIVZERO`) iki dilde iki mesajı var.

### Tam envanter

`grep -rhoP '(jitSetError|makeErrorValue)\("\K[^"]+' src/`

| Mesaj | Dil |
|---|---|
| `division by zero` | EN |
| `float division by zero` | EN |
| `decimal division by zero` | EN |
| `decimal modulo by zero` | EN |
| `decimal overflow` | EN |
| `expected array, got different type` | EN |
| `throw` | EN |
| `sıfıra bölme (mod)` | **TR** |
| `negatif üs tamsayıda tanımsız` | **TR** |

**Derleme zamanı tanıları (E001/E003/W001...) tamamen İngilizce** — taramada
tek bir Türkçe karakter çıkmadı. Yani konvansiyon İngilizce; runtime
tarafındaki iki mesaj istisna.

### Kaynağı

- `sıfıra bölme (mod)`: VM'in ilk commit'inden beri var (`e488f29`).
- `negatif üs tamsayıda tanımsız`: **bu oturumda `**` eklenirken benim
  yazdığım mesaj.** Komşu satırdaki Türkçe mesaja bakıp ona uydurdum;
  baskın konvansiyonu kontrol etmem gerekirdi. `E_POWNEG` henüz yayınlanmadı
  (0.9.9'da yeni), yani şimdi düzeltmenin maliyeti sıfır.

### Seçenekler

1. **İkisi de İngilizce olsun.** Konvansiyona uyar, tek dil. Fixture
   `tests/golden/arithmetic/mod_by_zero.runtime_error` güncellenir (regex
   `sıfıra bölme \(mod\)` → yeni metin) ve
   `tests/golden/operators/pow_negative_exponent.runtime_error` da.
2. **Şimdilik yalnız `E_POWNEG` düzeltilsin** (benim eklediğim, henüz
   yayınlanmamış). `sıfıra bölme (mod)` tarihsel olarak yerinde kalır,
   ayrı bir karara bırakılır.
3. **Mesajlar Türkçeleşsin.** Derleme tanılarının tamamı İngilizce olduğu
   için bu çok daha büyük bir iş (ve ayrı bir ürün kararı: saQut'un tanı
   dili ne olacak?). #131 (diagnostic mesajları merkezi değil, ADR-038
   deadline'lı) ile birlikte ele alınmalı.

Öneri: **(1)**. Eğer ileride tanı dili Türkçe olacaksa bu #131 kapsamında
topluca yapılmalı, tek tek değil.

### Kanıt sınıfı

Doğrulanan: yukarıdaki iki çıktı VM ve JIT'te; tam mesaj envanteri kaynak
taramasıyla; derleme tanılarında Türkçe bulunmadığı taramayla.
Doğrulanmayan: `throw` ile kullanıcı tarafından fırlatılan mesajlar (onlar
kullanıcının yazdığı metin, konvansiyon dışı) sayılmadı.

---
## I-03 — Derin rekürsiyonda JIT segfault, VM sınırsız (VM ≢ JIT)

**Durum:** karar bekliyor
**Bulgu dosyası:** `03-akis/BULGU-03-rekursiyon-derinligi.md`
**Ölçüm:** `0.9.9`, commit `031cc2a`, Release, `ulimit -s` = 8192

### Sorun

```c
int derin(int n) { if (n <= 0) { return 0; } return 1 + derin(n - 1); }
print(derin(1000000));
```

```
VM  → 1000000          (5M'de bile çalışıyor)
JIT → Segmentation fault (core dumped), rc=139
```

Eşik ~250k–270k. `ulimit -s 16384` ile eşik kalkıyor → **JIT native C
yığınını kullanıyor**, VM kendi heap'teki `callStack_`'ini.

### İki ayrı sorun

1. **JIT sessizce ölüyor.** SIGSEGV yakalanamaz, tanı üretmez, `try/catch`
   ile tutulamaz. Kullanıcı yalnız "Segmentation fault" görür.
2. **VM'in hiç sınırı yok.** `interpreter.cpp`'de derinlik kontrolü yok;
   sonsuz rekürsiyon OOM'a kadar gider.

Üçüncüsü: bu ikisi birlikte **VM ≡ JIT sözleşmesini bozuyor** — aynı program
bir backend'de çalışıp diğerinde çöküyor.

### Sorular

1. Tanımlı bir derinlik sınırı olacak mı? Kaç, ve aşılınca yakalanabilir
   hata mı yoksa ölümcül mü?
2. VM ve JIT aynı sınırı mı paylaşacak? (Determinizm sözleşmesi bunu
   gerektiriyor gibi.)
3. JIT'in native yığın kullanımı tasarım gereği mi, yoksa kendi yığınına mı
   taşınacak? Sınırın nereye konabileceğini bu belirliyor.

### İlgili

#213 ("rekürsif fonksiyonlar stack/return/GC ve determinism sınırlarını
ölçsün") tam olarak bunu istiyor, hâlâ açık. Bu bulgu o issue'nun somut
kanıtı olabilir.

---
## I-04 — `try`/`catch` ikisi de `return` etse bile E006

**Durum:** karar bekliyor
**Bulgu dosyası:** `06-hata-null/BULGU-04-trycatch-return.md`
**Ölçüm:** `0.9.9`, commit `1fa6562`. Derleme zamanı; backend farkı yok.

### Sorun

```c
int guvenliBol(int a, int b) {
    try   { return a / b; }
    catch (Error e) { return -1; }
}
```

```
error [E006]: 'guvenliBol' function must return int but some paths have no return
```

Her iki yol da return ediyor. Aynı yapı `if/else` ile yazılınca **kabul
ediliyor**.

### Kök neden

`type_checker.cpp:135` `pathAlwaysReturns()` içinde `IfStatement` ve
`SwitchStatement` case'leri var, **`TryStatement` case'i yok** → `default:
return false`.

Doğru kural `if/else` ile aynı: try bloğu VE catch bloğu ikisi de garantili
dönüyorsa, try/catch garantili döner.

### Kullanıcının ödediği bedel

Ulaşılamaz bir satır yazmak zorunda:

```c
    try { return a / b; } catch (Error e) { return -1; }
    return 0;      // ULAŞILAMAZ ama zorunlu
```

Üstelik bu satır için **ulaşılamaz kod uyarısı da verilmiyor** (`saqut check`
0 error 0 warning). Derleyici hem ölü kodu zorunlu kılıyor hem de ölü
olduğunu söylemiyor.

### Neden önemli

"Hatayı yakala, varsayılan dön" try/catch'in birincil kalıbı. Dili kullanan
biri ilk try/catch'li fonksiyonunda buna çarpar.

### Öneri

`pathAlwaysReturns`'e `TryStatement` case'i eklensin. Tek yerde, ~5 satır.

---

## I-05 — `finally` ayrılmış ama uygulanmamış

**Durum:** karar bekliyor (düşük öncelik)
**Ölçüm:** `0.9.9`, commit `1fa6562`

`finally` tokenizer'da anahtar kelime olarak tanımlı
(`token.hpp:191` `KW_FINALLY`, `token.hpp:476` kelime tablosu) ama
**`src/` altında .cpp tarafında hiç kullanılmıyor** — parser'da karşılığı,
AST'de alanı yok.

```
$ saqut run fin.sqt
error [E901]: unexpected token 'finally' — expected a statement
```

**İyi haber:** sessizce yanlış çalışmıyor, açıkça hata veriyor. `**`'ın eski
durumundan (sessizce 0) farklı olarak bu güvenli taraf.

Yine de kullanıcıya görünen durum kafa karıştırıcı: `finally` bir anahtar
kelime, yani değişken adı olarak da kullanılamaz, ama bir iş de yapmıyor.

### Seçenekler

1. **Uygulansın.** try/catch/finally tam olur. I-04 ile birlikte
   `pathAlwaysReturns` kuralı da finally'yi hesaba katmalı.
2. **Anahtar kelime listesinden çıkarılsın.** Kullanılmayan rezervasyon
   kaldırılır, `finally` sıradan bir tanımlayıcı olur.
3. **Rezerve kalsın, belgede "ileride" diye yazılsın.** Bugünkü durum, ama
   yazılı hale gelir.

Not: hata mesajı "unexpected token 'finally'" diyor; eğer (3) seçilirse
mesaj "finally is reserved but not implemented yet" gibi açık bir şey
olmalı.

---
