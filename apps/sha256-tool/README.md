# sha256-tool — saQut ile SHA-256 (FIPS 180-4)

Saf saQut SHA-256 uygulaması + küçük terminal aracı. Bit düzeyi işlemleri,
`byte[]` taşımayı, 32-bit taşma sarmasını ve uzun tamsayı yolunu gerçek bir
kriptografik algoritmada zorlar.

## Kullanım

```bash
./build/saqut run apps/sha256-tool/main.sqt -- <dosya>       # dosya özeti
./build/saqut run apps/sha256-tool/main.sqt -- -s "metin"    # metin özeti
./build/saqut run apps/sha256-tool/main.sqt -- --self-test   # NIST vektörleri
```

Çıkış kodları: `0` başarılı, `1` self-test başarısız, `64` kullanım, `66` dosya
yok.

## Doğrulama

`--self-test` beş NIST/spec vektörünü koşar (boş, `abc`, iki bloklu mesaj,
hızlı kahverengi tilki, bir milyon `a`). Hepsi geçer.

Harici oracle (`sha256sum`) çapraz kontrolü de yapıldı:

```text
metin "merhaba dünya"      a21fb229b1086766…  == sha256sum
apps/xml-tool/…/katalog.xml c827e681f4ea256d…  == sha256sum
build/saqut (6 MB ikili)   7b6a7403d4404f8d…  == sha256sum
/tmp 1 MB, 4 MB rastgele   0e65ca763ceb9276…, e74ec172576ac4dd…  == sha256sum
```

## Dil kaynaklı notlar (ölçülmüş)

- `>>` **aritmetiktir** (işaret genişletir, ör. `-8 >> 1 == -4`); belgeler bunu
  söylemez (bkz. `../BULGULAR.md` B-08). SHA-256 mantıksal kaydırma ister;
  `lshr()` maske ile yapar: `(x >> n) & (0x7FFFFFFF >> (n-1))`.
- `int` toplama 2^32 modunda sarar; `+` zincirleri SHA-256 için doğrudur.
- 32-bit sabitler `> 2^31` ise negatif ondalık yazılmalı (B-07).
- `longint` doğrudan `byte`'a cast edilemez ("only 'int' can be cast to byte");
  `as int as byte` gerekir (B-09).
- `int` → `byte` cast'i aralık dışında hata verir; 0..255 dışına çıkılmaması
  kod tarafında garanti edilir.

## Performans (bilgi — hata değil)

VM üzerinde ölçüm: ~0.35 MB/s (1 MB → 2.8 s, 4 MB → 10.4 s, 6 MB → 17.1 s;
lineer). `sha256sum` aynı 6 MB'ı 0.028 s'de işler. Fark, yorumlanan VM +
bayt-başına dizi erişimi + mesaj dolgusunda bayt-başına `push` maliyetinden
gelir; süper-lineer davranış gözlenmedi (bkz. `../BULGULAR.md` B-10).
