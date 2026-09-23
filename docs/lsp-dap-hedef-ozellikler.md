# saQut LSP ve DAP: hedef özellik listesi ve analiz

Durum tarihi: 2026-09-23 (1.0.0). Bu belge bir analizdir, kod içermez.
Amaç: saQut'un editör deneyimini "tam donanımlı" hale getirmek için LSP ve
DAP tarafında neyin desteklenmesi gerektiğini, bugün neyin olduğunu ve nerede
kırıldığını madde madde listelemek.

İşaretler: ✅ var · 🟡 var ama güvenilmez · ❌ yok · ➖ saQut'a uygun değil

---

## Ölçülmüş bulgular ve durumu (2026-09-23, `saqut 1.0.0`, gerçek sunucu çalıştırılarak)

> **Durum:** L-1..L-4 ve D-1..D-8 düzeltildi (1.0.0 dalı). LSP (24) ve DAP (10)
> golden senaryolarının tamamı geçiyor. Aşağıdaki metin ölçüm anındaki hatayı
> ve kök nedeni kayıt için korur.

Bu bölümdeki her madde `saqut lsp` / `saqut dap` gerçek süreçleriyle,
`tests/lsp/lsp_test_driver.py` ve `tests/dap/dap_test_driver.py` kullanılarak
gözlendi. Kaynak koda dokunulmadı.

### LSP

- ✅ **L-1 (düzeltildi): Tanı aralıkları bütün satırı işaretliyordu.** `int y = 1.5;` için
  CLI doğru sütunu veriyor (`column: 13`), LSP ise aralığı `0–16` gönderiyor
  (beklenen `12–13`). 1.0 öncesi commit (`0d5a0c6`) derlenip denendi: hata
  orada da var, yani 1.0'dan önce gelmiş. LSP golden'larının çoğu bu yüzden
  kırmızı. Kök neden adayı: LSP'nin `SourceLocation` → `Range` çevirisi sütunu
  ve uzunluğu kullanmıyor.
- ✅ **L-2 (düzeltildi): 24 LSP testinin tamamı ilk mesajda düşüyordu** çünkü `serverInfo.version`
  golden'da `0.9.4`, sunucuda `1.0.0`. Capability listesi değişmemiş. Sürüm
  alanı karşılaştırmadan hariç tutulmalı ya da golden güncellenmeli.
- ✅ **L-3 (golden güncellendi): Tamamlama senaryolarında ek E905/E903 tanıları** (yarım kod: `p.`,
  `bi`). 1.0'da `;` zorunlu olduğu için beklenen değişiklik; golden'lar
  güncellenmeli.
- **L-4: Rename ölçümleri** (test dosyası: global `count`, gölgeleyen yerel
  `count`, parametre `x`, alan `x`, struct `P`, `print`, FFI `sqrt`):
  - global ve yerel `count`: ✅ doğru ayrılıyor (gölgeleme doğru).
  - parametre `x`: ✅ çalışıyor (ilk ölçümde imleç `)` üzerindeydi; ölçüm hatası).
  - ✅ **struct alanı `x` (düzeltildi):** artık bildirim + tüm `x.alan` erişimleri (bu belgede) değişiyor.
  - ✅ **struct `P` (düzeltildi): eskiden yalnız bildirim değişiyordu, `P p;` kullanımı değişmiyordu**
    → rename sonrası kod derlenmez.
  - `print`: ✅ düzgün hata ("cannot rename builtin symbol").
  - ✅ **FFI `sqrt` (düzeltildi, artık açıklayıcı hatayla reddediliyor): var olmayan `file://<builtin:root.sqt>` belgesine
    düzenleme gönderiyor** ve import satırındaki `sqrt`'ü de değiştiriyor
    (`import {zz} from math` geçersiz). Editör WorkspaceEdit'i uygulayamayınca
    rename tümden başarısız olur: "rename bazen patlıyor" şikâyetinin en güçlü
    adayı. Doğrusu: FFI adlarında `prepareRename` reddi ya da import'u
    `sqrt as zz` biçimine çevirmek.
  - anahtar kelime / boşluk / dosya sonu ötesi: boş sonuç (hata yerine
    `prepareRename` ile reddedilmeli).

