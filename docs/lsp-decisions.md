# LSP / VS Code eklentisi karar günlüğü — `1.0.1`

Biçim: **karar | gerekçe | alternatif**. Plan B'ler "PLAN B" etiketlidir.
Sınır: LSP yalnız derleyicinin ön ucunu (lexer, parser, sembol toplayıcı,
type checker, modül yükleyici) kullanır; IR/optimizer/JIT'e dokunmaz.

## Bölüm 0 — teşhis: `t.` tamamlamada Thread metotları görünmüyor

Olası nedenler tek tek sınandı (`saqut lsp`'ye doğrudan istemciyle):

| Senaryo | Sonuç (tur başı, `84495b8`) |
|---|---|
| `Thread t = thread {...}; t.` (tek fonksiyon) | join/stop/running — doğru |
| Aynı `t.` + başka fonksiyonda `string t` | **string metotları** — yanlış |
| `int t` main'de, `Thread t` başka fonksiyonda, orada `t.` | **boş** |
| `t.jo` (nokta sonrası kısmi ad) | **boş** |
| `ts[0].` (dizi elemanı alıcı) | **boş** |
| `if { t. }` iç blok | doğru |

- **Parse edilemezlik ana neden değil:** parser panic-mode kurtarmayla
  `t.` satırında da AST ve sembol tablosu üretiyor (E903/E905 tanısı +
  tam tablo). Tamamlama çalışıyor.
- **Asıl neden 1 — kapsamsız ad çözümü:** `resolveChainType` alıcı adını
  `allSymbols()` içinde *ilk* eşleşmeyle çözüyordu; aynı ad başka
  fonksiyonda da tanımlıysa yanlış tip (ya da hiç) geliyordu. Gerçek
  kodda `t`, `s`, `i` gibi kısa adlar sık tekrarlandığından kullanıcı
  deneyiminde "bazen hiç çıkmıyor" olarak görünür.
- **Asıl neden 2 — nokta sonrası önek:** `t.jo|` bağlamı `Dot` değil
  `Normal` sayılıyordu (en yakın token tanımlayıcı) → üye listesi yerine
  globaller/anahtar kelimeler, onlar da önekle elenince boş liste.
  VS Code elle tetiklemede (Ctrl+Space) ya da listeyi yeniden isterken
  bu bağlamı kullanır.
- **Olası neden 3 — eski ikili:** eklentinin sürüm kontrolü yalnız
  `major >= 1`'e bakıyor; derleyici hâlâ `1.0.0` bildirdiği için Thread
  tamamlaması olmayan eski bir 1.0.0 ikilisi de kontrolden geçer.
  **Karar:** sunucu `initialize` yanıtında
  `capabilities.experimental.saqutLspLevel` (tamsayı özellik düzeyi)
  bildirir; eklenti gerekli düzeyi bilir, eksik/düşükse kullanılan
  ikilinin yolunu ve düzeyini gösteren bir uyarı verir. | sürüm dizesi
  derleyici sürümüdür, LSP yeteneğini söylemez (1.0.0 ikilileri arasında
  fark vardı) | `--version` karşılaştırması (yetersiz, bkz. yukarı).
- Teşhis testi: `tests/lsp/26_completion_member_scoped` (1. ve 4.
  satırdaki durumlar); tur başında kırmızı, Bölüm 1 ile yeşil.

## Test altyapısı

- **`{"$any": true}` joker satırı (lsp_test_driver.py)** | bu turda
  capability listesi her özellikte büyüyor; tüm golden'lar ilk satırda
  `initialize` yanıtının tamamını karşılaştırdığı için her özellik 25
  dosyayı kırıyordu. Capability'ler yalnız `01_initialize`'da doğrulanır,
  diğerleri joker kullanır | her özellikte 25 dosyayı yeniden kaydetmek
  (gerçek regresyonları gürültüde gizler).
