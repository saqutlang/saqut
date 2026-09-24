# Threading devir belgesi — `v1.0.1-multithread`

Bu belge, işi devralan ajanın **tek bağlamıdır** (o bu konuşmayı görmez). Yanında
zorunlu referans: `docs/adr/ADR-045-isolate-threading.md` ve
`docs/threading-feasibility.md`.

---

## A) Şu anki durum (tamamlanan adımlar + commit hash'leri)

Dal: **`v1.0.1-multithread`**. Her adımda `bash tests/run.sh` **rc=0** tutuldu.

- **1.1 + 1.2** → commit **`8787176`**: ADR-045 yazıldı; ucontext maddesi
  "Reddedilen"den **"Ertelendi"**ye taşındı; `docs/threading-feasibility.md`
  (olgusal uygunluk raporu, file:satır) eklendi ve ADR-045'ten linklendi.
- **`chore: eski tasks/ dizinini kaldır`** → commit **`ea16892`** (kullanıcı
  onaylı; ~990 dosya silindi).
- **(a) Isolate temeli + dağınık thread_local'ların birleştirilmesi** → commit
  **`07dcc04`**: `src/runtime/isolate.hpp` (`Isolate{ShadowStack,stringHeap,
  rng}` + `t_isolate` + `current()`), `gc/shadow_stack.cpp`, `gc/gc_heap.cpp`,
  `ffi/functions/sys.cpp` buraya bağlandı.
- **(a) JitRuntime Isolate üyesi** → commit **`1bcc081`**: `JitRuntime` +
  `JitTraceFrame` + `JitStructMeta` `src/runtime/jit_runtime.hpp`'ye taşındı;
  `rt()` = `Isolate::current().jit`; `JitRuntime g_jitRuntime;` **silindi**.
- **(b) ConstPool + immortal** → commit **`0503b4f`**: `Object::immortal` biti;
  `gc_heap.cpp markObject` immortal'da erken döner; `src/runtime/const_pool.hpp`
  (süreç-global); `LOAD_STRING` ConstPool'dan.
- **(c) hazırlık** → commit **`9b468ae`**: `Isolate`e `const CompiledProgram*
  program`; `Isolate::currentOrCreate()` + `IsolateGuard` RAII; ADR-045'e
  gömme kuralı, ConstPool program-ömürlü, MIR eager değişmezi, Isolate alan
  listesi, CompiledProgram yaşam döngüsü + `activeIsolates`.
- **Escape testi** → commit **`5d72451`**: `tests/golden/ffi/host_return_copy.sqt`
  (+`.expected`); host dönüşünün scratch owner'dan **kopyalandığı** interpreter
  ve JIT'te doğrulandı.
- **(A) decimal literal'leri ConstPool'a + embedProgramPtr** → commit
  **`855aa34`**: `ConstPool::internDecimal` + `owns()`; `LOAD_DECIMAL` ConstPool'dan
  (`jitBoxDecimal` codegen yolundan çıktı); `embedProgramPtr(ctx,p)` tek gömme
  yolu; 4 ham gömme dönüştürüldü; ADR'ye zorlama + immutability kapsamı.

**Not:** `CompiledProgram` tipi ve `compileProgram`/`runOnIsolate` **henüz
yazılmadı** (c1). `tryCompileAndRunProgram` hâlâ yekpare.

**Kaldırıldı (2026-09-24):** Kullanıcının kök `threading/` altındaki ucontext
denemesi depodan silindi (sahibi kararı). ucontext fikri ADR-045'te
**"Ertelendi"** olarak durur; yalnız dizin artık yoktur ve ona ilişkin
"dokunma" kuralı Bölüm 0'dan çıkarıldı. (Bölüm 4'teki `src/runtime/threading/`
ve Bölüm 5'teki `examples/threading/` farklı, gelecekte oluşturulacak
dizinlerdir.)

---

## B) Master prompt (bölümler eksiksiz)

### Bölüm 0 — ÇALIŞMA KURALLARI
- Fazları SIRAYLA uygula. Her faz sonunda DUR, raporla, onay bekle. Sonraki
  faza kendiliğinden geçme.
