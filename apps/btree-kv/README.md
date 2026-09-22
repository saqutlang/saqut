# btree-kv — saQut ile B-tree kalıcı anahtar-değer deposu

Arena tabanlı B-tree (minimum derece `t=2`), metin dosyasına kalıcılık ve
stdin'den komut okuyan uzun ömürlü bir REPL. Struct/dizi/özyineleme, referans
semantiği, dosya IO, `stdin::readLine` döngüsü ve `date` FFI'sini gerçek bir
depo uygulamasında zorlar.

## Kullanım

```bash
# etkileşimli REPL (isteğe bağlı: başlangıç dosyası yükler)
./build/saqut run apps/btree-kv/main.sqt -- repl [dosya]

# öz-doğrulama dizisi (1000 kayıt, split, sıralama, silme)
./build/saqut run apps/btree-kv/main.sqt -- demo

# n anahtar ekle ve ölç
./build/saqut run apps/btree-kv/main.sqt -- bench 20000
```

Exit: `0` başarılı, `1` demo başarısız, `64` kullanım, `66` dosya hatası.

## REPL komutları

```text
set <anahtar> <deger>     ekle/güncelle
get <anahtar>             oku (yoksa "(yok)")
del <anahtar>             tombstone sil
keys | dump               sıralı anahtar listesi / anahtar=deger
range <lo> <hi>           [lo, hi] aralığı
count | height            canlı kayıt sayısı / ağaç yüksekliği
save <dosya> | load <dosya>
help | quit
```

## Tasarım notları

- **Arena + indeks:** düğümler tek `BNode[]` dizisinde; çocuklar indeks
  tutar. Böylece özyinelemeli yapı dizi-içi-struct referansıyla kurulur
  (dil, dizi elemanı struct mutasyonunu destekliyor — ölçüldü).
- **Önleyici bölme (preemptive split):** kökten aşağı inerken dolu çocuk
  bölünür (CLRS yaklaşımı).
- **Silme = tombstone:** `Entry.deleted` işaretlenir; okuma/tarama atlar,
  `save` sıkıştırır. Gerçek düğüm birleştirme/ödünç alma kapsam dışı.
- **String sıralaması elle:** saQut'ta string `<`/`>` yok (bkz.
  `../BULGULAR.md` B-11); `cmpStr` UTF-8 baytları üzerinden sözlük sırası verir.
- **Kalıcılık kısıtı:** anahtar/değerde sekme veya yeni satır olmamalı.

## Doğrulama

`demo` çıktısı (0.9.9, VM):

```text
PASS  ekleme: 1000 kayit
PASS  siralama: artan
PASS  get k0500 = 1500
PASS  guncelleme sayiyi artirmiyor
PASS  silme: canli 999
PASS  olmayan anahtar null
PASS  yukseklik = 8
```

REPL kalıcılık turu: `set/del/save` → yeni süreç `load` → sıkıştırılmış
(2 kayıt) doğru geri yüklendi.

## Ölçüm (0.9.9, VM) — hata değil, taban çizgisi

```text
bench 5000   → 155 ms,  height 11,  ~13 MB RSS
bench 20000  → 884 ms,  height 13,  ~34 MB RSS
REPL stresi  → 5000 set + 2000 get, 0.23 s, ~13 MB RSS (kararlı)
```