### DAP

Test programı: global `int g = 10;`, `add()` fonksiyonu, `double d = x as double;`,
`for` döngüsü, tek satırda iki deyim.

- ✅ **D-1 (düzeltildi): Giriş noktası yanlış satır.** `stopOnEntry` ile ilk durma `main`
  içinde **1. satır** (global başlatıcı `int g = 10;`, `main`'in başına
  enjekte ediliyor). ✅ 8.1'deki tahmin doğrulandı.
- ✅ **D-2 (düzeltildi): Satır atlama.** `next` dizisi: 1 → 7 → **9** (8 atlandı) → 10 → 11
  → 10 → 11 → 10 → 13 → 14 → 15. 8. satır (`double d = x as double;`) yalnız
  cast komutu içeriyor ve bu komut satır bilgisi taşımıyor. ✅ doğrulandı.
- ✅ **D-3 (düzeltildi, satır kaydırma eklendi): Breakpoint doğrulanmıyordu.** `setBreakpoints` sonuçları:
  satır 8 `verified:false`, satır 3 ✅, satır 12 (`}`) `false`, satır 6
  (fonksiyon başlığı) `false`, satır 13 ✅. Doğrulanmayan breakpoint en yakın
  çalıştırılabilir satıra **kaydırılmıyor**; kullanıcı gri nokta görür.
- ✅ **D-4 (düzeltildi): `stopped` olayında `hitBreakpointIds` yoktu.** Editör hangi
  breakpoint'e takıldığını vurgulayamaz.
- ✅ **D-5 (düzeltildi): `stepIn` ilk denemede ilerlemiyordu** (tek IR komutu çalıştırıyordu). 9. satırda (`add(x, 2)`) ilk
  `stepIn` yine 9'da duruyor, `add`'e ancak ikinci `stepIn` ile giriliyor.
- 🟡 **D-6 (kısmen): `stepOut` çağrı satırını atlıyordu.** D-2 ile satır bilgisi eksikliği giderildi; çağrı satırında dönüşten sonra komut kalmadıysa bir sonraki satırda durulur (gdb ile aynı). "Return value" kapsamı hâlâ yok. `add` içinden `stepOut`,
  dönüş değerinin atandığı 9. satırı göstermeden 10'a gidiyor (VS Code
  kullanıcısı dönüş değerini göremez; "return value" kapsamı da yok).
- ✅ **D-7 (düzeltildi): Program bittikten sonra olay yağmuru.** Program sonlandıktan sonra
  gelen her istek için `exited` + `terminated` olayları yeniden gönderiliyor
  (tek oturumda 11 kez). Olaylar bir kez gönderilmeli, sonraki isteklere
  hata dönülmeli.
- ✅ **D-8 (bulundu ve düzeltildi): `setBreakpoints` dosya başına gelir ama
  tüm dosyaların breakpoint'lerini siliyordu;** çok dosyalı projede diğer
  dosyadaki breakpoint'ler kayboluyordu. Ayrıca breakpoint satırın her
  komutunda tetikleniyor, çağrıdan aynı satıra dönüşte yeniden takılıyordu;
  artık yalnız satıra girişte tetikleniyor.
- ✅ `for` döngüsünde `next` başlık ve gövde arasında doğru dolaşıyor;
  tek satırdaki iki deyim (`string t = "a"; t = t + "b";`) tek adımda geçiyor.

---

## 0. Önce temel: güvenilirlik (özellik eklemeden önce)

Yeni özellikten önce mevcutların güvenilir olması gerekir. Bugün ctest'te 24
LSP senaryosu kırmızı; editör deneyimindeki "bazen patlıyor" hissinin kaynağı
bu.

