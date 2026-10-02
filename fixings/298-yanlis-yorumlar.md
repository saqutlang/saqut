# #298 — Frontend'de yanlış bilgi veren ve süreç anlatan yorumlar

**Görev (tek cümle):** Kodun şu an yaptığını yanlış anlatan yorumlar
düzeltilsin, geliştirme sürecini ("Faz 2", "Dilim 1.5", "PLAN B") anlatan
notlar mimariyi/kısıtı anlatan metne dönsün, çöp dosya silinsin. Davranış
değişikliği yok.

## Issue tablosundaki yanlış yorumlar

| Yer | Durum |
|---|---|
| `cli/args.hpp` `--jit` "VM'e düşer" | #291'de düzeltildi (`46f6026`) |
| `tokenizer.cpp` "O(1) lookup yerine O(n) for döngüsü" | düzeltildi (anlam ters) |
| `tokenizer.cpp` "readIdentifier — değişmedi" | fonksiyonun yaptığıyla değiştirildi (ASCII harf/rakam/`_`/`$`; tanınmayan karakterde boş ad) |
| `parser/token.hpp` `keywords[]` ile eşleşmeli, "~60 girdi", "constexpr" | düzeltildi: gerçek eşleşme `KW_MAP` ↔ `KEYWORD_MAP`, `keywords[]` ölü (#287'de tabloların kendisi birleşecek). `constexpr` iddiası da yanlıştı (`inline const`) |
| `main.cpp` KULLANIM / YENİ KOMUT adımları | #291'de düzeltildi |
| `cli/cli.hpp` "cli.hpp tarafından include edilir" | #291'de düzeltildi |
| `ir_generator.cpp` "FIELD_GET/ARRAY_GET/LOAD_GLOBAL … Dilim 1.5 JIT'i reddediyor" | kaldırıldı: bu opcode'lar hemen üstteki `valueType` dalında zaten işleniyor, JIT destekliyor (`mir_backend.cpp` ARRAY_GET/FIELD_GET/LOAD_GLOBAL dalları). Yerine `default`'un gerçek kuralı yazıldı |

## Ek bulunan yanlış yorumlar

- `parser_base.hpp`: "ast.hpp, symbols.hpp, exec.hpp diag vermeden Parser kurar"
  — üçü de diag veriyor; diag'siz kuran tek yer `bench.hpp`.
- `ast_node.hpp`: `resolvedType`/`isConstant` üzerinde "TODO(faz-3/4)" — ikisi
  de çoktan dolduruluyor (TypeChecker, ConstantFoldingPass); `foldedValue`
  TODO'su hiç uygulanmamış bir alandan söz ediyordu, kaldırıldı.
  `isReachable` TODO'su → DeadCodeElimPass'in gerçek kullanımı.
- `identifier.hpp`: "Symbol tanımlandığında bağlanacak" TODO'ları → SymbolCollector
  bağlıyor.
- `symbol_collector.cpp`: "member çözümü Faz 3 → TODO" → üye adı TypeChecker'da
  çözülür (nesnenin tipi gerekir).
- `cli/commands/run.hpp`: "Dilim 1'in desteklediği opcode kümesi" → JIT'in
  desteklediği küme.

## Süreç notları

Kapsam (issue ile aynı): `src/parser src/semantic src/symbol
src/ir/ir_generator.cpp src/opt src/tokenizer src/lexer` + `src/cli`. 52 satırdan:
- 11 dosya başlığında `KATMAN: Faz N — …` → hattaki aşama adı (Sembol toplama /
  Anlam denetimi / AST optimizasyonu / Parser).
- `Faz 2/5`, `Dilim 1.5`, `PLAN B`, `(Faz 3-c)` gibi etiketler kaldırıldı;
  ADR atıfları (`ADR-040`, `ADR-045`) korundu.
- `lexer.cpp` `Adım 1/2/3` algoritma adımıdır (sayı okuma), süreç değil;
  dokunulmadı.
- Tarama sonrası kapsamda `Faz N`/`Dilim N`/`PLAN B`/`TODO(faz-N)` kalmadı.

## Çöp dosya

`src/parser/test.txt` (içerik `test`, `15db98f` ile eklenmiş, hiçbir kod/test
okumuyor) silindi.

## Kanıt

- `cmake --build build` uyarısız; `ctest -j8` 347/349 (düşen `lsp_28`/`lsp_29`
  df15a5c'den beri, ilgisiz).

## Yapılmayan / kanıtlanmayan

- Issue öneri 2 ("frontend başlık yorumlarının §10.1'e göre gözden
  geçirilmesi") yalnız süreç etiketleri ve yanlış bilgi düzeyinde yapıldı;
  her başlığın "neyi garanti eder, neye dokunulursa ne etkilenir" diye yeniden
  yazılması yapılmadı. Bu, #292/#293 bölmeleri sırasında dosya dosya ele
  alınması daha az çakışmalı.
- `src/lexer` dışında `src/vm`, `src/mir`, `src/lsp` taranmadı (issue kapsamı
  frontend).
- Issue numarası atıfları (`#227`, `#239`) karar geçmişi olarak bırakıldı.
