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

## Bölüm 1 — dayanıklı tamamlama motoru

- **Kapsam aralıkları token dizisinden** (`ScopeIndex`, lsp_analysis.cpp) |
  SymbolCollector'ın Scope nesneleri kaynak aralığı taşımıyor; tokenizer
  sözdizimi hatasında da tam token dizisi verdiği için `{ }`/`( )`
  eşleşmeleri yarım kodda da kullanılabilir. Parametre, for-init ve catch
  değişkeninin kapsamı, tanımı içeren `( )`'den hemen sonra gelen `{ }`
  gövdesidir | SymbolCollector'a aralık kaydı eklemek (ön ucu değiştirir;
  bozuk kodda aralık yine eksik kalır).
- **Ad çözümü:** en içteki kapsamdaki, imleçten önce tanımlanmış yerel;
  yoksa global/import/yerleşik. Başka dosyanın sembolü tamamlamada yalnız
  bu dosyaya import edildiyse görünür (eskiden grafikteki her modülün
  export edilmemiş üst düzey adları da öneriliyordu).
- **Alıcı ifadesi geriye doğru yürünür:** tanımlayıcı, `a.b.c`, `a[i]`,
  `f()`, `x.m()` (yerleşik/Pool/List/Thread metot dönüş tipleri), string
  literal; enum adı → üyeler.
- **Satır sonu kuralı:** satır sonunda kalan `x.` ile alt satırın başındaki
  ad birleştirilmez (`yap().⏎ ns[0].` iki ayrı deyimdir; parser ise tek
  zincir okur). Satır başında `.bar()` biçimli zincir etkilenmez.
- **Son geçerli analiz** | her turda sözdizimi temiz (kendi dosyasında E9xx
  yok) analiz `lastGood` snapshot'ına devredilir (kopya değil, sahiplik
  taşınır). Kök ad güncel analizde çözülemezse alıcı token'ları güncel
  metinden, kök adın kapsamı ve tipler snapshot'tan çözülür. İmleç eski
  metne **satır düzeyinde** eşlenir (ortak baş/son satırlar; aradaki blok
  eşit satırlıysa satır satır) — birden çok yerde düzenleme olağan olduğu
  için bayt düzeyinde tek değişim bölgesi varsaymak yetmedi (ilk
  denemede imleç dosya başına eşleniyordu) | ağır: hata toleranslı ayrı
  bir parser. **PLAN B (kısmen):** parser değiştirilmedi; mevcut
  panic-mode kurtarma çoğu yarım ifadede zaten tam tablo veriyor (ölçüldü:
  `t.`, `if (t.`, `[t.`, açık `(`); tablo gerçekten kaybolduğunda (ör.
  bozuk fonksiyon başlığı `int ma in()`) snapshot devreye girer.
- **Bağlamlar:** deyim başı (anahtar kelimeler + snippet'ler + semboller),
  ifade (semboller + `true/false/null/thread/Pool/List`), `lock`/`unlock`
  hedefi (yalnız shared primitif globaller), `Pool(`/`List(`/`as` (değer
  tipleri; koleksiyon/Thread hariç), `shared` (tüm tipler), `x::`, import.
- **Sıralama (`sortText`):** `0_` yerel, `1_` aynı dosya globali, `2_`
  import edilmiş, `3_` yerleşik/anahtar kelime/snippet, `4_` import
  edilmemiş proje sembolü (Bölüm 3). Sunucu önekle zaten filtreler.
- **Pool/List/Thread öğeleri** gerçek eleman tipiyle (`void push(int value)`)
  ve `documentation` açıklamasıyla döner.
- Golden güncellemeleri: `13` (yalnız `sortText` eklendi), `25` (T yerine
  eleman tipi + açıklama). Yeni: `26` (Bölüm 0 teşhisi, artık yeşil), `27`
  (bağlamlar + son geçerli analiz).

## Test altyapısı

- **`{"$any": true}` joker satırı (lsp_test_driver.py)** | bu turda
  capability listesi her özellikte büyüyor; tüm golden'lar ilk satırda
  `initialize` yanıtının tamamını karşılaştırdığı için her özellik 25
  dosyayı kırıyordu. Capability'ler yalnız `01_initialize`'da doğrulanır,
  diğerleri joker kullanır | her özellikte 25 dosyayı yeniden kaydetmek
  (gerçek regresyonları gürültüde gizler).
