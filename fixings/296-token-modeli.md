# #296 — Token modeli: string tür, parser'da yeniden sınıflandırma, iki token.hpp

**Görev (tek cümle):** Token'ın türü tokenizer'da bir kez belirlensin (enum),
parser ve araçlar yeniden sınıflandırmasın; keyword listesi tek tablo olsun;
sözcüksel hatalar tokenizer'ın kendi tanı kanalından gelsin.

## Ürün sahibi kararları (2026-09-26)
1. Tokenizer tanı kanalı bu turda.
2. Başlıklar üçe bölünür: `tokenizer/token.hpp` (token nesneleri),
   `tokenizer/token_kind.hpp` (TokenType + tek keyword/operatör tablosu),
   `parser/parser_token.hpp` (ParserToken + öncelik tablosu).

## Doğrulanan durum (`1.0.1` @ df15a5c)
- `Token::type` string; `parseToken` string karşılaştırması + kontrolsüz
  `find()->second` (tabloda olmayan kelimede UB). 23 `gettype()` kullanımı
  (LSP, bench, `tokens`, parser).
- Keyword'ler iki tabloda (`tokenizer.cpp KW_MAP`, `parser/token.hpp
  KEYWORD_MAP`), `KEYWORD_MAP`'te ölü `date` girdisi; `tokenizer.hpp`'de üç
  ölü tablo (`operators[]`, `delimiters[]`, `keywords[]`).
- E906/E907 token alanlarında taşınıp parser'da raporlanıyordu.
- **Ek bulunan hata:** `scan()` dosya sonunu `token->token == "EOL"` metniyle
  tanıyordu → `int EOL = 5;` içeren program o noktada sessizce kesiliyordu
  (baseline'da E904 + E905). Sentinel token da hiç silinmiyordu.
- Başlıktaki "TokenType parser'da çünkü ADR-002" atfı yanlıştı: ADR-002
  (`docs/fikirler.md`) yalnız Pratt parser seçimini kaydeder.

## Yapılan
- `git mv src/parser/token.hpp src/tokenizer/token_kind.hpp`; öncelik tablosu
  ve `ParserToken` yeni `src/parser/parser_token.hpp`'ye. Ölü
  `OPERATOR_MAP_STRREV` silindi. `KEYWORD_MAP` tek keyword listesi; `date`
  girdisi silindi (bugünkü davranış: `date` identifier).
- `Token`: `category` (TokenCategory enum) + `kind` (TokenType); `gettype()`
  kalktı; `TokenList` buraya taşındı. `tokenCategoryName()` `saqut tokens`
  etiketini üretir.
- Tokenizer: `KW_MAP` ve üç ölü tablo silindi; `scan()` her token'ın `kind`'ını
  tablodan yazar (tabloda olmayan operatör → SVR_VOID, UB yok); dosya sonu
  `TokenCategory::End` ile tanınır ve sentinel silinir.
- Tanı kanalı: `Tokenizer(DiagnosticEngine*)`; E906/E907 tokenizer'da;
  `StringToken::badEscapes/unterminated` ve parser'daki raporlama bloğu kalktı.
  ModuleLoader, `symbols`, `exec` (iki yer), FFI kataloğu diag geçer.
- `parseToken` → `pt.type = token->kind`.
- Adlar: `Tokenizer::hmx` → `lexer` (private), `Lexer::include` →
  `tryConsume(word, consume)`.
- 23 `gettype()` kullanımı enum karşılaştırmasına çevrildi.
- Belgeler: `knowledge-base/03_Frontend.md` §11, `06_Tooling.md` kanıt listesi,
  `docs/learn/01-yeni-anahtar-kelime-ekleme.md` (ana ağaçta): yeni keyword =
  `KEYWORD_MAP`'e **tek satır** (eskiden iki tablo + "birlikte yap, yoksa
  çöker" uyarısı).

## Kanıt (Debug build)
- Değişiklik öncesi ikiliyle `tests/` + `examples/` altındaki 321 `.sqt`
  için `saqut tokens` (1,1M satır) ve `saqut check` çıktıları: **birebir aynı**
  (`cmp`/`diff` boş). E906/E907/tanınmayan karakter örneklerinin `run`/`check`
  çıktıları birebir aynı.
- Rehber adımıyla `KEYWORD_MAP`'e geçici `{"loop", KW_WHILE}` eklendi:
  `loop (i < 3) {...}` VM ve `--jit`'te `012`, token `[keyword]`; geri alındı.
- Yeni `tests/general/token_model_test.sh` (CTest `token_model`): `EOL`
  değişkeni, `date` identifier / `while` keyword etiketleri, E906/E907
  konumu ve E907'nin sözdizimi hatalarından önce gelmesi. Baseline ikiliye
  karşı `EOL` senaryosunda düşüyor.
- `cmake --build build` uyarısız; `ctest -j8` 348/350 (düşen `lsp_28`/`lsp_29`
  df15a5c'den beri, ilgisiz).

## Kullanıcıya görünen değişiklik
- `EOL` adlı ad artık programı kesmiyor (hata düzeltmesi).
- Sözcüksel tanılar tokenizer'da üretildiği için, aynı dosyada parse hatası
  string'den önceki bir satırdaysa tanı sırası değişebilir (kodlar, mesajlar,
  konumlar aynı). Ölçülen örneklerde sıra değişmedi.
- `bench` ve `tokens` tokenizer'a diag vermez: E906/E907 bu iki komutta
  raporlanmaz (eskiden `bench`'te parser bunları stderr'e basıyordu).

## Kanıtlanmayanlar / kalan
- Tanınmayan karakter (`@`) hâlâ tanısız boş identifier token'ı üretiyor;
  kanal artık var, yeni bir tanı kodu (#295 kapsamında) ile eklenebilir.
- LSP keyword tamamlama listesi (`lspKeywords()`) hâlâ elle; `KEYWORD_MAP`'in
  desteklenen alt kümesi olduğu için ayrı bir bayrak gerekir (#287).
- `KW_DATE` enum değeri ve parser'daki `KW_DATE` dalları ölü kaldı (#287
  madde 3 / tip adları).
- `docs/free-performans-kazanimlari.md` (#220) `gettype()` performans planını
  anlatıyor; o plan bu değişiklikle kısmen karşılandı, belge güncellenmedi.
