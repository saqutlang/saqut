# PR #305'i 1.0.1'e düzgün birleştirme planı

> Konu: `saqutlang/saqut` PR #305 — "Issue 303 atama hedefi"
> Branch: `issue-303-atama-hedefi` (head `69913ef`) → base `1.0.1` (`e9882b0`)
> Durum: GitHub'da `MERGEABLE`/`CLEAN`, ama birleşen ağaç **derlenmiyor**. Bu plan onu düzgün indirmek içindir.

---

## 0. Kanıt özeti (planın temeli)

- PR branch'i `df15a5c`'den dallanmış; `1.0.1` (`e9882b0`) aynı noktadan ilerlemiş (#304 `72bd1ca`).
- `git merge-tree origin/1.0.1 HEAD` → **çakışmasız**, ama sonuç **derlenmiyor**:
  `src/ir/ir_generator.cpp:1460: error: 'dataLookupMethod' was not declared in this scope`.
- Onarım repo dışı `/tmp/merged` kopyasında denendi: **tek satır** değişince build temiz ve **362/362 test geçti** (`ctest --test-dir build -j8`).
- Etkileşim yüzeyi yalnız üç dosya: `src/data/string.cpp`, `src/ir/ir_generator.cpp`, `src/semantic/type_checker.cpp`. `string.cpp` ve `type_checker.cpp` uyumlu; kırık tek yer `ir_generator.cpp`.

---

## 1. Alınacak karar (ürün sahibi)

**Kapsam:** PR başlığı "#303" ama gövdesi boş ve içinde **8 issue** var
(#290, #291, #295, #296, #297, #298, #299, #303). İki yol:

- **(A) Tek PR olarak landir:** hızlı, ama ~108 dosya tek pakette ve #303 hâlâ "kısmi"
  (issue açık). PR başlığını/gövdesini gerçeği yansıtacak şekilde güncellemek şart.
- **(B) Issue bazına böl:** daha temiz kayıt, ama 8 ayrı PR + rebase emeği.

Aşağıdaki adımlar iki durumda da aynı; yalnız başlık/gövde adımı (5.2) A'ya özgüdür.

---

## 2. Ön koşullar

- [ ] 1.0.1'in donmuş bir revizyonda olduğunu teyit et:
      `git fetch origin && git rev-parse origin/1.0.1` (şu an `e9882b0`).
- [ ] Temiz worktree (şu an temiz) ve PR head'in push edilmiş olduğunu teyit et.
- [ ] Force-push izni gerekiyorsa (Strateji R) açık onay al.

---

## 3. Strateji seçimi

- **Strateji R (önerilen) — feature branch'i 1.0.1 üstüne rebase:**
  temiz/lineer geçmiş, "tam base revision" ilkesi (AGENTS.md §7).
  Bedeli: PR branch'ine **force-push** gerekir (geçmiş yeniden yazılır).
- **Strateji M — 1.0.1'i feature branch'e merge et:**
  force-push yok, ama branch'te merge commit oluşur ve PR diff'i kirlenir.
- Her iki yolda da kırık satır **elle** düzeltilir; git bunu çakışma olarak
  **göstermez** (sessiz otomatik birleşim). Kritik nokta budur.

---

## 4. Strateji R — adım adım

```bash
cd /home/saqut/Masaüstü/saqutcompiler-fixings
git fetch origin
git worktree add /tmp/saqut-305-merge -b issue-303-atama-hedefi-rebased origin/issue-303-atama-hedefi
cd /tmp/saqut-305-merge
git rebase origin/1.0.1
```

Rebase sırasında:
1. Çakışma **çıkmayabilir**. Çıkmazsa bile (bkz. 5.1) derleme kırılacaktır — sadece
   1.0.1'in dokunduğu üç dosyada (`string.cpp`, `ir_generator.cpp`,
   `type_checker.cpp`) dikkatli ol.
2. Çakışma çıkarsa çöz ve `git rebase --continue`.

Rebase bitince 5. bölümdeki düzeltmeyi ayrı bir commit olarak ekle.

---

## 5. Zorunlu iş

### 5.1 Tek satırlık onarım (bloklayıcı)

`src/ir/ir_generator.cpp` — `IndexExpression` düğümündeki `s[i] → charAt` yolu:

```cpp
// önce (1.0.1 / #304 — artık var olmayan API):
static const int charAtId =
    dataMethodId(dataLookupMethod("string", "charAt", false, false));

// sonra (#290 registry API'si):
static const int charAtId =
    dataMethodId(dataFindMethod(DataMethodCategory::StringVal, "charAt"));
```

Gerekçe: `"charAt"` `DataMethodCategory::StringVal` altında kayıtlı
(`src/data/string.cpp:255`); `dataMethodId(const DataMethod*)` her iki tarafta da var;
`data_registry.hpp` zaten include edilmiş. Bu değişiklik `/tmp/merged`'de doğrulandı.

### 5.2 PR kaydı (yalnız A seçilirse)

Başlığı ve gövdesini gerçeği yansıtacak şekilde güncelle: hangi 8 issue, hangisi
kısmi, hangi kanıtla. Boş gövde bırakma.

---

## 6. Doğrulama kapıları (atlanamaz)

```bash
cd /tmp/saqut-305-merge
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build                 # KAPI 1: uyarısız derlenmeli
ctest --test-dir build -j8          # KAPI 2: 362/362 (LSP dahil) yeşil
```

- **KAPI 3 — elle tekrar üretme:** `5 = 3;`, `5 += 3;`, `5 &= 3;` üçü de E027 vermeli;
  `arr[0]=`, `p.x=`, `a+=` geçmeli.
- **KAPI 4 — `git diff --cached --name-only`** ile stage'in yalnız görev path'leri
  olduğunu teyit et (`git add -A` yok).
- **LSP notu:** Testi ASCII yolda (`/tmp/...`) çalıştır; `Masaüstü` yollu asıl
  checkout'ta `lsp_28`/`lsp_29` **çevresel** olarak düşer (fixture'lar dosya URI'sinde
  literal `Masaüstü` beklerken binary `Masa%C3%BCst%C3%BC` üretir), PR'ın suçu değildir.
  Bu iki testi yeşil görmenin tek yolu ASCII path'tir.

