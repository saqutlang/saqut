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
