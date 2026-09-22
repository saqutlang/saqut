# apps/ — 1.0 öncesi gerçek uygulama testleri

Bu dizin, saQut 1.0 öncesi sistem testinin **aracıdır**: dili gerçek terminal
uygulamaları yazarak zorlamak ve karşılaşılan hataları/doküman sapmalarını
kaydetmek.

- Uygulamalar burada geliştirilir ve koşulur.
- Bulgular `BULGULAR.md` dosyasında toplanır (komut + gözlem + sınıf + ilgili
  issue).
- Kanıtlanmış, kalıcı değeri olan uygulamalar daha sonra `examples/` altına
  taşınabilir (examples = kanıtlanmış kaynak kodu birikimi).

## Uygulamalar

| Dizin | Ne | Durum |
|---|---|---|
| `xml-tool/` | XML 1.0 alt kümesi ayrıştırıcı + check/pretty/query/stats CLI | çalışıyor (VM); büyük dosyada dil kaynaklı yavaşlık için BULGULAR B-02 |
| `sha256-tool/` | SHA-256 CLI — bitwise/byte[]/tamsayı sarması; NIST + `sha256sum` ile doğrulandı | çalışıyor (VM) |
| `btree-kv/` | (planlandı) B-tree kalıcı anahtar-değer deposu + REPL | — |

## Koşum

```bash
./build/saqut run apps/xml-tool/main.sqt -- <komut> <dosya>
```

Backend notu: normatif backend VM'dir (`run`). `--jit` yalnız deneysel
karşılaştırma için kullanılır; VM ≢ JIT çıktı farkı bulgu olarak kaydedilir.
