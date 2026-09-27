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

İlk tur #291'den başladı; #292–#298 o sırada açıldı ve kural gereği sıranın
başına girdi.

| Issue | Konu | Dosya | Durum |
|---|---|---|---|
| #298 | Yanlış / süreç anlatan yorumlar + çöp dosya | [298-yanlis-yorumlar.md](298-yanlis-yorumlar.md) | uygulandı, `117bbe1`, issue kapandı |
| #297 | IR slot tipleri üretimde kaydedilmiyor | [297-slot-tipleri.md](297-slot-tipleri.md) | uygulandı, `9653647`, issue kapandı |
| #296 | Token modeli: string tür + parser'da yeniden sınıflandırma | [296-token-modeli.md](296-token-modeli.md) | uygulandı |
| #295 | Tanı kodlarının merkezi kaydı yok | — | bekliyor (JSONL sözleşmesi kararı) |
| #294 | Derleme hattı 10 yerde elle kuruluyor | — | bekliyor |
| #293 | Dev fonksiyonlar (checkExpr, generateExpression) | — | bekliyor |
| #292 | AST yürüyücüleri, ortak çocuk gezme yok | — | bekliyor |
| #291 | CLI: komut bilgisi 5 yerde kopya | [291-cli-komut-kaydi.md](291-cli-komut-kaydi.md) | uygulandı, `46f6026`, issue kapandı |
| #290 | Built-in metot sistemi kopyaları | [290-builtin-metot-kaydi.md](290-builtin-metot-kaydi.md) | uygulandı, `84760a3`, issue kapandı |
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