- Her faz ayrı commit(ler): "threading(fazN): ...".
- Spesifikasyonla çelişen mevcut kod, beklenmeyen gömülü adres ya da paylaşılan
  mutable yapı bulursan DUR ve raporla. Kendi başına tasarım kararı verme. Bu
  promptta cevabı olmayan her soru için dur ve sor.
- Tek thread davranışı her fazda birebir korunur: mevcut test paketinin tamamı
  her faz sonunda geçmeli.
- Yeni kod C++20: `std::jthread`, `std::stop_token`,
  `std::condition_variable_any`, `std::atomic` (wait/notify dahil),
  `std::scoped_lock`, `std::latch`. Boost ya da yeni bağımlılık YOK.
- Concurrency testleri iki modda koşar: normal ve GC stres modu (eşik minimumda,
  her tahsiste toplama). Hepsi `-fsanitize=thread` ile de temiz geçmeli.

### Bölüm 1 — cevapları (uygulama talimatı)
- **1.1** ADR-045 commit'le; ucontext maddesini "Ertelendi"ye taşı (yapıldı).
- **1.2** Uygunluk raporunu `docs/threading-feasibility.md` yap, ADR'den linkle
  (yapıldı).
- **1.3 Lambda lifting:** type check'ten SONRA, IR üretiminden ÖNCE ayrı AST
  dönüşüm geçişi (ör. `src/semantic/thread_lifting.cpp`). Kurallar:
  `thread { gövde }` → üst seviye sentetik fonksiyon
  `void __thread_<fonksiyon>_<n>(yakalananlar...)`. Serbest değişkenlerden
  çevreleyen fonksiyonun yerel/parametresi olanlar **KOPYA** ile yakalanır ve
  sentetik fonksiyonun parametresi olur; değerleri thread başladığı anda mesaj
  gibi serialize edilir (deep copy). İstisna: Thread handle'ı (id olarak
  kopyalanır). Gövde içinde yakalanmış değişkene atama → derleme hatası
  ("yakalanan değişken bir kopyadır"). shared olmayan global'e referans →
  thread'in kendi kopyası; shared global → doğrudan erişim. İç içe `thread {}`
  desteklenir; gövdede `return` thread'i bitirir; gövde void.
- **1.4 Global init:** `IRGenerator`, global başlatma ifadelerini main'in
  başından iki sentetik fonksiyona taşısın: `__init_shared()` (shared globaller;
  süreç başında BİR KEZ, main thread'inde, her şeyden önce) ve
  `__init_globals()` (shared olmayan globaller; main thread'i main()'den önce,
  her yeni thread'in isolate'i de gövdeden önce çağırır). Global initializer'ların
  yan etkileri thread başına tekrar çalışır. Bunu ADR'ye yaz.
- **1.5 Pool/List parse:** `Pool` ve `List` anahtar kelime olur. Parser `Pool(`
  ya da `List(` gördüğünde parantez içini mevcut tip parse fonksiyonuyla TİP
  olarak okur (böylece `Pool(int)` de çalışır).
  `examples/parser-stress/Final.sqt` içindeki `struct List`'i yeniden adlandır.
- **1.6 Tip sistemi:** `TypeKind`'a `Pool, List, Thread` eklenir. Pool ve List,
  Array'deki `elementType` alanını kullanır. `shared` tipin değil **sembolün**
  özelliğidir: `Symbol`'e `isShared` bayrağı.

### Bölüm 2 — GENEL MİMARİ ÖZETİ
- Her thread = isolate: kendi Heap, GC, shadow stack, globalSlots, RNG, JIT
  runtime durumu. Başka bir isolate'in heap'ine asla dokunulmaz.
- Kod bir kez derlenir, tüm thread'ler aynı kodu çalıştırır. String sabitleri
  immortal ConstPool'da.
- Shared veri heap dışında, süreç-global SharedSlots tablosunda yaşar; heap
  pointer'ı içermez; her erişimde kopyalanarak sınırı geçer.
- v1 KISITI: Pool(T)/List(T) yalnızca shared global bildiriminin initializer'ında
  kullanılabilir. Böylece Pool/List için heap'te handle ve GC finalizer'ı
  GEREKMEZ.
- Thread handle = süreç-global ThreadTable'a bir tamsayı id. Heap nesnesi değil,
  finalizer gerekmez.

### Bölüm 3 — FAZ 1 (ISOLATE REFACTOR, dil değişikliği yok)
1. `src/runtime/isolate.hpp`: `struct Isolate` (Heap, ShadowStack, hostFrame,
   pendingError, jitRootSource, string heap kancası, RNG, globalSlots).
   `thread_local Isolate* t_isolate;` + `Isolate::current()`.
2. `g_jitRuntime`/`rt()` kaldırılsın; erişimler `Isolate::current()` üzerinden.
   Dağınık thread_local'lar (shadow_stack.cpp:9, mir_backend.cpp:372,
   gc_heap.cpp:311) Isolate'e taşınsın.