- [ ] **LSP golden testlerini yeşile döndür.** İlk mesaj (`initialize`
      yanıtı) bile beklenenle eşleşmiyor: capability listesi ve `serverInfo`
      sürümü değişmiş, golden'lar güncellenmemiş. Önce gerçek sapma ile bayat
      golden ayrılmalı.
- [ ] **Sunucu asla çökmemeli.** Her istek işleyicisi bir hata sınırının
      içinde çalışmalı: istisna olursa yalnız o istek JSON-RPC hatası
      döndürmeli, süreç ayakta kalmalı. Editör açısından çöken bir dil
      sunucusu, hiç olmamasından kötüdür.
- [ ] **Yarım kod her zaman "normal durum".** Editörde kod %90 zamanda
      bozuktur (`p.` yazılmış, `)` eksik). Parser kurtarması yapılmış bir AST
      ile sembol tablosu yine kurulmalı; tamamlama ve hover bozuk dosyada da
      çalışmalı. 1.0'da parser eksik `;`'yi artık E905 ile raporluyor, bu
      yeni tanıların LSP testlerini etkilediği gözden geçirilmeli.
- [ ] **Konum kodlaması tek ve doğru.** LSP `utf-16` sütun, saQut iç konumu
      bayt/kod noktası. Türkçe karakter içeren satırlarda (ç, ğ, ş) hover ve
      rename kaymamalı. `positionEncoding` müzakere edilmeli (istemci
      `utf-8` destekliyorsa o seçilmeli).
- [ ] **Artımlı eşitleme.** `textDocumentSync: 1` (tam metin) şu an. `2`
      (artımlı) büyük dosyalarda gecikmeyi düşürür.
- [ ] **Performans bütçesi.** Her tuş vuruşunda tam yeniden derleme yerine
      bekletme (debounce) ve yalnız değişen modülün yeniden analizi.
      Hedef: tanılar < 150 ms, tamamlama < 50 ms.
- [ ] **Modül ad alanı geçişiyle uyum (1.0 değişikliği).** ModuleNamespacer
      çakışan adları token metninde `ad@N` olarak değiştiriyor. LSP; hover,
      rename, documentSymbol ve semanticTokens çıktısında bu iç adları
      göstermemeli. Rename'in çakışan adlarda doğru dosyaları değiştirdiği
      ayrıca test edilmeli.

---

## 1. LSP: yaşam döngüsü ve altyapı

| Özellik | Durum | Not |
|---|---|---|
| `initialize` / `initialized` / `shutdown` / `exit` | ✅ | golden'lar bayat |
| `$/cancelRequest` | ❌ | uzun istekler (references, rename) iptal edilebilmeli |
| `$/progress` + `window/workDoneProgress` | ❌ | büyük projede indeksleme ilerlemesi |
| `window/showMessage`, `window/logMessage` | ❌ | binary bulunamadı, sürüm uyuşmazlığı gibi durumları kullanıcıya söyle |
| `workspace/didChangeConfiguration` | ❌ | `saqut.maxCallDepth`, `saqut.jit`, biçimlendirme ayarları |
| `workspace/didChangeWatchedFiles` | ❌ | editör dışında değişen `.sqt` dosyaları (git checkout) |
| `workspace/workspaceFolders` | ❌ | çok klasörlü çalışma alanı |
| `textDocument/didOpen` / `didChange` / `didClose` | ✅ | |
| `textDocument/didSave` | ❌ | kaydetmede tam `check` |

---

## 2. LSP: dil özellikleri

### 2.1 Tanılar

- ✅ `publishDiagnostics`: bugünkü push modeli.
- ❌ **Pull modeli** (`textDocument/diagnostic`, `workspace/diagnostic`):
  modern istemciler tercih ediyor; açık olmayan dosyaların hatalarını da
  gösterir.
- ❌ **İlgili bilgi** (`relatedInformation`): E002'de "ilk tanım burada", E010'da
  döngüyü oluşturan alanlar, import çakışmasında kaynak dosya.
