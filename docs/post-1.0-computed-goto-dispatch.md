# [1.0 sonrası] VM dispatch'ini computed-goto'ya çevirme

**Durum:** Kayıtlı iş — 1.0 kapsamı dışında. Kaynak karar: ADR-045 tur 2,
S1 (`docs/threading-decisions.md` "S1 — VM gerilemesi").

## Arka plan

ADR-045 threading işinden sonra tek thread VM benchmark'ında ölçülen fark:

- Komut sayısı (cachegrind): a-öncesine göre **+%0.76** (kaynak düzeyindeki
  gerçek maliyet; `executeThreadOp` frame kaçışı düzeltmesinden sonra).
- Duvar süresi: **+%5–6**. Yalnız `-falign-jumps/labels/loops=32` ile
  a-öncesinin kendisi aynı miktarda yavaşlıyor; iki ikili aynı bayraklarla
  derlendiğinde fark gürültü içinde (−%1.4 / +%0.8). Yani fark, tek
  `switch` üzerinden dağıtan interpreter döngüsünün makine kodu yerleşimine
  (dolaylı sıçrama hedefleri, uop önbelleği) bağlı; switch'e herhangi bir
  case eklemek bu piyangoyu yeniden çeker.

## Öneri

`Interpreter::runUntilEvent` dispatch'ini GCC/Clang "labels as values"
(`&&label`, `goto *table[op]`) ile doğrudan-iş-parçacıklı (threaded)
dispatch'e çevirmek:

- Her opcode gövdesinin sonunda kendi dolaylı sıçraması olur → dal tahmin
  geçmişi opcode çiftlerine göre ayrışır; tek merkezi `jmp *%rax`'ın
  yerleşim duyarlılığı ortadan kalkar.
- Taşınabilirlik: MSVC desteklemez → `#if defined(__GNUC__)` ile computed
  goto, aksi halde mevcut `switch` (aynı gövde makrolarıyla).

## Kapsam / riskler

- Dispatch döngüsündeki DAP kancaları (breakpoint, adım, bütçe, `vmTrace_`
  profil kancası, GC safepoint `maybeCollect`) her gövde sonundaki ortak
  "NEXT" makrosuna taşınmalı; aksi halde davranış değişir.
- Soğuk opcode'lar (threading, host çağrıları) noinline yardımcılarda
  kalmalı (`executeThreadOp` örneği).
- 0.8/1.0 VM kodunun tamamına dokunur → ayrı branch, tam `tests/run.sh`,
  ctest, DAP golden'ları, GC stres ve TSan.

## Kabul ölçütü

- heavy benchmark'ta (fib(30) + struct/string/dizi döngüleri) VM duvar
  süresi a-öncesine göre ≤ +%1, hizalama bayraklarıyla ve bayraksız
  derlemede ayrı ayrı ölçülerek (ABBA dönüşümlü koşu, medyan + minimum);
  mümkünse donanım sayaçlı bir makinede IPC ile doğrulanmalı.
- Tüm test takımları yeşil.