3. `LOAD_STRING`'in gömdüğü nesneler (mir_backend.cpp:2152-2159, :1637-1650)
   immortal ConstPool'dan ayrılsın; markObject bu adres aralığını atlasın. Havuz
   derlemede doldurulur, sonra salt okunur.
4. Derleme/koşu ayrımı: `CompiledProgram compileProgram(...)` (MIR ctx + fonksiyon
   giriş tablosu) ve `int runOnIsolate(const CompiledProgram&, Isolate&)`.
   `MIR_finish` tüm koşular bitince.
5. Interpreter heap'i ve globalSlots'u Isolate'ten alsın; IR programı salt okunur
   paylaşılır.
6. FileRegistry ve FfiCatalog'a `freeze()` (derleme sonunda; sonrasında mutasyon
   = assert). sysRng Isolate üyesi.
7. print: satır başına tek mutex.
8. Bölüm 1.4'teki `__init_shared`/`__init_globals` ayrımı bu fazda (henüz shared
   yok; `__init_shared` boş kalır).
9. Test: `tests/isolate_concurrency_test.cpp` — bir programı bir kez derle,
   4 std::thread'de ayrı Isolate'lerle eşzamanlı koştur, JIT ve interpreter;
   çıktılar referansla aynı; iki mod; TSan temiz.

### Bölüm 4 — FAZ 2 (RUNTIME PRİMİTİFLERİ, `src/runtime/threading/`, dil yok)
1. ThreadTable/ThreadCore: id, ad, `std::jthread`, Isolate, durum
   (running/parked/finished), yakalanmayan hata bilgisi.
2. SharedSlots: slot türleri `atomic<int64_t>`, `atomic<double>`, `atomic<bool>`,
   PoolCore, ListCore. Her primitif slotun `lock` için kendi `std::mutex`'i.
3. Mesaj serileştirme, TİP YÖNLÜ: `serialize(const Value&, const Type&,
   MessageBuffer&)` ve `deserialize(MessageBuffer&, const Type&, Heap&)`. Graph
   kopyası: visited haritası ile aliasing ve döngüler korunur. String: uzunluk +
   byte. **SORU (cevapla, uygulama değil):** FFI native handle'ları (dosya, soket)
   bugün runtime'da nasıl temsil ediliyor? int id mi, void* mi? Buna göre
   "sahiplik devri" kuralını Faz 3'te belirleyeceğiz.
4. PoolCore: mutex, notEmpty/notFull (`condition_variable_any`), `deque<Message>`,
   max (`setMax`, 0 = sınırsız). push dolu kuyrukta bloklar, pop boşta bloklar;
   ikisi de `stop_token` ile uyanır ve iptal edilebilir.
5. ListCore: sabit boyutlu chunk'lardan oluşan liste (elemanlar asla yer
   değiştirmez). append mutex altında yazar, uzunluğu release ile yayınlar;
   get(i) acquire ile okur, KİLİTSİZ; i >= length → runtime hatası.
6. Park katmanı (v1 basit): global epoch sayacı + tek `condition_variable_any`.
   `park(pred, stop_token)` pred doğru olana ya da stop'a kadar bekler. Her
   shared mutasyon (atomik yazma, push, pop, append, unlock) epoch'u artırır ve
   notify_all yapar. Optimizasyon (sembol başına bekleyen listeleri) sonraya;
   kodda TODO ile işaretle.
