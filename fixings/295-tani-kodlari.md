# #295 — Tanı kodlarının merkezi kaydı yok; E003 21+ anlamda; konumsuz modül hataları

**Görev (tek cümle):** Her tanı ve çalışma zamanı hata kodu tek bir kayıtta
(seviye, başlık, açıklama) dursun, belge oradan üretilsin, kayıtsız ya da
ölü kod test tarafından yakalansın; E003 tek bir anlama insin.

## Ürün sahibi kararları (2026-09-27)
1. E003 **şimdi** bölünür (check JSONL `code` alanı değişir).
2. Belge tablodan üretilir + bayatlık testi (yeni CLI yüzeyi yok).
3. Çalışma zamanı `Error.code` değerleri de ayrı tabloda kayda girer.

## Doğrulanan durum (`1.0.1` @ df15a5c)
- Kaynakta 54 farklı kod literali (issue ile aynı); `diagnosticCatalog()` 26
  kodu içeriyordu. Katalogda olup hiç üretilmeyen: `E009`, `W001`. Katalogda
  olmayan ama üretilen: E000, E014–E019, W005, W008, W009, E_IMPORT_*,
  E_MODULE_*, E_SYMBOL_NOT_IMPORTED + 12 çalışma zamanı kodu.
- Katalogdaki `message` şablon alanını okuyan kod yoktu; başlık yorumundaki
  elle yazılmış kod listesi ikinci kopyaydı (ve E012+ eksikti).
- `report("E003"` 32 yerde (hepsi TypeChecker), 9 ayrı hata sınıfı.
- `E_MODULE_NOT_FOUND`/`E_MODULE_PARSE` `SourceLocation{}` ile raporlanıyordu,
  `importLoc` elde olduğu halde.

## Yapılan
- `src/diagnostic/diagnostic.hpp`: `diagnosticCatalog()` tamamlandı
  (kod, seviye, İngilizce başlık, Türkçe açıklama); ölü `message` alanı ve
  elle yazılmış başlık listesi kaldırıldı; ölü `E009`/`W001` silindi;
  `runtimeErrorCatalog()` eklendi (12 kod).
- E003 bölündü:

  | Kod | Anlam | Yer |
  |---|---|---|
  | E003 | atama/argüman/dönüş tipi uyuşmazlığı (`checkAssign`) | 5 |
  | E020 | literal hedef tipe sığmıyor / ondalık literal int bağlamında | 4 |
  | E021 | operatör bu tiplerde tanımlı değil | 6 |
  | E022 | nullable değer null denetimi olmadan kullanıldı | 4 |
  | E023 | geçersiz `as` dönüşümü | 9 |
  | E024 | switch/case tip uyumsuzluğu | 3 |
  | E025 | çağrılamayan değer çağrıldı | 1 |
  | E026 | metot alıcısı yanlış tipte | 3 |
  | E027 | `++`/`--` atanabilir konum gerektirir | 1 |

  Mesaj ve ipucu metinleri değişmedi; yalnız kod.
- ModuleLoader: `E_MODULE_NOT_FOUND` / `E_MODULE_PARSE` import satırının
  konumunu taşır (giriş dosyasının kendisi yoksa konum yine boştur).
- `tests/gen_diagnostic_docs.cpp` (CMake hedefi `gen_diagnostic_docs`):
  `docs/compiler-errors.md`'yi üretir; `--check` modu CTest
  `diagnostic_docs_fresh` içinde. Belge yeniden üretildi (eski belge yalnız
  ADR-045 eklerini listeliyordu).
- `tests/general/diagnostic_codes_test.sh` (CTest `diagnostic_codes`): kaynaktaki
  her kod literali kayıtlı, kayıttaki her kod üretiliyor. İlk koşuda
  `diagnostic_engine.hpp` örneğindeki var olmayan `W001`'i yakaladı (örnek
  düzeltildi).
- `tests/general/diagnostic_split_test.sh` (CTest `diagnostic_split`): E003 ve
  E020–E027 için en küçük negatif örnek (kod + satır, E003 sızmıyor) +
  `E_MODULE_NOT_FOUND` konumu. Eski ikiliye karşı ilk E020 senaryosunda düşüyor.
- Beklentiler: 6 golden `.compile_error` (E022×2, E020×3, E021), 2 LSP JSONL
  (E020), fixture/README yorumları, kod içi yorumlar (ir_generator,
  mir_backend, type_checker).
- `knowledge-base/05_Runtime.md` §26.

## Kanıt (Debug build)
- `cmake --build build` uyarısız; `ctest -j8` 351/353 (düşen `lsp_28`/`lsp_29`
  df15a5c'den beri, ilgisiz).
- 52 kod kayıtlı, tümü kaynakta üretiliyor.

## Kullanıcıya görünen değişiklik
- `check` JSONL / LSP / terminal tanılarında `code`: yukarıdaki 28 hata
  E003 yerine E020–E027 ile gelir. Kodla filtreleyen betik/araçlar etkilenir.
- `E_MODULE_NOT_FOUND`/`E_MODULE_PARSE` artık import satırına bağlı (LSP
  hatayı satırda gösterebilir).

## Yapılmayan / kanıtlanmayanlar
- Çağrı yerleri string kod kullanmaya devam ediyor (190+ yer); yazım hatası
  derlemede değil CTest'te yakalanır.
- "Her kodun negatif fixture'ı" issue önerisi yalnız yeni kodlar için
  yapıldı; E001–E019, W00x, E_IMPORT_* için tam kapsam denetlenmedi.
- Çalışma zamanı kodlarının hepsi VM ve JIT'te aynı durumda üretiliyor mu
  denetlenmedi (yalnız kayıtlı olmaları).
- Tarihsel belgeler (ADR-040, `docs/opcode-ir.md`,
  `docs/lsp-dap-hedef-ozellikler.md`) eski E003 anlamını anıyor; ADR geçmişi
  yeniden yazılmadı. `saqutwebside/` taranmadı.
- Ana ağaçtaki kirli `symbol_collector.cpp` değişikliğin (E_IMPORT_UNKNOWN
  ipucu) bu dala dahil değil; birleşirken çakışmaz ama birlikte incelenmeli.
