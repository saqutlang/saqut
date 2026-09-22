# Bulgu 01 — `bool` gösterimi tutarsız: `print` `1`, `as string` `true`

**Adım:** 01-degiskenler / 02_tipler.sqt (her tipten bir değişken yazdır)
**Ölçüm:** `0.9.9`, commit `9adf302`, Release
**VM ≡ JIT:** evet, ikisi de aynı. Backend kusuru değil, dil kararı.

## Ne oldu

En küçük adımda, her tipten bir değişkeni yazdırırken:

```c
bool t = true;
print(t);          // 1      ← beklenen: true
```

Derleyici bool'un metin karşılığını **biliyor**, ama `print` onu kullanmıyor:

```c
print(t);                        // 1
print(t as string);              // true
print("deger: " + (t as string)); // deger: true
```

## Kapsam

| Bağlam | Çıktı |
|---|---|
| `print(t)` | `1` |
| `t as string` | `true` |
| `"x" + (t as string)` | `deger: true` |
| `bool[]` elemanı | `1` |
| struct alanı | `1` |
| `struct.toJson()` | `{"bayrak":1}` |

## En ciddi kısmı: `toJson()` tip kaybı veriyor

```c
struct K { bool bayrak; int sayi; }
K k; k.bayrak = true; k.sayi = 5;
print(k.toJson());     // {"bayrak":1,"sayi":5}
```

Python ile doğrulandı: sözdizimi geçerli ama **tip yanlış**.

```
$ python3 -c "import json; d=json.loads(open('out.json').read()); print(type(d['bayrak']))"
<class 'int'>          ← beklenen: <class 'bool'>
```

JSON'da boolean ayrı bir tiptir (`true`/`false`). `1` üreten bir serileştirici,
veriyi dışarı veren her programda tip bilgisini kaybediyor. saQut'tan çıkan
JSON'u okuyan taraf `bayrak` alanının bool olduğunu anlayamaz.

## Belge ne diyor

`data-types.md:80`:

> A logical value, either `true` or `false`. Stored internally as an `int`
> (1 for `true`, 0 for `false`).

Yani `print(t)` → `1` belgeyle çelişmiyor. Çelişen şey `as string`'in `true`
vermesi: aynı değerin iki gösterimi var ve hangisinin doğru olduğu yazılı
değil. `toJson` ise belgeden bağımsız olarak yanlış — JSON'un kendi tip
sistemi var.

## Karar gerekli (ürün sahibi)

Üç seçenek:

1. **`print(bool)` → `true`/`false`, `toJson` → `true`/`false`.** Tutarlı ve
   JSON doğru olur. Bedeli: `print(1 < 2)` artık `true` basar, mevcut
   fixture'ların çıktısı değişir. **Sayıldı: `bool` geçen 9 golden fixture
   var** (`grep -rl bool tests/golden --include=*.sqt`), yani maliyet sınırlı.
2. **Yalnız `toJson` düzeltilsin.** `print` bugünkü gibi `1` kalır (belgeyle
   uyumlu), ama JSON tip olarak doğru olur. Dar değişiklik.
3. **Bugünkü davranış kalsın, belge netleştirilsin.** `as string` ile `print`
   arasındaki fark yazılır. JSON tip kaybı kabul edilir.

Ben (1) veya (2)'yi öneriyorum; (3) `toJson`'ın ürettiği veriyi tüketen
tarafta sorunu bırakır.

## Kanıt sınıfı

Doğrulanan: yukarıdaki altı bağlamın çıktısı, VM ve JIT'te birebir aynı;
JSON çıktısı harici parser (python `json`) ile okundu ve tipi ölçüldü.
Doğrulanmayan: 9 fixture'ın kaçının çıktısının GERÇEKTEN değişeceği (bool
geçmesi, bool YAZDIRMASI demek değil) tek tek bakılmadı; `bool` dönen builtin
metodların (`contains`, `startsWith` vb.) çıktısı bu turda denenmedi.