7. Park öncesi GC: son toplamadan beri tahsis >= eşiğin yarısıysa park etmeden
   önce topla.
8. Deadlock dedektörü: park mutex'i altında liveThreads ve parkedThreads
   (pop/push/wait/join'de bekleyenler; sleep ve IO SAYILMAZ). parked == live
   olursa: her thread'in adını ve beklediği yeri yazdır, süreci hata koduyla
   sonlandır.
9. Testler (C++ birim, TSan): N üretici × M tüketici; sınırlı kuyrukta bloklama;
   pop'ta beklerken stop; List'e 4 thread × 100k eşzamanlı append → sayı ve
   eleman kümesi doğrulaması; serialize/deserialize gidiş-dönüş (aliasing +
   döngü); deadlock dedektörü pozitif ve negatif durum.

### Bölüm 5 — FAZ 3 (DİL YÜZEYİ)
Lexer: yeni anahtar kelimeler `shared, lock, unlock, wait, thread, Pool, List,
Thread`. Çakışma raporuna göre sadece `Final.sqt` etkileniyor.
Parser:
- `shared` global bildirim niteleyicisi; global dışında kullanım = hata.
- NUD'da `Pool(` / `List(` → tip argümanlı intrinsic; `thread` + `{` → ThreadExpr.
- Deyimler: `lock a;`, `lock a, b;`, `unlock a;`, `wait(ifade);`.
Semantik:
- shared'e izin verilen tipler: `int, float, bool, Pool, List`. (shared
  string/struct/array v1'de YOK, anlamlı bir hata ile.)
- shared primitif okuma/yazma atomik. `+=`, `-=` atomik read-modify-write.
  `x = x + 1` (ayrı yükle + yaz) → UYARI: "+= kullan ya da lock al".
- lock hedefi shared primitif olmalı. Kilit kapsam sonunda otomatik bırakılır;
  açık `unlock` desteklenir. Tutulmayan kilidi unlock / aynı kilidi iki kez alma
  → statik tespit edilebiliyorsa hata. `lock a, b` slot indeksine göre sıralı
  alınır.
- `wait` bir lock kapsamı içinde → HATA (v1). pop/push/join lock içinde → UYARI.
- wait koşulunda en az bir shared sembol yoksa → HATA. Koşul, yakaladığı yerelleri
  parametre alan sentetik `bool __wait_pred_<n>(...)` fonksiyonuna derlenir;
  runtime `park(pred)` döngüsü.
- Pool metotları: `push(T)`, `pop() -> T`, `setMax(int)`, `uzunluk`. List:
  `append(T)`, `get(int) -> T`, `uzunluk`. Uzunluk için mevcut dizi uzunluğu
  konvansiyonunu (property ya da metot) aynen izle.
- Thread: `Thread t = thread {...};` hemen başlar. Metotlar: `t.stop()`
  (bloklamaz), `t.join()`, `t.running()`.
- Yeni hata/uyarı kodları: mevcut son koddan devam et,
  `docs/compiler-errors` listesine ekle.
IR + VM + JIT:
- Yeni opcode'lar: `SHARED_LOAD/STORE/RMW`, `LOCK/UNLOCK`,
  `POOL_PUSH/POP/LEN/SETMAX`, `LIST_APPEND/GET/LEN`, `WAIT`,
  `THREAD_SPAWN(fnId, args)`, `THREAD_STOP/JOIN/RUNNING`. Tip argümanı olan
  opcode'lar tip tablosundaki id'yi taşır.
- Interpreter: doğrudan runtime primitiflerini çağırır.
- JIT: hepsi mevcut import mekanizmasıyla `rt_*` `extern "C"` host çağrılarına
  indirilir; `opcodeSupported`'a eklenir.
- Geri kenar yoklaması (back-edge poll): interpreter ve JIT'te her döngü geri
  kenarında isolate başına tek bir atomik bayrak kelimesi okunur (bit0: stop
  istendi, bit1: debug duraklatma). Bu, Faz 4'ün de temeli.
- Kilitler ve hatalar: isolate başına tutulan-kilit yığını (kilit + frame
  derinliği). Exception unwind sırasında açılan frame'lerin kilitleri bırakılır.
  stop ile çıkışta da.
- Yakalanmayan hata bir thread'de: stack trace yazdır, süreci sonlandır.
- main döndüğünde: tüm thread'ler join edilir (deadlock dedektörü aktif).
Testler: `examples/threading/` altında `.sqt` programları: üretici-tüketici,
append-only List, stop, lock ile iki değişkenli tutarlılık, beklenen deadlock
hatası. Her biri interpreter ve JIT'te, deterministik çıktıyla (toplamlar ya da
sıralanmış sonuçlar). Derleme hatası testleri: her yeni hata kodu için bir
negatif örnek.

