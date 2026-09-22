# xml-tool — saQut ile XML ayrıştırıcı + CLI

Elle yazılmış, bağımlılıksız XML 1.0 alt kümesi ayrıştırıcısı ve küçük bir
terminal aracı. Amaç hem kullanışlı bir araç hem de saQut'un metin/dizi/struct/
enum/özyineleme/hata-yönetimi/dosya-IO yüzeyini gerçek bir programda zorlayan
bir test aracı olmasıdır.

## Kullanım

```bash
./build/saqut run apps/xml-tool/main.sqt -- check  <dosya>
./build/saqut run apps/xml-tool/main.sqt -- pretty <dosya>
./build/saqut run apps/xml-tool/main.sqt -- query  <dosya> <yol>
./build/saqut run apps/xml-tool/main.sqt -- stats  <dosya>
```

Çıkış kodları: `0` başarılı, `64` kullanım hatası, `65` iyi-biçimlilik hatası,
`66` dosya okunamadı.

`query` yol sözdizimi:

```text
katalog/kitap/ad        eşleşen öğelerin metin içeriği
katalog/*/yil           '*' herhangi bir öğe adıyla eşleşir
katalog/kitap/@id       eşleşen öğelerin 'id' özniteliği
```

## Modüller

- `xml_types.sqt` — veri modeli (NodeKind, XmlAttr, XmlNode, XmlError).
- `xml_parser.sqt` — ayrıştırıcı; `Parser` durumu referans semantiğiyle taşınır.
- `xml_ops.sqt` — pretty-print, yol sorgusu, istatistik.
- `main.sqt` — CLI dağıtıcı.

## Kapsam ve sınırlar (bilinçli)

- Öğe, öznitelik, metin, yorum, CDATA, işleme talimatı; DOCTYPE atlanır.
- Varlık çözümü: `&amp; &lt; &gt; &quot; &apos; &#NNN; &#xHH;`.
- İyi-biçimlilik: kök tekilliği, etiket eşleşmesi, tırnak kapanışı, yorumda
  `--` yasağı, metinde/öznitelikte ham `<` yasağı, XML bildiriminin yeri.
- **İlk hatada durur** (fail-fast): kaskad hata gürültüsü yerine tek, konumlu
  hata. Çok hatalı raporlama bilinçli olarak kapsam dışıdır.
- DTD iç modeli, ad-alanı çözümü, şema doğrulaması yoktur.
- Pretty-print biçim normalizasyonu yapar (yalnız-boşluk metin düğümleri
  atılır, girinti 2 boşluk); **DOMParser değildir**, kaynak byte-korunumlu
  yeniden üretim garanti edilmez.

## Performans uyarısı (dil kaynaklı — bkz. `../BULGULAR.md` B-02)

Ayrıştırıcı `string.charAt` / `string.length` üzerinden çalışır. Bu iki metot
saQut'ta **kod-noktası indeksleme** yaptığı için çağrı başına O(n)'dir; bu da
karakter-bazlı ayrıştırmayı O(n²)'ye çevirir.

Ölçüm (0.9.9, VM):

- 35 KB (`testdata/derin.xml`, 5000 derinlik): ~8.9 s
- 1.3 MB (`testdata/genis.xml`): 20 sn'de tamamlanmaz
- Aynı 1.3 MB dosyada `byte[]` indeksli tarama: **0.09 s**

Yani bu araç küçük belgeler için doğru ve yeterlidir; MB ölçeğinde dilin metin
API'si nedeniyle kullanılamaz. Hızlı sürüm, ayrıştırıcının `byte[]` üzerinden
(ASCII sınırlayıcılar bayt düzeyinde) çalışmasını ve metin/öznitelik değerlerini
`byte[] → utf8::decode` ile parça parça çözmesini gerektirir.

## Test verisi

`testdata/` içinde bir iyi-biçimli belge ve yedi negatif durum vardır
(tırnaksız öznitelik, uyuşmayan etiket, bozuk varlık, ikinci kök, geç XML
bildirimi, boş belge, yorumda `--`).
