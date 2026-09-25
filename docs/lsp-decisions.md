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

## Bölüm 2 — import'a duyarlı analiz

- Modül yükleyici zaten import edilen dosyaları açık belge overlay'i ya da
  diskten okuyordu; eklenen: **bağımlılık grafiği**. Her analiz, modül
  grafiğindeki dosyaları `DocumentState::deps`'e yazar. Bir belge
  değişince/kapanınca ve `didChangeWatchedFiles` gelince, grafiğinde o
  dosya olan açık belgeler yeniden analiz edilir (geçişli bağımlılıklar
  dahil, çünkü grafik geçişlidir).
- **Tanılar yalnız değiştiyse yeniden yayımlanır** (`lastPublished_`) |
  her tuşta tüm bağımlı belgelere bildirim göndermek gürültü | her seferinde
  yayımlamak.
- **Bayat tanı temizliği:** bir bağımlılığın tanıları kalktığında o
  dosyaya boş liste gönderilir (eskiden bir kez yayımlanan hata, dosya
  grafikte hatasız kalınca editörde kalıyordu).
- Modül döngüsü: yükleyicinin E_MODULE_CYCLE tespiti aynen gösterilir;
  LSP çökmez (golden 28).
- **Tanım/referans aralığı tanımlayıcıya** kaydırıldı | `definitionLoc`
  bildirim başını gösteriyordu (`int topla`'da `int`); tanıma gitme ve
  "tanımı dahil et" referansı tip kelimesini seçiyordu. Golden 05, 09, 10,
  11, 12 bu düzeltmeyle güncellendi (yalnız aralık başı/sonu).

## Bölüm 3 — proje indeksi ve otomatik import

- **İş parçacığı modeli (PLAN B):** ayrı indeksleme thread'i yerine
  G/Ç okuyucu thread + tek analiz thread'i; indeksleme analiz thread'inde,
  istek kuyruğu boşken **dosya dosya** ilerler | ön ucun tekilleri
  (`FileRegistry::intern/path`, kaynak konumlarının yol tablosu) kilitsiz;
  paralel parse veri yarışı olurdu. Kilit eklemek derleyicinin sıcak
  yollarını (her `filePath()`) etkiler | FileRegistry'yi kilitlemek. Sonuç:
  istek en fazla tek dosyanın indeksleme süresi kadar bekler (ölçüm:
  Bölüm 6).
- **Kayıt başına:** modül yükleyici + sembol toplayıcı (type check yok).
  Tek dosya yerine grafik yüklenir ki import edilen sembollere yapılan
  referanslar doğru hedefe bağlansın (dosyalar arası sayım/rename için
  şart) | yalnız parse + ad eşleştirme (gölgelenmede yanlış sayar).
- **Kimlik:** üst düzey sembol = (tanım dosyası, görünen ad) | offset
  düzenlemeyle kayar; üst düzey ad dosya içinde tektir (E002).
- **Önbellek:** içerik hash'i aynıysa yeniden indekslenmez; açık belgenin
  kaydı her analizde onun sonucundan yenilenir.
- **Kapsam:** kökler `workspaceFolders`/`rootUri`; `build*`, `node_modules`,
  `.git` ve `initializationOptions.index.exclude` (glob `*`, `**`, `?`)
  hariç. `index.synchronous: true` (testler) indekslemeyi initialize
  içinde bitirir. İlk tarama bitince istemci destekliyorsa
  `workspace/codeLens/refresh` istenir.
- **Otomatik import:** önekle eşleşen, bu dosyada görünmeyen export'lar
  `sortText 4_` ile; `labelDetails.description` = göreli yol,
  `additionalTextEdits` = aynı dosyadan zaten import varsa `}` önüne
  `, ad`; yoksa son import satırının altına `import {ad} from "göreli";`.
  Göreli yol import eden dosyanın dizinine göre (yükleyicinin çözümüyle
  aynı). `import {|} from "dosya.sqt"` o dosyanın export'larını önerir.
- **References/rename:** belgenin kendi analizine ek olarak indeksteki
  (açık olmayan) dosyalardaki kullanımlar. Rename güvenliği: konumdaki metin
  eski adla birebir eşleşmeli (`import {x as y}` ile gelen `y` kullanımları
  değişmez). workspace/symbol: alt dizi eşleşmesi, en çok 1000 sonuç.
- **İletişim günlüğü** yalnız `SAQUT_LSP_LOG=<dosya>` ile | eskiden her
  mesaj `/tmp/saqut-lsp.log`'a girintili yazılıyordu (gecikme + kaynak kodun
  izinsiz diske yazılması).
- Golden 28 (çok dosyalı): tanım, açık olmayan dosya dahil references,
  workspace/symbol, iki otomatik import biçimi, dosya import'undan ad
  tamamlama, rename, döngü, bağımlı yeniden analiz + bayat tanı temizliği.

## Bölüm 4: editör özellikleri

- **Gereksiz kod tespiti LSP'de (lsp_editor.cpp)** | derleyicinin W003'ü
  optimizer'dan gelir; LSP IR/optimizer'a dokunmaz. AST + sembol tablosu
  üzerinde kullanılmayan değişken/import, çağrılmayan export'suz fonksiyon
  (main hariç) ve return sonrası kod bulunur | W003'ü LSP'ye taşımak
  (ön uç sınırını ihlal eder).
- **Seviye Hint (4) + tag Unnecessary (1)** | editör soluk gösterir, hata
  listesini kirletmez | Warning (gürültü; derleyici çıktısıyla çelişir).
- **Import kullanımı token taramasıyla** | tip konumundaki kullanım
  (`Nokta p;`) AST'de ref olarak görünmüyordu → yanlış "kullanılmıyor" |
  yalnız sembol ref'leri.
- **Hiyerarşik documentSymbol** | fonksiyon altında yereller, struct
  alanları, enum üyeleri; aralık `export`/`shared` dahil | düz liste (eski).
- **Semantic legend genişletildi** (struct, enum, enumMember;
  declaration/global/shared) | shared global ayırt edilebilir olmalı;
  Pool/List/Thread `type` | ayrı `sharedVariable` tipi (standart dışı).
- **codeLens yalnız "N referans"**, komut `saqut.showReferences`, argümanlar
  [uri, pos, locations]; sayım proje indeksinden (açık olmayan dosyalar
  dahil) | resolve ile tembel hesap (her kaydırmada gecikme, ek tur).
- **inlayHints yalnız literal argümanlarda**, `saqut.inlayHints.parameterNames`
  ile kapatılabilir | her argümanda (gürültü).
- **codeAction**: import kaldır, yan etkisiz başlatıcılı değişken kaldır,
  E001 → proje indeksinden "Import ekle" | çağrı başlatıcılı değişkeni de
  kaldırmak (yan etki kaybı).
- Golden 01, 06, 09–11, 13, 19, 21, 23–25 yeniden kaydedildi: yalnız ipucu
  tanıları eklendi, documentSymbol hiyerarşik biçime geçti, legend değişti.
  Golden 29 tüm Bölüm 4 özelliklerini kapsar.

## Test altyapısı

- **`{"$any": true}` joker satırı (lsp_test_driver.py)** | bu turda
  capability listesi her özellikte büyüyor; tüm golden'lar ilk satırda
  `initialize` yanıtının tamamını karşılaştırdığı için her özellik 25
  dosyayı kırıyordu. Capability'ler yalnız `01_initialize`'da doğrulanır,
  diğerleri joker kullanır | her özellikte 25 dosyayı yeniden kaydetmek
  (gerçek regresyonları gürültüde gizler).