### Bölüm 6 — FAZ 4 (DAP ENTEGRASYONU)
Önce RAPORLA (uygulamadan): DAP bugün interpreter ile mi JIT ile mi çalışıyor?
Breakpoint/step kancaları nerede? Tek thread varsayan yerler neler? Rapor sonrası
DUR.
Sonra uygula:
- Thread id: main = 1, diğerleri artan. Ad: "main", "thread#N @ dosya:satır".
- `threads` isteği canlı thread'leri döndürür. Thread açılış/kapanışında `thread`
  olayı (reason: started/exited).
- All-stop modeli: bir thread durduğunda (breakpoint, step, exception, pause)
  global duraklatma bayrağı set edilir; her isolate bir sonraki yoklama noktasında
  (geri kenar, satır kancası) park eder. pop/wait'te zaten bekleyen thread'ler
  durmuş sayılır; uyandıklarında kullanıcı koduna geçmeden bayrağı kontrol
  ederler. `stopped` olayı: tetikleyen threadId + `allThreadsStopped: true`.
- stackTrace/scopes/variables threadId'ye göre ilgili isolate'in frame ve heap'ini
  okur; YALNIZCA duraklatılmışken (güvenli).
- Ek bir "Shared" scope: shared primitif değerleri, Pool ve List uzunlukları.
- continue: hepsini devam ettirir. next/stepIn/stepOut: hepsini devam ettirir,
  istenen thread adım hedefine varınca yine all-stop (gdb scheduler-locking off).
  `supportsSingleThreadExecutionRequests: false`.
- Bloklanmış thread'in üst frame adı işaretlensin: "[bekliyor: pop jobs]".
- Debugger altında deadlock: süreci öldürmek yerine reason "exception" olan bir
  stopped olayı ve deadlock açıklaması.

### Bölüm 7 — HER FAZ SONU RAPOR FORMATI
- Değişen/eklenen dosyalar (kısa açıklamayla)
- Test sonuçları: normal / GC stres / TSan, interpreter / JIT
- Spesifikasyondan sapmalar ve nedenleri
- Açık sorular ve riskler (en fazla 10 madde)
- 600 kelimeyi geçme.

---

## C) Sonraki turlarda verilen kararlar

- **S1 (structMeta'ya runtime erişimi):** Isolate'e `const CompiledProgram*
  program` ekle (runOnIsolate RAII guard bağlar); sıcak yol için struct meta
  pointer'ını codegen'de `jitStructNew`'e **immediate argüman** olarak geç.
  ADR-045'e gömme kuralı eklendi (immutable + program-ömürlü; heap adresi asla).
- **S2 (CompiledProgram ömrü):** Sahibi **run komutu** (`run.hpp`'de
  `unique_ptr`); yıkım sırası: tüm thread'ler join → isolate'ler yıkılır →
  `MIR_gen_finish` → `MIR_finish` → CompiledProgram yıkılır. `activeIsolates`
  atomik sayaç; yıkıcı `assert(activeIsolates == 0)`. **ConstPool süreç-global
  değil, CompiledProgram üyesi** (program-ömürlü).
- **MIR değişmezi:** `MIR_link(ctx, MIR_set_gen_interface, ...)` → **eager**
  (mir_backend.cpp:3466); koşu sırasında hiçbir `MIR_*` çağrısı yok. "Koşu
  sırasında MIR'a dokunulmaz; çok thread aynı MIR_context'inde kod üretmez."
- **Risk 1 (rt() hot path):** `rt()` → `t_isolate->jit` (kontrolsüz) + debug'da
  `assert(t_isolate)`. Lazy oluşturma yalnız `Isolate::currentOrCreate()` (LSP,
  birim testleri). Benchmark: (a) öncesi commit ile (c2) sonrası iki sayı; fark
  %3'ü geçerse TLS erişim modelini kontrol et (initial-exec; `__tls_get_addr`
  görülürse raporla).
