# 0.9.9 — Doğrulama sürümü (geliştirme yok)

Bu dal **yeni özellik içermez**. Amacı tek: 0.1'den 0.9'a kadar yapılan her
şeyin gerçek bir uygulama yazılırken çalışıp çalışmadığını görmek.

Ürün sahibi kararı (2026-09-22): dil, kendisini kullanan biri tarafından
kullanılarak doğrulanır. Test paketi bu işin aracı değildir — **uygulamanın
kendisi araçtır**. Test paketi zaten geçiyor; geçmesi bir şeyin çalıştığını
kanıtlamıyor, yalnızca test edilmiş olanın çalıştığını kanıtlıyor.

## Yöntem

Basitten karmaşığa, her adımda çalışan bir program:

1. Değişkene değer atama gibi en küçük şeyden başla.
2. Her adımda bir önceki adımın üstüne koy.
3. Sonunda runtime'da **sürekli çalışan** (uzun ömürlü, durum tutan,
   girdi işleyen) uygulamalara çık.

Kapsam dışı: LSP ve DAP.

## Durma kuralı

**Bir şeyin ters gittiği fark edildiği anda durulur.** Etrafından dolaşılmaz,
workaround yazılmaz, "şimdilik böyle yapayım" denmez. Çünkü workaround'un
kendisi bulgunun kaybolması demektir: dili kullanan bir insan o noktada
takılacak ve bizim bunu görmemiz gerekiyor.

Durunca:
- Ne yapılmak istendiği, ne olduğu ve ne beklendiği yazılır.
- En küçük repro çıkarılır.
- VM ve JIT ayrı ayrı koşulur (ayrışma kendi başına bir bulgudur).
- Issue açılır, ürün sahibine sorulur.

Bulgu "küçük" diye atlanmaz. Bu turda `a[0]++`'ın sessizce hiçbir şey
yapmaması da, `a[0] += 5`'in derleyiciyi segfault etmesi de kimsenin
aramadığı yerlerden çıktı.

## Neden test paketi yetmiyor

0.9.8'de `bash tests/run.sh` tertemiz geçiyordu. Aynı gün, belge yazmak için
operatörleri tek tek denerken altı kusur çıktı; üçü hiçbir issue'da yoktu:

| Bulgu | Test paketi neden görmedi |
|---|---|
| `a[0]++` sessizce çalışmıyor | Fixture'da `++` hep yerel değişkendeydi |
| `a[0] += 5` derleyiciyi çökertiyor | Birleşik atama hep `x += n` biçiminde yazılmıştı |
| `f += 1.0` sessizce 0 | Aynı sebep |
| `2 ** 3` sessizce 0 | `**` hiç kullanılmamıştı |
| `2 ** 3 ** 2` = 64 | Sağ birleşim hiç denenmemişti |
| `string?` JIT'te ham işaretçi | Fixture'lar deterministik olsun diye `env()` kullanmıyordu |

Ortak nokta: **hiçbiri yanlış cevap vermiyordu, hepsi sessizdi.** Diferansiyel
harness (VM≡JIT) de yakalayamaz, çünkü iki backend aynı bozuk IR'ı alıyordu.

Bu yüzden 0.9.9'un aracı harness değil, dili gerçekten kullanmak.

## Dizin

Her adım kendi klasöründe: kaynak, nasıl çalıştırıldığı, gözlenen davranış.
Buradaki programlar `tests/run.sh`'a bağlanmaz — amaçları geçmek değil,
kullanılmak.