- ❌ **Etiketler**: `Unnecessary` (W001 kullanılmayan değişken, W003 ölü kod,
  soluk gösterilir), `Deprecated` (W006, W007, üstü çizili).
- ❌ **`codeDescription.href`**: her tanı kodundan sitedeki
  `/compiler-errors/#e003` sayfasına link.
- ❌ **Kaynak etiketi** (`source: "saqut"`) ve kodun (`E003`) ayrı alanda
  olması.

### 2.2 Gezinme

| Özellik | Durum | saQut'a özgü not |
|---|---|---|
| Tanıma git (`definition`) | ✅ | import takma adı (`as`) üzerinden kaynağa gitmeli |
| Bildirime git (`declaration`) | ❌ | FFI fonksiyonunda `src/internal/ffi.sqt` satırına git |
| Tipe git (`typeDefinition`) | ❌ | `Point p;` üzerinde `struct Point`'e |
| Uygulamaya git (`implementation`) | ➖ | arayüz/kalıtım yok |
| Referanslar (`references`) | ✅ | modüller arası, takma adlar dahil |
| Belge vurgulama (`documentHighlight`) | ✅ | okuma ve yazma ayrı tür (`Read`/`Write`) olmalı |
| Belge sembolleri (`documentSymbol`) | ✅ | hiyerarşik: struct → alanlar, enum → üyeler |
| Çalışma alanı sembolleri (`workspace/symbol`) | ❌ | Ctrl+T ile tüm projede fonksiyon/struct ara |
| Çağrı hiyerarşisi (`callHierarchy`) | ❌ | kim çağırıyor / neyi çağırıyor; özyineleme görünür |
| Tip hiyerarşisi | ➖ | kalıtım yok |
| Belge linkleri (`documentLink`) | ❌ | `import {...} from "lib.sqt"` yoluna tıklanınca dosya açılır |

### 2.3 Düzenleme yardımı

| Özellik | Durum | Not |
|---|---|---|
| Tamamlama (`completion`) | 🟡 | tetikleyiciler `.` ve `:`; aşağıdaki listeye bakın |
| Tamamlama detayı (`completionItem/resolve`) | ❌ | belgelemeyi tembel yükle |
| İmza yardımı (`signatureHelp`) | ✅ | aktif parametre vurgusu, FFI isteğe bağlı parametreleri (`seek?`, `size?`) |
| Hover | ✅ | tip + belge + (varsa) sabit değeri |
| **Snippet'ler** | ❌ | aşağıda ayrı liste |
| Satır içi ipuçları (`inlayHint`) | ❌ | aşağıda |
| Kod eylemleri (`codeAction`) | ❌ | aşağıda; en yüksek "şirinlik" getirisi burada |
| Yeniden adlandırma (`rename`) | 🟡 | `prepareRename` yok, bazen çöküyor |
| `prepareRename` | ❌ | yerleşik (`print`), FFI ve anahtar kelime üzerinde rename'i baştan reddet |
| Biçimlendirme (`formatting`, `rangeFormatting`, `onTypeFormatting`) | ❌ | `saqut fmt` önce CLI'da olmalı |
| Katlama (`foldingRange`) | ❌ | fonksiyon, struct, blok, yorum blokları, import grubu |
| Seçim genişletme (`selectionRange`) | ❌ | AST'den doğrudan türetilir |
| Bağlantılı düzenleme (`linkedEditingRange`) | ➖ | etiket yok |
| Renk (`documentColor`) | ➖ | |

### 2.4 Görsel

| Özellik | Durum | Not |
|---|---|---|
| Anlamsal renklendirme (`semanticTokens/full`) | ✅ | |
| `semanticTokens/full/delta` ve `range` | ❌ | büyük dosyada gerekli |
| Değiştiriciler (modifiers) | ❌ | `readonly` (enum üyesi), `declaration`, `defaultLibrary` (FFI, yerleşikler), `deprecated` |
| Kod mercekleri (`codeLens`) | ❌ | `main` üstünde "▶ Run  ▶ Run (JIT)  🐞 Debug"; fonksiyon üstünde "N referans" |

