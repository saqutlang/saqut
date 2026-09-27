# LSP Golden Testleri

`src/lsp/` regresyona bu dizinle karşı korunur. Amaç `docs/prompt-lsp-dap-kurtarma.md`
Faz 0'da tanımlı: önce mevcut (doğru ya da bozuk) davranışı kilitle, sonra
düzelt. Kod düzeltmesi bu dizinde YAPILMAZ — yalnızca test altyapısı.

## Format

Her senaryo iki dosyadan oluşur:

- `NAME.jsonl` — istemciden sunucuya gönderilecek mesajlar, satır başına bir
  JSON nesnesi (JSON-RPC gövdesi; `Content-Length` zarfı sürücü tarafından
  eklenir).
- `NAME.expected.jsonl` — sunucudan beklenen TÜM çıktı mesajları (response +
  notification), üretilme sırasıyla, satır başına bir JSON nesnesi.

Mesaj sayısı istek sayısıyla birebir eşleşmez: bir `didOpen` bildirimi
cevap üretmez ama bir `publishDiagnostics` bildirimi tetikler; `initialized`
gibi bazı bildirimler hiç çıktı üretmez. Sürücü tüm istekleri sırayla yazıp
stdin'i kapatır, süreç bitene kadar stdout'ta biriken TÜM mesajları toplar
ve bunları `.expected.jsonl` ile **sırayla, JSON-yapısal** (anahtar sırası
önemsiz) karşılaştırır.

`%FIXDIR%` yer tutucusu her iki dosyada da `tests/lsp/fixtures`'ın mutlak
yoluyla değiştirilir — bu sayede URI'ler makineden bağımsız kalır.

## Bilinen-bozuk davranış senaryoları (`wip_` öneki)

Dosya adı `wip_` ile başlıyorsa CMake o testi `WILL_FAIL` ile işaretler.
Bu senaryolarda `.expected.jsonl` **bugünkü** davranışı değil, **doğru/
gelecekteki** davranışı tanımlar — yani test bilerek şu an KIRIK'tır.
İlgili faz kök nedeni düzeltince test "beklenmedik şekilde geçti" diye
görünür (CTest bunu da bir hata olarak raporlar); bu, `WILL_FAIL`
özelliğini kaldırıp dosyayı `wip_` önekinden kurtarma (ve normal senaryo
listesine taşıma) zamanı geldiğinin sinyalidir.

Şu an aktif bekleyen bilinen-bozuk senaryo yok. (Faz 1'de `wip_buffer_overlay`
kök neden #1 düzeltildiği için `07_buffer_overlay` adıyla normal senaryo
listesine taşındı.)

## Yeni senaryo ekleme

1. Gerekiyorsa `fixtures/` altına yeni bir `.sqt` dosyası ekle (yalnızca
   `wip_` senaryoları veya "diskte X, buffer'da Y" testleri için disk
   içeriği önemlidir — çoğu senaryo `didOpen` ile içeriği zaten sağlar).
2. `tests/lsp/NAME.jsonl` dosyasını yaz (istekler; `%FIXDIR%` kullan).
3. Gerçek sunucu çıktısını kaydet:

   ```sh
   python3 tests/lsp/lsp_test_driver.py \
       --binary build/saqut \
       --fixtures tests/lsp/fixtures \
       --scenario tests/lsp/NAME.jsonl \
       --record tests/lsp/NAME.expected.jsonl
   ```

4. **`--record` çıktısını gözden geçir** — bu adım "doğru" değil "gerçek"
   çıktıyı yazar; bozuk bir davranışı sessizce kilitlememek için diff'i oku.
   Bilinen-bozuk bir senaryo yazıyorsan `--record` çıktısını KULLANMA —
   `.expected.jsonl`'i doğru/gelecekteki davranışla elle yaz ve dosyayı
   `wip_` ile adlandır.
5. `cmake -B build && ninja -C build && ctest --test-dir build -R lsp` ile
   doğrula.

## Doğrulama (mevcut senaryoyu tekrar çalıştırma)

```sh
python3 tests/lsp/lsp_test_driver.py \
    --binary build/saqut \
    --fixtures tests/lsp/fixtures \
    --scenario tests/lsp/NAME.jsonl \
    --expected tests/lsp/NAME.expected.jsonl
```

## Mevcut senaryolar

| Dosya | Ne kilitliyor |
|---|---|
| `01_initialize` | `initialize` → capabilities cevabı |
| `02_didopen_valid` | Geçerli dosya `didOpen` → boş `publishDiagnostics` |
| `03_didopen_error` | E020 içeren dosya `didOpen` → konumlu diagnostic |
| `04_hover` | Bir referans konumunda `hover` → tip+isim |
| `05_definition` | Bir referans konumunda `definition` → aralık |
| `06_documentSymbol` | Fonksiyon+lokal değişken sembol listesi |
| `07_buffer_overlay` | Diskte E020 hatalı `overlay_broken.sqt`, `didOpen` buffer'ı düzeltilmiş → diagnostics buffer'a göre boş (Faz 1) |
| `08_didchange_overlay` | `didOpen` geçerli, `didChange` E020 hatası ekliyor → diagnostics güncellenip hata gelir (Faz 1) |
| `09_syntax_error_recovery` | `broken()` içinde sözdizimi hatası (`)`) → konumlu E901 diagnostic; hatanın DIŞINDAKİ `main()` fonksiyonunda hover/definition hâlâ doğru çalışır (Faz 2: panic-mode recovery) |
| `10_turkish_encoding` | Çok baytlı UTF-8 (Türkçe) karakter içeren satırlarda hover/definition sorgu konumu ve dönen aralık UTF-16↔byte dönüşümüyle doğru hesaplanır (Faz 3, kök neden #4 — `src/lsp/position.hpp`) |
| `11_scoped_definition` | İki ayrı fonksiyonda aynı adlı yerel değişken (`x`) — her fonksiyondaki referans KENDİ fonksiyonunun tanımına gider, karışmaz (Faz 3, kök neden #3 — token+offset tabanlı `findSymbolAt`) |
| `12_cross_file_definition` | `import` edilen fonksiyona giden `definition` sorgusu, sorgulanan dosyanın değil TANIMIN bulunduğu dosyanın URI'sini döndürür (Faz 3, kök neden #4 — çok-dosya URI) |
| `13_completion_scope` | İki ayrı fonksiyonda farklı lokaller (`birinci`/`ikinci`) — completion yalnızca imlecin bulunduğu fonksiyonun lokallerini önerir, başka fonksiyonun lokali önerilmez (Faz 4 — scope filtreleme) |
| `14_completion_dot_chain` | `p.adres.` zinciri — struct alanları zincir çözümüyle doğru struct'ın alanlarını gösterir (Faz 4 — token-tabanlı zincir çözümü) |
| `15_completion_nonstruct_dot` | `x.` (x int) — struct olmayan tipe `.` ile alan tamamlama boş döner (Faz 4 — hatalı zincir savunması) |
| `16_completion_scope_method` | `s::` (s string) — yalnızca string builtin metodlarını gösterir, array/struct metodlarını göstermez (Faz 4 — BuiltinMethodRegistry kategori filtrelemesi) |
