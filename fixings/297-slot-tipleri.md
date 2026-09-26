# #297 — IR slot tipleri opcode'dan tahmin ediliyor (yeni opcode → sessiz Int)

**Görev (tek cümle):** Bir opcode'un sonucunun türü, opcode'un tanımlandığı
tek yerde (OPCODE_LIST satırı) söylensin; `finalizeSlotTypes` bunu okusun ve
yeni opcode ekleyen kişi bu bilgiyi vermeden derleyemesin.

## Doğrulanan durum (`1.0.1` @ df15a5c)

- `finalizeSlotTypes` (`ir_generator.cpp` ~2257): 150 satırlık `switch
  (ins.opcode)`; Float/Float32/Long/Ref/Str/Decimal üreten 60+ opcode elle
  listeli, `default: break` → listede olmayan opcode sessizce Int.
- OPCODE_LIST (#132) enum/ad/arite/backend için tek kaynaktı, sonuç türü
  bilgisi yoktu; başlık yorumu "başka yerde elle senkron switch kalmadı"
  diyordu (yanlış).
- İssue'nun iki önerisi: (1) her slotu üretim anında tiple yazmak
  (`freshSlot(SlotType)`), (2) geçişte default'suz switch + `-Werror=switch`.

## Seçilen yol ve gerekçe

OPCODE_LIST'e dördüncü sütun: `X(ISIM, ARITE, BACKENDS, SONUC)`, değerler
`OpResult` enum'u (`None, Int, Long, Float, Float32, Decimal, Str, Ref,
ValueType, Copy, Call, Host, Null`).

- Öneri (2)'nin amacını daha güçlü karşılar: yeni satır sütunsuz yazılamaz
  (makro 4 argüman ister → derleme hatası), üstelik bilgi opcode'un TANIM
  satırında durur, ayrı bir switch'te değil.
- Öneri (1) `ir_generator.cpp`'nin tüm emit noktalarına (3000+ satır) yayılan
  bir değişiklik; bu tur için risk/fayda oranı düşük. Sütun, ileride (1)
  yapılırsa üretim anı tipiyle karşılaştırılacak tutarlılık kaynağı olarak da
  kullanılabilir.
- `finalizeSlotTypes` opcode switch'i → `opcodeResult()` üzerinden 13 dallı,
  default'suz bir switch (OpResult'a değer eklenirse -Wswitch uyarır).

Davranış korunumu için `Int` ve `None` ikisi de "slota dokunma" demektir
(eski `default: break` ile aynı: slot varsayılanı Int, çok yazıcılı slotta
önceki tip korunur).

## Yapılan

- `src/ir/instruction.hpp`: `OpResult`, 123 satıra sütun, `opcodeResult()`,
  başlık yorumu (sütunun anlamı; opcode gövdesinin nerelerde ayrıca yazıldığı
  — "elle senkron switch kalmadı" iddiası düzeltildi). `CALLHOST` satırındaki
  "şu an yalnız print" bayat notu ve `ENTER_TRY`'ın dest'i düzeltildi.
- `src/ir/ir_generator.cpp` `finalizeSlotTypes`: opcode listesi kaldırıldı.
- `tests/test_opcode.cpp`: sütun testi — adı `_TO_<TÜR>` olan her dönüşüm
  opcode'unun sütunu adındaki türle aynı olmalı; F32/L/D/F aileleri ve özel
  kurallı opcode'lar.
- `docs/learn/03-yeni-veri-tipi-ekleme.md` (ana ağaçta, izlenmeyen dosya):
  yeni opcode örneğine dördüncü alan.

## Kanıt

- **Eşdeğerlik:** scratchpad'de derlenmiş nesnelere bağlanan bir araç
  (`slotdump`) `tests/golden` + `examples` altındaki 253 `.sqt`'den derlenen
  218 programın tüm fonksiyonlarının `slotTypes` ve `slotNullable`
  tablolarını bastı (5723 satır). Değişiklik öncesi ve sonrası `diff`: **boş**.
- `test_opcode` geçiyor (123 opcode). Negatif kontrol: başlığın bir
  kopyasında `CAST_DECIMAL_TO_FLOAT` sütunu `Int` yapılınca test düşüyor.
- `cmake --build build` uyarısız; `ctest -j8` 347/349 (VM≡JIT diferansiyel
  testleri dahil; düşen `lsp_28`/`lsp_29` df15a5c'den beri, ilgisiz).

## Kalan / kanıtlanmayan

- Sütunun yanlış seçilmesi (ör. yeni bir `Float` üreten opcode'a `Int`
  yazmak) hâlâ derleme hatası değil. Ad kuralına uyan dönüşüm opcode'ları
  test yakalar; diğerleri için `--jit` diferansiyel testi gerekir.
- `slotNullable` fixpoint'indeki fallible cast listesi (`left == 1` → nullable)
  hâlâ elle; yeni bir fallible cast bu listeye eklenmezse `as T?` sonucu
  JIT'te nullable işaretlenmez. Ayrı bir sütun/bayrak olarak ele alınabilir.
- Parametre slotları hâlâ tip ADINDAN (`slotTypeFromTypeName`, string) çözülüyor
  — #287 madde 3 / #289 kapsamı.