---

## 3. Tamamlama: saQut'ta ne önerilmeli

- Yerel değişkenler, parametreler, globaller, fonksiyonlar (kapsama göre sıralı).
- Nokta sonrası: struct alanları + UFCS yerleşik metotları (`upper`, `push`,
  `toJson`...), alıcının tipine göre süzülmüş. Nullable alıcıda (`string?`)
  metotlar önerilir ama "önce null kontrolü" uyarısıyla.
- `Color.` sonrası enum üyeleri.
- `import { | } from fs;` içinde yalnız o modülün export ettiği adlar;
  tırnaklı yolda dosya yolu tamamlama.
- `from ` sonrası gömülü modül adları (`math`, `fs`, `path`...).
- **Otomatik import:** kullanıcı `sqrt` yazınca öneride "`import { sqrt } from math;` ekle".
  Ek düzenleme (`additionalTextEdits`) ile import satırını dosyanın başına ekler.
- `case ` sonrası: switch konusunun tipi enum ise kalan (henüz yazılmamış) üyeler.
- `as ` sonrası: geçerli hedef tipler (cast matrisine göre süzülmüş).
- Tip konumunda (`int`, `Point`, `Point[]`, `T?`) yalnız tipler.
- Anahtar kelimeler bağlama duyarlı: `break`/`continue` yalnız döngüde,
  `return` yalnız fonksiyonda.

---

## 4. Snippet listesi (öneri)

Snippet'ler hem eklentide statik (`snippets.json`) hem LSP tamamlamasında
(`insertTextFormat: Snippet`) bağlama duyarlı sunulabilir. Önce statik set:

- `main` → `int main() { ... return 0; }`
- `fn` → `${1:int} ${2:name}(${3}) { $0 }`
- `if`, `ife` (if/else), `elif`
- `for` → `for (int ${1:i} = 0; $1 < ${2:n}; $1++) { $0 }`
- `fora` → dizi üzerinde `for (int i = 0; i < ${1:arr}.length(); i++) { ${2:T} x = $1[i]; $0 }`
- `while`, `wt` (`while (true) { ... if (cond) { break; } }`)
- `do` (do-while)
- `sw` (switch/case/default)
- `swe` → enum için tüm üyeleri dolduran switch (LSP tarafında, tipten üretilir)
- `try` → `try { $1 } catch (Error ${2:e}) { $0 }`
- `st` (struct), `en` (enum)
- `imp` → `import { $2 } from ${1:fs};`
- `nn` → `if (${1:x} != null) { $0 }` (nullable daraltma kalıbı)
- `readl` → `stdin::readLine` döngüsü (`while (true) { string? line = readLine(); if (line == null) { break; } ... }`)
- `rf` → `readFile` + `utf8::decode` kalıbı
- `args` → `sys::args()` ile argüman döngüsü

---

## 5. Satır içi ipuçları (inlay hints)

- Parametre adları: `randomInt(/*lo:*/1, /*hi:*/7)`, özellikle literal argümanlarda.
- Örtük tipler: saQut'ta `auto` yok, bu yüzden tip ipucu gereksiz. Yalnız
  örtük genişletmede (W004) `int → double` işareti faydalı olur.
- Enum değeri: `Color.Green` yanında `= 1`.
- `switch` sonunda eksik enum üyeleri: `// eksik: Blue`.

---

## 6. Kod eylemleri (en çok "şirinlik" buradan gelir)

Her tanının yanında tek tıkla düzeltme:

- **E001** tanımsız ad → "yakın ad: `count`" (düzeltme önerisi zaten var) ve
  FFI adıysa "`import { x } from mod;` ekle".
- **E_SYMBOL_NOT_IMPORTED** → import satırını ekle.
- **E003 nullable** → "null kontrolüyle sar" (`if (x != null) { ... }`),
  global ise "yerel kopyaya al".
