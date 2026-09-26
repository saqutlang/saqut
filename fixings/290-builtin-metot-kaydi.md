# #290 — Built-in metot sistemi: ölü alan, bayat ipuçları, kopya kurallar

**Görev (tek cümle):** Bir built-in metodun imzası, alıcı kısıtı ve hangi
alıcı tipine ait olduğu tek kayıttan okunsun; tip denetleyici ve LSP aynı
fonksiyonları kullansın.

## Doğrulanan durum (`1.0.1` @ df15a5c)

| Issue maddesi | Kaynakta | Doğrulandı |
|---|---|---|
| 1. `DataMethod::mutating` ölü | `src/data/data_type.hpp:109`; okuyan kod yok (grep boş). Aynı bilgi `HOST_MUTATING` bayrağında da var | evet |
| 2. E001 ipucu elle yazılmış, bayat | `type_checker.cpp` (~1746): string listesinde `length`, `toBuffer` yok; dizide `toString` yok | evet |
| 3. `toString` yalnız `byte[]` kuralı iki kopya | TC (~1731) ve `lsp_analysis.cpp` (~437), ikisi de ada göre `"toString"` karşılaştırıyor | evet |
| 4. Pool/List/Thread metotları kayıt dışı | 3 kopya: `checkThreadIntrinsic`, LSP `threadMethodsForType`, LSP `methodReturnType` | evet (issue 2 dedi) |
| 5. Alıcı kategorisi string'den | `dataLookupMethod(leftName, ...)` `leftName == "string"`; TC, LSP analiz ve LSP handler ayrı ayrı `leftName` üretiyor | evet |

Ek bulgular:
- LSP `builtinMethodsForType` struct dizisinde (`Person[]`) `toJson`/`dump`
  öneriyordu; TC bunları reddediyor. Ayrıca adı büyük harfle başlayan her
  değişkene struct metotları öneriyordu (ad sezgisi).
- `data_registry.cpp` "sıra kararlıdır", `array.cpp`/`string.cpp` "sıra
  önemsizdir" diyordu; çelişki. Doğrusu: sıra runtime id'dir, ADR-044 gereği
  kararlı tutulur.

## Yapılan

- `mutating` alanı ve 28 tablo satırındaki değeri kaldırıldı; `data_type.hpp`'ye
  "yerinde değiştirme yalnız heap alıcıda mümkün" kısıtı yorum olarak yazıldı.
  Davranış değişmez (alanı okuyan kod yoktu). Değer tipli alıcıya mutating
  metot eklemek ayrı bir dil kararıdır; yapılmadı.
- `data_registry`: string tabanlı `dataLookupMethod` yerine
  `dataReceiverCategory(Type)`, `dataFindMethod(category, name)`,
  `dataMethodAcceptsReceiver`, `dataMethodNames`. Önek haritası
  (`"sv:"`, `"ar:"`) kaldırıldı.
- `toString` alıcı kuralı kayıtta: `params[0] = dpFixed(byte[])`. TC ve LSP
  `dataMethodAcceptsReceiver` ile okuyor; ada göre özel durum yok.
- E001 ipuçları kayıttan türüyor: `methods of 'string': length, upper, ..., toBuffer`.
- `src/semantic/thread_intrinsics.hpp`: Pool/List/Thread metotlarının tek
  tablosu (op, argüman/dönüş kuralı, kilitte bloklama, belge). TC
  `checkThreadIntrinsic` 60 satırlık `if` zincirinden tablo aramasına indi;
  LSP tamamlama, dönüş tipi ve imza yardımı aynı tablodan.
- Belgeler: `knowledge-base/05_Runtime.md` §31 (#223 öncesinden kalma
  `builtin_methods.hpp`/VM switch anlatımı düzeltildi),
  `docs/learn/03-yeni-veri-tipi-ekleme.md` (ana ağaçta, izlenmeyen dosya).

## Kullanıcıya görünen değişiklikler (yalnız tanı ve editör)

- `s.push(1)` (string): eski `'s': cannot assign string to string[]` +
  `'1': cannot assign int to string` yerine
  `'push' is not a built-in method for type 'string'` ve string metot listesi.
  Neden: eski arama string'de bulamayınca dizi metotlarına düşüyordu.
- `int[].toString()`: mesaj `byte[]::toString requires a byte[] receiver` →
  `'toString' requires a byte[] receiver, got 'int[]'`.
- Pool/List/Thread argüman tanısının bağlam metni: `get index` → `get argument`.
- LSP: struct dizisinde `toJson`/`dump`, büyük harfli skaler değişkende struct
  metotları artık önerilmiyor (derleyicinin reddettiği öneriler).
- Eski sözdiziminde (`string::x`, `Person::x`, W006) bulunmayan metot artık
  dizi metotlarına düşmüyor; aynı "not a built-in method" hatasını veriyor.

## Kanıt (Debug build, worktree, dal `issue-290-builtin-metot-kaydi`)

- `cmake --build build`: uyarısız.
- `ctest -j8`: 347/349. Düşen `lsp_28_workspace_multi`, `lsp_29_editor_features`
  #291 öncesinden beri düşüyor (df15a5c lens başlığı).
- Ara koşuda `lsp_25/26/27` Thread tamamlama sırası yüzünden düştü (tablo
  `stop, join, running` sırasındaydı); tablo LSP'nin test edilen sırasına
  (`join, stop, running`) alındı, üçü de geçiyor.
- Yeni `tests/general/builtin_method_registry_test.sh` (CTest
  `builtin_method_registry`): baseline ikiliye karşı ilk senaryoda düşüyor,
  yeni ikiliyle geçiyor.

## Kanıtlanmayanlar

- LSP'deki öneri daralmaları (struct dizisi, büyük harfli ad) için tracked
  LSP fixture'ı yok; yalnız kaynak okunarak ve mevcut 30+ LSP senaryosunun
  geçmesiyle doğrulandı.
- `flags` (HOST_*) alanının tüketicisizliği #288'in konusu; burada dokunulmadı.
