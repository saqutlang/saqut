# #303 — Atama hedefi denetlenmiyor (kısmi)

**Yapılan:** TypeChecker'da her atama operatörü (`=`, `+=`…`%=`, `&=`, `|=`,
`^=`, `<<=`, `>>=`) için hedef yazılabilir konum olmalı (Identifier,
MemberAccess, IndexExpression); değilse E027 (`++`/`--` ile aynı kural).
`5 = 3;`, `5 += 3;`, `5 &= 3;` artık E027. E027 katalog açıklaması
güncellendi, `docs/compiler-errors.md` yeniden üretildi.
`tests/general/diagnostic_split_test.sh`'a üç senaryo eklendi.

**Kanıt:** Debug build uyarısız; `ctest -j8` 352/354 (düşen `lsp_28`/`lsp_29`
df15a5c'den beri, ilgisiz).

**Kalan (issue açık):** `&=`, `|=`, `^=`, `<<=`, `>>=` hâlâ TypeChecker'ın
atama dalında değil; E016 (thread yakalaması) ve W008 (shared atomik
olmayan güncelleme) kuralları bu operatörlere uygulanmıyor. Taşımak shared
değişkenlerde yeni uyarılar doğurabileceği için IR/VM davranışıyla birlikte
ayrıca doğrulanmalı.