- **E003 dönüşüm** → "`as T` ekle".
- **E905 eksik `;`** → `;` ekle.
- **E906 bilinmeyen kaçış** → kaçışı kaldır ya da `\\` yap.
- **E010 struct döngüsü** → alanı `T?` yap.
- **W001 kullanılmayan değişken** → sil.
- **W003 ölü kod** → sil.
- **W004 örtük genişletme** → açık `as` ekle.
- Kaynak eylemleri (hata olmadan): "importları sırala / kullanılmayanları
  kaldır", "fonksiyon çıkar" (extract function), "değişken çıkar",
  "switch'e eksik enum case'lerini ekle".

---

## 7. saQut'a özgü LSP uzantıları (cam kutu tezi)

LSP'nin standart olmayan ama izinli `workspace/executeCommand` ve özel
istekleriyle saQut'un farkı editöre taşınabilir:

- `saqut/showTokens`, `saqut/showAst`, `saqut/showIr`: imlecin bulunduğu
  fonksiyonun token/AST/IR'sini yan panelde aç (sanal belge).
- `saqut/showOptimized`: katlanan ifadeleri ve silinen ölü kodu kaynakta vurgula.
- `saqut/jitReport`: fonksiyonun JIT'te derlenip derlenmeyeceğini ve
  desteklenmeyen opcode'u söyle.
- Kod merceği: fonksiyon üstünde "IR: 42 komut".

---

## 8. DAP: temel ve bugünkü durum

Bugün desteklenen istekler: `initialize`, `launch`, `configurationDone`,
`setBreakpoints`, `threads`, `stackTrace`, `scopes`, `variables`, `evaluate`,
`continue`, `next`, `stepIn`, `stepOut`, `pause`, `terminate`, `disconnect`.
İlan edilen: `supportsConfigurationDoneRequest`, `supportsTerminateRequest`,
`supportsEvaluateForHovers`; geri kalan hepsi `false`.

### 8.1 Adımlamanın güvenilirliği ("yanlış satır, satır atlama")

Kodu okuyarak çıkardığım olası nedenler. Bunları doğrulamak gerekir:

- [ ] **IR komutlarının bir kısmı satır bilgisi taşımıyor.** `ir_generator.cpp`
      içinde 43 `Instruction ins(...)` kuruluşuna karşılık 40 `sourceLine`
      ataması var. Cast dönüşümleri (`emitCastConv`), `FLOAT32_TO_FLOAT`,
      `LOAD_SLOT` kopyaları ve `emitDefaultValue`'nun ürettikleri gibi yardımcı
      komutlar 0 ya da önceki bir konumu (`currentLoc_`) devralıyor. Adımlama
      "satır değişti mi" kontrolüne dayandığı için yanlış satırda durma ya da
      satır atlama olur.
      **Kural:** IR'deki her komut, onu üreten AST düğümünün satırını taşımalı.
      Bunu bir test sağlamalı: `sourceLine == 0` olan hiçbir komut kalmamalı
      (fonksiyon sonu dönüşü hariç).
- [ ] **Global başlatıcılar `main`'in başına enjekte ediliyor.** Satır
      bilgileri başka dosyanın satırlarını taşıyor. Debugger `main`'e girince
      önce başka bir dosyanın satırlarında duruyormuş gibi görünür. Bu
      komutlar ya "adımlanmaz" olarak işaretlenmeli ya da ayrı bir sentetik
      çerçevede çalışmalı.
- [ ] **Deyim sınırı (statement boundary) kavramı yok.** Satır bazlı durma,
      tek satırda birden çok deyim (`int a = 1; int b = 2;`) ve çok satırlı
      ifadeler için yetersiz. IR'ye "deyim başı" işareti eklenirse `next` tam
      deyim deyim ilerler; DAP'nin sütunlu breakpoint'leri de bununla
      mümkün olur.