- **c1/c2 bölmesi:** (c1) **mekanik** bölme, davranış değişikliği YOK; sarmalayıcı
  = `compileProgram` → `IsolateGuard` → `runOnIsolate` → CompiledProgram yıkımı.
  Rapor `crossing-locals` tablosu içermeli: `değişken | eski kapsam | yeni yeri`.
  (c2) `rt()` → `t_isolate->jit`; `current()` assert'e döner + benchmark.
- **(d) öncesi:** `git status` ile `interpreter.hpp/.cpp`'de commit edilmemiş,
  sana ait olmayan değişiklik varsa DUR ve raporla; temizse devam.
- **(A) kararı:** decimal literal'leri `ConstPool::internDecimal` (değere göre
  tekilleştirilmiş, immortal); `LOAD_DECIMAL` ConstPool'dan; `jitBoxDecimal`
  codegen yolundan çıkar. Ön kontroller: (1) tek yerinde yazım `host_bridge.hpp:91`
  scratch owner (literal değil) → **BLOKLAMAZ**; (2) `DecimalObject` kimlik
  kullanımı yok. **Uygulandı** (`855aa34`).
- **Immutability kapsamı:** yalnız **paylaşılan** (ConstPool) nesneler için;
  **scratch** nesneler isolate'e özel olduğu ve dışarı sızmadığı sürece
  değiştirilebilir. ADR-045'e işlendi.
- **Yapısal korumalar:** tüm kod gömmeleri `embedProgramPtr` üzerinden (debug
  non-null assert); `compileProgram` hiçbir isolate bağlı değilken çalışır
  (c2'deki `rt()` assert'i bunu zorlar). ADR'ye yazıldı.

---

## D) Açık maddeler

1. **`static HostEnv jitEnv;`** (`src/mir/mir_backend.cpp:1251`) hâlâ
   süreç-global; **c1 içinde Isolate'e alınmalı.**
2. `embedProgramPtr` assert'i şimdilik yalnız **non-null**; `ConstPool::owns()`
   ile IRProgram-ömürlü veriyi (trace `fn.name`/`file`) kapsayan bir üyelik
   kontrolüne genişletilmeli (immortal kayıt defteri).
3. Decimal döndüren host fonksiyonu olmadığı için **decimal kaçış testi yok**;
   ilk decimal host fonksiyonu eklendiğinde yazılmalı.
4. **Benchmark** (a öncesi vs c2 sonrası) ve **TSan** henüz koşulmadı.
5. **CompiledProgram sahipliğinin `run.hpp`'ye taşınması ertelendi.**
6. Faz 2 Bölüm 4 madde 3'teki **native handle sorusu cevapsız** (dosya/soket
   temsili: int id mi, void* mi?).
7. ~~Çalışma ağacında başka bir aktörün commit edilmemiş değişiklikleri~~
   (`run.hpp`, `interpreter.*` vb.) **atıldı (2026-09-24)**; artık geçerli bir
   engel değildir. (d) öncesi `git status` kontrolü (Bölüm C) yine uygulanır.

---

## E) Sıradaki adım

**c1** (compileProgram/runOnIsolate mekanik bölmesi + crossing-locals tablosu;
structMeta codegen'de doğrudan CompiledProgram'a yazılır, runtime
`Isolate::current().program->structMeta`'dan okur — thread başına kopya yok)
→ **c2** → **c3** (ConstPool'un süreç-global tekilden CompiledProgram üyesine
taşınması; S2) → **d** → **e** → **f** → **g** → **h**; her adım ayrı commit, her
adımda `tests/run.sh` yeşil. Faz 1 sonunda tam rapor (Bölüm 7 formatı + MIR
arayüz kontrolü + benchmark). Faz 2'ye geçme.