---

## 7. PR'ı güncelle / merge et

- **Strateji R:** `git push --force-with-lease origin issue-303-atama-hedefi-rebased:issue-303-atama-hedefi`
  (force-push **açık onayınızla**). PR otomatik güncellenir; sonra GitHub'da
  **"Rebase and merge"** veya **"Squash and merge"** ile 1.0.1'e indir.
- **Strateji M:** branch'e merge commit push edilir (force yok); PR "Merge" ile iner.
- Merge'ü **KAPI 1–3 yeşil olmadan tetikleme**. Merge'den sonra 1.0.1'de bir kez daha
  `cmake --build build && ctest` çalıştır (temiz 1.0.1 üzerinde doğrula).

---

## 8. Geri alma / güvenlik

- Orijinal `origin/issue-303-atama-hedefi` (`69913ef`) ve `origin/1.0.1` (`e9882b0`)
  **silinmez**; rebase ayrı bir worktree/branch'te yapılır, asıl ref'ler dürüst kalır.
- `git reset --hard`, `git checkout --`, `git add -A` **kullanılmaz** (AGENTS.md §7).
- 1.0.1'i başka sürüm dallarıyla (1.0.0, 1.0.1-webserver) birleştirme — yalnız
  feature→1.0.1 PR akışı.

---

## 9. Bilinmeyenler / riskler

- **Rebase çakışmaları öngörülemez:** merge-tree temiz çıktı, ama commit-commit rebase
  üç ortak dosyada çakışma üretebilir. Çözüm 5.1'deki satır ve mevcut kodla sınırlı tutulmalı.
- **#303 kısmi:** bitsel bileşik atamalar hâlâ E016/W008 dalında değil; land edilse bile
  #303 **kapanmamalı**.
- **Tek başına branch zaten 352/354** (kalan 2 = çevresel LSP); birleşim sonrası hedef
  **362/362**.

---

## Özet

Birleştirme teknik olarak yapılabilir; engel, bayat tabandan kaynaklanan **tek satırlık**
API uyumsuzluğu. Rebase + o satırın
`dataFindMethod(DataMethodCategory::StringVal, "charAt")` ile düzeltilmesi + tam
build/ctest kapısı yeterli.