- [ ] **Optimizasyon açıkken hata ayıklama.** Katlama ve DCE satırları
      yok ediyor. `launch` varsayılanı `--dont-optimize` olmalı; ya da
      silinen satırlara konan breakpoint'ler "doğrulanmadı" (verified:false)
      olarak işaretlenmeli ve en yakın geçerli satıra kaydırılmalı.
- [ ] **Döngü başlıkları.** `for (init; cond; update)` tek satırda üç ayrı
      noktayı içerir. `next` her turda başlığa dönmeli ve bir kez durmalı;
      bugün update ve cond ayrı adımlar gibi görünebilir.
- [ ] **JIT ile hata ayıklama yok.** Debug oturumu her zaman VM'de çalışmalı
      ve bu kullanıcıya açıkça söylenmeli.

### 8.2 DAP özellik listesi

| Özellik (capability / istek) | Durum | Not |
|---|---|---|
| Satır breakpoint'i (`setBreakpoints`) | 🟡 | `verified` ve kaydırılmış satır doğru dönmeli |
| **Koşullu breakpoint** (`supportsConditionalBreakpoints`) | ❌ | `i == 500`: `evaluate` altyapısı var, en düşük maliyetli kazanım |
| **Vuruş sayısı** (`supportsHitConditionalBreakpoints`) | ❌ | "10. geçişte dur" |
| **Log noktası** (`supportsLogPoints`) | ❌ | `x = {x}` basar, durmaz; print eklemeden iz sürme |
| Fonksiyon breakpoint'i (`supportsFunctionBreakpoints`) | ❌ | fonksiyon adı zaten IR'de var |
| **İstisna breakpoint'leri** (`setExceptionBreakpoints`) | ❌ | "yakalanmayan hatada dur" ve "her `throw`da dur"; saQut'ta `try/catch` olduğu için çok değerli |
| İstisna bilgisi (`exceptionInfo`) | ❌ | durunca `E_DIVZERO: division by zero` + trace |
| Veri breakpoint'i (`dataBreakpoints`) | ❌ | "bu global/alan değişince dur"; VM'de STORE_GLOBAL/FIELD_SET üzerinden yapılabilir |
| Sütun breakpoint'i (`breakpointLocations`) | ❌ | tek satırdaki deyimler; 8.1'deki deyim sınırı gerektirir |
| Adımlama (`next`/`stepIn`/`stepOut`) | 🟡 | 8.1 |
| `stepInTargets` | ❌ | `f(g(x))` satırında g'ye mi f'ye mi girilecek |
| `granularity` (statement / line / instruction) | ❌ | "instruction" modunda IR komut komut adım: saQut'a özgü güçlü özellik |
| Geri adım (`stepBack`, `reverseContinue`) | ❌ | VM deterministik, kayıt/oynatma ile mümkün; ileri seviye |
| `restart` / `restartFrame` | ❌ | çerçeveyi baştan çalıştır |
| `goto` / `gotoTargets` | ❌ | yürütmeyi başka satıra taşı; dikkatli |
| Değişken değiştirme (`setVariable`) | ❌ | int/float/string/bool yerelleri ve alanları |
| `setExpression` | ❌ | `p.x` gibi ifadelere yazma |
| Değerlendirme (`evaluate`) | 🟡 | hover, watch ve repl bağlamlarını ayır; yan etkisiz değerlendirme garanti edilmeli |
| Tamamlama (`completions`) | ❌ | Debug Console'da ad tamamlama |
| Değer biçimi (`supportsValueFormattingOptions`) | ❌ | int'i hex göster; byte[]'ı metin olarak göster |
| `loadedSources` | ❌ | modül grafiğindeki tüm dosyalar |
| `modules` | ➖ | tek binary |
| `disassemble` | ❌ | **IR'yi "disassembly" olarak göster**: cam kutu tezinin debugger'daki karşılığı |
| `readMemory` / `writeMemory` | ➖ | kullanıcıya açık bellek yok |
| `terminateThreads` | ➖ | tek iş parçacığı |
| `cancel` | ❌ | uzun `evaluate` iptali |
| `runInTerminal` | ❌ | programın stdin'i (readLine) entegre terminalden okuyabilsin; bugün stdin'li programlar debug edilemiyor olabilir |
| `output` olayı (stdout/stderr ayrı kategori) | ? | `print` ve `stderr::write` Debug Console'da ayrı renkte |
| `attach` | ❌ | çalışan bir `saqut run --debug-port` sürecine bağlanma; ileride |

