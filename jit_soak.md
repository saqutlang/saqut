# Görev: JIT soak koşusu ve raporu

Bu bir uygulama görevi değil, **ölçüm ve kanıt** görevidir. Kod değişikliği
beklenmiyor; beklenen şey, JIT backend'inin uzun koşuda kararlı olup
olmadığının ölçülmesi ve sonucun kayda geçirilmesi.

## Önce oku

`AGENTS.md` bağlayıcı çalışma sözleşmesidir; §4 (şüpheci denetim yöntemi) ve
§5 (dört aşamalı Definition of Done) bu görev için özellikle geçerlidir.
Kanıtsız başarı cümlesi kurulmaz.

## Bağlam

VM tarafı ölçüldü ve temiz çıktı (#112, 2026-09-22 tarihli yorum):
110 milyon iş turu, RSS sabit, canlı nesne sayısı 40 dilim boyunca **6554**
(tek sapma yok), toplama sayısı **30008** her dilimde aynı — tempo
deterministik.

**JIT tarafı hiç koşulmadı.** Bu görev onu kapatır.

Araç hazır: `tests/soak/soak_kosucu.sh` + `tests/soak/is_yuku.sqt`
(commit `357a7e3`).

## Koşu

```
./build-release.sh                       # Release ZORUNLU, aşağıdaki nota bak
tests/soak/soak_kosucu.sh 40 2000000 --jit
```

Uzun sürer (VM'de ~1 saat 50 dakika; JIT daha hızlı ama iş yükü
CALLHOST ağırlıklı olduğundan fark büyük olmayabilir). Arka planda koşturmak
ve süre sınırı koymak mantıklı:

```
nohup timeout 7200 tests/soak/soak_kosucu.sh 40 2000000 --jit > jit_soak.log 2>&1 &
tail -f jit_soak.log
```

`timeout` süreyi keserse sondaki özet bloğu basılmaz; tablo yine okunabilir.

### Release build neden zorunlu

Script Debug build'de çalışmayı reddeder, bilerek: ASan kendi allocator'ını
kullanır ve redzone ekler — o profildeki RSS eğrisi GC'nin değil ASan'ın
davranışıdır ve fragmantasyon hiç görünmez. Bellek güvenliği ayrı bir kanıt
sınıfıdır (ASan/UBSan taramaları zaten yapıldı: VM 139/139, JIT 133/133,
UBSan 145/145 temiz).

## Ne ölçülüyor

Script iki ayrı ölçüm yapar; **ayrımı önemlidir**:

**Ölçüm 1 — süreç-içi büyüme.** Tek süreç artan tur sayısıyla koşar
(1x, 2x, 4x, 8x). Sızıntı ancak böyle görünür; her dilimde yeni süreç
başlatmak birikimli etkiyi sıfırlar ve sızıntıyı gizler.

**Ölçüm 2 — dilim tekrarı.** Aynı iş tekrar tekrar koşulur; tur süresinin
kayıp kaymadığına bakılır (tempo kayması, ısınma).

## Geçme ölçütü

- **RSS yatay kalmalı.** 8 kat iş için anlamlı büyüme olmamalı. Sızıntı
  monoton artar; bir kez sıçrayıp platoya oturan bir değer sızıntı değildir
  (VM'de dilim 9'da %8 sıçrayıp 31 dilim sabit kaldı — glibc malloc arena
  büyütmesi).
- **Canlı nesne sayısı sabit kalmalı** (dilim tekrarında; aşağıdaki tuzağa
  dikkat).
- **Tur süresi kaymamalı.** ±%5 makine gürültüsüdür.

## ÖLÇÜM TUZAĞI — `live` sütununu ham haliyle okuma

`--gc-stats` program **bitiminde** tek anlık görüntü alır:

```
live = gerçek canlı küme + son toplamadan bu yana biriken çöp
```

Ölçüm 1'de tur sayısı değiştiği için program toplama döngüsünün farklı
noktalarında biter ve `live` **dalgalı görünür** — bu sızıntı değil, testere
dişi eğrisinin farklı noktalarında yakalanmaktır. VM koşusunda doğrulandı:
her ek tur `live`'ı tam 92 artırıyor (bir turun ürettiği çöp), eşik dolunca
sıfırlanıyor.

Gerçek canlı kümeyi görmek için toplamayı zorla:

```
./build/saqut run --jit --gc-threshold=1024 --gc-stats tests/soak/is_yuku.sqt -- 200000
```

VM'de bu yolla ölçüldüğünde 100 kat iş için canlı küme **azalıyordu**
(705 → 613 → 521).

**RSS bu ölçümün güvenilir göstergesidir** — işletim sisteminden alınan
bellek toplama arasında geri verilmez, testere dişi yapmaz.

## JIT'e özgü dikkat edilecekler

VM'de olmayan, yalnız JIT'te olabilecek sorunlar:

1. **Kod belleği büyümesi.** JIT native kod üretir ve `mmap`'le sayfa alır.
   Uzun koşuda kod belleği büyüyor mu? `/proc/<pid>/status` → `VmData`,
   ya da `strace -c -e trace=mmap,munmap,mprotect` ile sayım.

2. **Shadow stack büyümesi.** `jitShadowStack()` GC kökü olarak taranır;
   `enter`/`leave` dengesizliği olursa dizi monoton büyür ve hem bellek hem
   toplama maliyeti artar.

3. **Koşu heap'inin ömrü.** JIT heap'i artık koşuya bağlı (`RunHeapBinding`,
   commit `b33d569`) — süreç ömrüne değil. Tek koşuda bu görünmez; ama
   aynı süreçte arka arkaya program çalıştıran bir senaryo denenirse
   heap'in gerçekten yıkıldığı doğrulanmalı.

4. **VM ile karşılaştırma.** JIT sayaçları (`collections`, `freed`, `live`)
   VM'inkilerle **aynı olmalı** — iki backend aynı GC çekirdeğini ve aynı
   politikayı paylaşıyor (ADR-022). Ayrışma varsa bu bir bug'dır, tempo
   farkı değil.

## Rapor

Sonucu **#112'ye yorum olarak** yaz. Şunları içermeli:

- Koşu parametreleri (dilim × tur, backend, build tipi, revizyon hash'i)
- Ölçüm 1 tablosu (tur ↔ RSS ↔ canlı nesne)
- Ölçüm 2 tablosu veya özeti (dilim ↔ süre ↔ RSS ↔ toplama)
- VM sonucuyla karşılaştırma — özellikle `collections` sayısı
- **Doğrulanmayanlar** açıkça listelenmeli (AGENTS.md §10.4)

Sızıntı veya kayma bulunursa: kök nedeni aramaya girmeden önce **bulguyu
kaydet**; düzeltme ayrı bir görevdir.

## Yapma

- Debug/ASan build ile ölçme (yukarıdaki gerekçe).
- `sudo`, `nice -20` gibi ayarlar — ölçtüğümüz şey süre değil RSS ve canlı
  küme eğrisi; CPU önceliğinden etkilenmezler.
- VM koşusunu tekrar etme; o tamamlandı ve #112'de kayıtlı.
- `tests/soak/` altındaki dosyaları değiştirme — aynı araçla ölçüm yapılması
  karşılaştırmanın ön koşulu. Araçta bir kusur görürsen raporla.
