# fixings — derleyiciyi genişletilebilir hale getirme işleri

**Amaç:** Yeni özellik eklemek değil. Orta düzey bir C++ geliştiricisinin yeni
bir anahtar kelime, veri tipi, FFI fonksiyonu, CLI komutu veya JIT kancası
ekleyebilmesi için elle senkron tutulan kopyaları tek kaynağa çekmek, ölü kodu
ve gereksiz kısıtları kaldırmak.

**Ölçüt:** `docs/learn/0x-*.md` rehberlerindeki "şu N dosyaya dokun" adımları
azalır; unutulan bir adım sessiz yanlış davranış değil derleme hatası ya da
açık test hatası üretir.

**Taban:** `1.0.1` @ `df15a5c`. Başlangıçtaki kirli ağaç
(`src/symbol/symbol_collector.cpp`, `symbol_collector_test.txt` silinmesi,
`cmake/openssl.cmake`, `vendor/`, `docs/learn/`) kullanıcıya aittir; bu işler
ona dokunmaz.

## Sıra (en yeniden en eskiye)

| Issue | Konu | Dosya | Durum |
|---|---|---|---|
| #291 | CLI: komut bilgisi 5 yerde kopya | [291-cli-komut-kaydi.md](291-cli-komut-kaydi.md) | uygulandı, commit bekliyor |
| #290 | Built-in metot sistemi kopyaları | — | bekliyor |
| #289 | `char` tipi yarım | — | bekliyor (dil kararı) |
| #288 | FFI tablo ↔ ffi.sqt tutarlılık denetimi | — | bekliyor |
| #287 | Keyword tabloları + tip adı string eşlemeleri | — | bekliyor |
| #286 | FFI tek kaynak (üretici + autoThunk) | — | bekliyor (büyük tasarım) |

## Her issue için akış

1. Kaynağı oku, issue iddialarını güncel kodda doğrula (satır numaraları
   `1.0.1-webserver`'dan alınmış olabilir).
2. Ürün kararı gereken noktaları ayır, sor.
3. En küçük diff ile tek kaynağa çek; davranışı koruyan testleri çalıştır.
4. İlgili `docs/learn/` rehberini ve dosya başlık yorumlarını güncelle.
5. Sonucu bu klasördeki dosyaya ve issue'ya yorum olarak yaz (DoD durumu,
   kanıtlanmayanlar).