### 8.3 Değişken görünümü (variables)

- Kapsamlar: **Locals**, **Globals** (modül modül), **Return value**
  (fonksiyondan çıkarken son dönen değer), **Exception** (catch'e girerken).
- Struct: alanlar genişletilebilir ağaç; `Node?` null ise `null` ve
  genişletilemez.
- Dizi: `int[5]` başlığı, elemanlar sayfalı (`indexedVariables`), büyük
  dizilerde ilk 100 eleman ve devamı.
- `string`: tırnaklı, uzunsa kısaltılmış (tamamı hover'da); `byte[]`
  isteğe bağlı UTF-8 önizleme.
- `date`: epoch ms yanında ISO biçimi (`2026-09-23T10:00:00Z`).
- `decimal`: tam ondalık gösterim.
- Enum: `Color.Green (1)`.
- Tip sütunu (`type`) her değişkende dolu.
- Başlatılmamış/erişilemeyen: 1.0'da tüm değişkenler tipli sıfırla başlıyor;
  henüz bildirilmemiş (kapsama girmemiş) değişkenler listelenmemeli.

---

## 9. VS Code eklentisi (LSP/DAP'nin vitrini)

- Binary çözümleme: `saqut.path` ayarı → PATH → hata mesajı ve "indir" linki.
- Sürüm uyumu: eklenti `saqut --version` okuyup uyumsuzsa uyarsın.
- Görev (task) sağlayıcı: `saqut: run`, `run --jit`, `check`, `bench`.
- Sorun eşleyici (problemMatcher): `file:line:col: error [E003]: ...` biçimi
  için hazır; LSP kapalıyken de hatalar Problems panelinde görünsün.
- Durum çubuğu: "saQut 1.0.0 · VM" ve JIT göstergesi.
- Komutlar: "Show Tokens / AST / IR" (7. madde), "Restart Language Server".
- `launch.json` şablonları: "Debug current file", "Debug with arguments"
  (`args` → `--` sonrası), "Debug with stdin file".
- TextMate grameri 1.0 ile uyumlu olmalı: `**`, `++x`, `longint`, `decimal`,
  `date`, E906 kaçışları, `export`, `as` takma adı.
- `.vsix` her release'te üretilip GitHub Release'e eklenmeli (sitede
  `saqut-1.0.0.vsix` gösteriliyor ama dosya yok).

---

## 10. Önerilen sıra

1. **Sağlamlaştırma** (0. bölüm + 8.1): LSP golden'larını yeşile döndür, hiç
   çökmeyen sunucu, IR'de her komuta satır bilgisi, adımlama testleri.
2. **Günlük hissi değiştirenler:** prepareRename + sağlam rename, snippet'ler,
   otomatik import'lu tamamlama, `codeAction` (hızlı düzeltmeler), koşullu
   breakpoint + log noktası + istisna breakpoint'i, `setVariable`.
3. **Gezinme ve görünüm:** workspace/symbol, typeDefinition, callHierarchy,
   foldingRange, selectionRange, inlayHints, codeLens (Run/Debug).
4. **saQut'a özgü:** Show IR paneli, DAP `disassemble` = IR, instruction
   granularity adımlama, JIT raporu.
5. **İleri seviye:** formatting (`saqut fmt` sonrası), veri breakpoint'i,
   geri adım (deterministik kayıt/oynatma), attach.

Her madde için kabul ölçütü: `tests/lsp` ve `tests/dap` altında en az bir
golden senaryo. Bugün DAP'ta 70% civarı olan başarı hissinin nedeni, adımlama
davranışının senaryoyla kilitlenmemiş olmasıdır.
