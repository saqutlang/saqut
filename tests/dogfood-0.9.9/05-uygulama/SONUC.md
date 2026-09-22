# Adım 5 — İlk gerçek uygulamalar

Buraya kadarki her şeyin bir arada kullanıldığı, iş yapan programlar.
Özellik denemek değil, çalışan bir şey yazmak amaçlandı.

## `envanter.sqt` — stok takibi

struct + enum + dizi + string + double aritmetiği + fonksiyonlar.
Ekleme, arama, stok düşme (yetersiz stok ve olmayan ürün dahil), raporlama.

Sonuç: **doğru**. Bütün tutarlar python ile ayrıca hesaplandı ve birebir
eşleşti (50×12.5 + 20×45.0 + 100×8.75 = 2400.0; 10 satış sonrası 2275.0).
VM ve JIT çıktısı `diff` ile birebir aynı.

## `envanter_yuk.sqt` — ölçek

5000 ürün, 20000 stok hareketi, 200 lineer arama.

```
baslangic degeri: 5000000.0      (5000 × 100 × 10.0)
son deger:        4800000.0      (20000 birim × 10.0 düşüldü)
VM 0.055s   JIT 0.034s
```

Sonuç: **doğru ve hızlı**.

## `surekli.sqt` — SÜREKLİ ÇALIŞAN uygulama

Uzun ömürlü, durum tutan, her turda tahsis yapan kuyruk simülasyonu.
Kısa ömürlü string trafiği + canlı kalan küçük bir küme.

| tur | islenen | bekleyen | live | liveBytes | peakBytes | süre |
|---|---|---|---|---|---|---|
| 1.000 | 3.400 | 5 | 6733 | 478.361 | 526.041 | 0.00s |
| 10.000 | 34.020 | 5 | 3037 | 216.743 | 526.041 | 0.08s |
| 50.000 | 170.100 | 5 | 976 | 70.759 | 526.041 | 0.40s |
| 200.000 | 680.407 | 5 | 5608 | 405.249 | 526.191 | 1.72s |

**Bu tablo iyi haber:**

- `bekleyen` her ölçekte **5** — canlı küme sabit, program kendi durumunu
  doğru yönetiyor.
- `peakBytes` 200 kat ölçek artışında **~526 KB'de yatay** (526.041 →
  526.191). Sızıntı yok.
- Süre **lineer** ölçekleniyor (10k→200k, yani 20 kat: 0.08s → 1.72s ≈ 21 kat).
  Süper-lineer davranış yok (krş. #206).

### Determinizm

Aynı girdiyle 3 koşu, **GC sayaçları dahil bit-bit aynı**:

```
islenen: 170100  toplam: 8161560  bekleyen: 5
gc: collections=349 freed=2549045 live=976 liveBytes=70759 peakBytes=526041
```

Üç koşuda da aynı satır. Ürün sahibinin tarif ettiği determinizm tanımı
(aynı kod + aynı girdi + aynı backend → aynı sonuç, her yerde) bu iş yükünde
sağlanıyor.

### VM ≡ JIT

200.000 turda çıktı `diff` ile birebir aynı. JIT 1.07s, VM 1.72s.

## Bu adımda bulgu çıkmadı

Adım 1-4'te üç bulgu çıkmıştı (I-01, I-02, I-03). Adım 5'te — yani her şeyin
bir arada çalıştığı yerde — yeni bir kusur görülmedi. Uygulamalar doğru
çalışıyor, uzun koşuda kararlı ve deterministik.
