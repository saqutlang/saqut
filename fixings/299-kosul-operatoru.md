# #299 — `?:` sözdizimi sessizce kabul ediliyor (hata sınıfı: ikili anlamı olmayan token'lar)

**Görev (tek cümle):** Öncelik tablosunda yeri olan ama ikili anlamı olmayan
her token infix konumda sözdizimi hatası versin; hiçbir katman bilinmeyen
operatörü sessizce bir değere indirmesin.

## Doğrulanan durum (`issue-295-tani-kodlari` @ 835f42e)
- `int a = 1 ? 2 : 3;`, `7 ? 5`, `5 ! 3`, `5 ~ 3`, `1 : 2` → hatasız derlenip
  `0` basıyordu (issue yalnız `?:`'yı anıyordu; sınıf daha geniş).
- Katmanlar: parser `parseLeftDenotation` özel dalı olmayan her öncelikli
  token'dan `BinaryExpression` kuruyordu; TypeChecker tanımadığı operatörü
  aritmetik sayıyordu; IRGenerator `default:` dalında `LOAD_CONST 0`
  üretiyordu (optimizasyon kapalıyken de — sabitin kaynağı IR'dır).

## Yapılan
- `parser/parser_token.hpp`: `IsBinaryOperator()` — BinaryExpression
  kurulabilecek infix operatörlerin tek listesi.
- `parser.cpp`: listede olmayan token → E901 (`?`/`:` için "conditional
  expressions (a ? b : c) are not supported; use if/else"); token tüketilir,
  ifadenin kalanı atlanır, kapsayan sınırlayıcı tüketilmez → tek tanı.
- TypeChecker: aritmetik/bitsel bölüme gerçek aritmetik/bitsel operatör ya da
  bitsel bileşik atama dışında bir şey gelirse E021 (savunma).
- IRGenerator: bilinmeyen operatör `default:` → `std::logic_error` (iç hata);
  sessiz 0 kalktı.
- `tests/general/non_binary_operator_test.sh` (CTest `non_binary_operator`):
  6 negatif (her biri tam 1 E901) + pozitif (nullable `?`, `as int?`, önek
  `!`/`~`, `&=`/`<<=`). Eski ikiliye karşı düşüyor.

## Kanıt
- Debug build uyarısız; `ctest -j8` 352/354 (düşen `lsp_28`/`lsp_29`
  df15a5c'den beri, ilgisiz).

## Ayrı issue'ya bırakılan yan bulgu
- `5 = 3;`, `5 += 3;`, `5 &= 3;` hatasız derleniyor: atama hedefinin
  yazılabilirliği denetlenmiyor. `&=`/`|=`/`^=`/`<<=`/`>>=` TypeChecker'ın
  atama dalında değil (aritmetik daldan geçiyor; sonuçlar doğru).
