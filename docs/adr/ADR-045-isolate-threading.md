# ADR-045 — İzole Thread Modeli (Isolate Concurrency)

**Durum:** Kabul edildi  
**Tarih:** 2026-09-24  
**Karar verenler:** Ürün sahibi  
**İlişkili:** ADR-022 (GC çekirdeği), ADR-034 (FFI), ADR-037 (JIT value ABI),
ADR-038 (determinizm), ADR-042 (v1 kapsamı; §6 v1'de concurrency yoktur — bu
ADR v1 sonrası runtime modelini tanımlar), #116, #118, #222  
**Uygunluk raporu:** [docs/threading-feasibility.md](../threading-feasibility.md)

## Bağlam

v1.0 kapsamı concurrency'yi dışarıda bırakmıştır (ADR-042 §6). Bu ADR, v1
sonrası için benimsenen thread modelini kaydeder: **her OS thread'i kendi
izole yürütme ortamıdır (isolate)**. Paylaşımlı mutable heap ve global GC
reddedilmiştir; onun yerine "heap dışında yaşayan, heap pointer'ı içermeyen,
sınırda kopyalanan" bir paylaşım modeli seçilmiştir.

## Karar

### RUNTIME

1. Her thread bir **isolate**'tır: `std::jthread` üzerinde çalışır; kendi
   `Heap`'i, kendi GC'si, kendi shadow stack'i ve kendi **global değişken
   kopyası** vardır. Bir isolate başka bir isolate'in heap'ine asla dokunmaz.
2. GC'lerde **kilit yoktur, stop-the-world yoktur**; her isolate kendi heap'ini
   bağımsız toplar.
3. Kod **bir kez** derlenir; tüm thread'ler aynı makine kodunu çalıştırır.
   String sabitleri **program ömrü** boyunca yaşayan, immutable bir
   **ConstPool**'da tutulur; GC onu ne işaretler ne süpürür. ConstPool
   süreç-global değil, **CompiledProgram üyesidir** (aynı süreçte birden çok
   koşu olursa her program havuzunu kapanışta bırakır).
4. Bir thread uyumadan önce (pop/wait), son toplamadan bu yana eşiğin anlamlı
   bir kısmı kadar tahsis yapıldıysa GC çalışır.
5. Tüm bloklamalar **tek bir park/unpark katmanından** geçer; böylece ileride
   event loop eklenebilir.
6. **Koda gömülü adres kuralı:** bir adres makine koduna yalnızca işaret
   ettiği veri **immutable ise** VE **tüm isolate'lerden uzun yaşıyorsa**
   gömülebilir (program ömürlü veri). **Heap nesnesi adresi asla gömülmez.**
   ConstPool string/decimal literalleri ve `structMeta` bu kurala uyar.
   **Zorlama:** her gömme `embedProgramPtr` yardımından geçer (debug'da
   non-null assert); `compileProgram` hiçbir isolate bağlı değilken çalışır,
   dolayısıyla codegen sırasında yanlışlıkla bir heap erişimi olursa `rt()`
   assert'i patlar. **Immutability kapsamı:** bu şart yalnız **paylaşılan**
   (ConstPool) nesneler içindir; **scratch** nesneler isolate'e özel olduğu ve
   dışarı sızmadığı sürece değiştirilebilir (ör. `HostRetOwner::decimal`,
   `src/ffi/host_bridge.hpp:91`).
7. MIR derlemesi **eager**'dir (`MIR_set_gen_interface`); koşu sırasında hiçbir
   `MIR_*` API çağrılmaz. Birden çok thread aynı `MIR_context`'inde kod üretmez.

### SHARED VERİ

Tek ilke: **heap dışında yaşar, heap pointer'ı içermez, her erişimde sınırı
kopyalanarak geçer.**

6. `shared int/float/bool` → `std::atomic`; tekil okuma/yazma ve `+=` kilitsiz.
7. `shared Pool q = Pool(T);` → sınırlı, tipli FIFO kuyruk. `push` deep copy
   ile girer; `setMax` doluysa bloklar. `pop` boşsa bloklar ve alıcının
   heap'inde yeni nesne kurar.
8. `shared List l = List(T);` → append-only. `append` mutex altında; uzunluk
   atomik yayınlanır; `get(i)` kilitsiz ve kopyalayarak okur. Elemanlar
   chunk'larda sabit adreste kalır.
9. Thread'ler arası görünen **her şey** `shared` işaretlidir; diğer globaller
   thread başına ayrı kopyadır.

### MESAJ SEMANTİĞİ

10. Struct/array/string: **deep copy** (graph kopyası; içteki aliasing ve
    döngüler korunur). Native handle'lar (soket vb.): **sahiplik devri**;
    gönderendeki geçersizleşir.
11. İleri iş: Buffer için transfer (sıfır kopya) semantiği.

### DİL YÜZEYİ

12. `Thread t = thread { ... };` — gövde üst seviye bir fonksiyona dönüştürülür
    (lambda lifting). Yakalanan yereller başlangıçta kopyalanır; `shared`
    olanlar ve `Thread` handle'ları paylaşılır.
13. `Pool(T)` ve `List(T)` derleyici intrinsic'idir: argüman, ifade değil
    **tip adı** olarak çözülür. Jenerik sözdizimi yoktur.
14. `lock a;` / `lock a, b;` kapsam sonunda otomatik unlock; açık `unlock a;`
    da mümkündür. Çoklu kilitler sabit sırayla alınır (`std::scoped_lock`
    gibi).
15. `wait(koşul);` koşul doğru olana kadar bekler. Koşulda **en az bir `shared`
    sembol** olmak zorundadır, yoksa derleme hatası. Runtime, koşuldaki shared
    sembollerde değişiklik olduğunda bekleyeni uyandırıp koşulu yeniden
    değerlendirir.
16. `t.stop()`: pop/wait gibi bloklayan noktalar iptal noktasıdır; thread orada
    temiz çıkar, kapsamdaki kilitler bırakılır.
17. `main` dönünce açık thread'ler beklenir. Tüm yaşayan thread'ler
    bloklanmışsa **"all threads are blocked"** hatası (deadlock dedektörü).
18. Yakalanmayan bir hata tüm süreci durdurur (stack trace ile). `print` satır
    bazında atomiktir.

### Isolate alanları (Faz 1, kapsayıcı)

Her `Isolate` en az şunları taşır: `JitRuntime jit`, `ShadowStack shadow`,
string-heap kancası, RNG durumu, `const CompiledProgram* program` (koşu
raundunda `IsolateGuard` bağlar), ve ileriki adımlarda `Heap` + `globalSlots`.
Oluşturma açıkça `Isolate::currentOrCreate()` (LSP, birim testleri); koşu yolu
RAII guard ile bağlanır ve `current()` guard'lı yolda assert'e döner.

### CompiledProgram ömrü

`CompiledProgram` MIR context'ini (opaque), fonksiyon giriş tablosunu,
`structMeta`'yı ve ConstPool'u taşır. Sahibi **run komutudur**. Yıkım sırası
sabittir: tüm thread'ler join -> tüm isolate'ler yıkılır -> `MIR_gen_finish` ->
`MIR_finish` -> CompiledProgram yıkılır. Aktif koşan isolate sayısı atomik
sayaçla izlenir; yıkıcı `activeIsolates == 0` bekler.

## Reddedilen alternatifler

- **Multiprocess + shared memory:** GC nesneleri pointer içerir; process'ler
  arasında taşınamaz.
- **Paylaşımlı heap + global GC:** kilitli allocator, stop-the-world, veri
  yarışı.

## Ertelendi

- **ucontext green thread'leri:** Reddedilmedi. Kullanıcının `threading/`
  altındaki ucontext denemesi 2026-09-24'te kaldırıldı; yaklaşım ileride bir
  isolate içinde M:1 hafif thread katmanı olarak değerlendirilebilir.

## Fazlar

1. **Isolate refactor'u** — dil değişikliği yok.
2. **Runtime primitifleri** — Pool/List çekirdekleri, serialize/deserialize,
   park/unpark, deadlock sayaçları; C++ birim testleri + TSan.
3. **Dil yüzeyi** — `thread`, `shared`, `lock`/`unlock`, `wait`, `Pool`/`List`.
4. **Buffer transferi** — sıfır kopya semantiği.

## Sonuçlar

- Thread izolasyonu, GC'yi tek-thread'li mark-sweep olarak korur; eşzamanlı/
  paralel GC yazılması gerekmez.
- Paylaşım tek bir ilkeye indirgenir: heap dışında yaşam, heap pointer'ı
  taşımama, sınırda kopyalama.
- Determinizm, verili girdi sırasına göre korunur; dil yüzeyi açık ve
  isteğe bağlıdır.

## Uygulama durumu

**Tasarlandı; Faz 1 kısmen uygulandı.** v1.0.1-multithread dalında tamamlanan
Faz 1 dilimleri: Isolate temeli + dağınık thread_local'ların birleştirilmesi
(`07dcc04`), JitRuntime'ın Isolate üyesi olması ve `rt()` erişimi (`1bcc081`),
ConstPool + `immortal` işaretleme (`0503b4f`), `Isolate::program` alanı +
IsolateGuard (`9b468ae`), decimal literal'lerin ConstPool'a taşınması +
`embedProgramPtr` (`855aa34`), compileProgram/runOnIsolate bölmesi + structMeta
CompiledProgram'da (`9c61a78`, c1), `rt()` sıcak yolu + `current()` assert'i +
isolate'siz derleme (`033f9bc`, c2), ConstPool CompiledProgram üyesi +
gömme üyelik assert'i (`bddab15`, c3), CompiledProgram sahipliği CLI
komutlarında (`a7e6c71`, c4), Interpreter isolate bağı + salt okunur
IRProgram (`df884c1`, d), FileRegistry freeze (`e2c0848`, e), print mutex'i
(`5fecf75`, f), `__init_globals` üreticisi (`ff30dac`, g — Plan B), JIT GC
sayaç global'leri (`f538915`), isolate eşzamanlılık testi (`03a7f32`, h).

Faz 2 (runtime primitifleri, `src/runtime/threading/`): ThreadTable + park
katmanı + deadlock dedektörü (`e82e37f`), yapısal mesaj serileştirme
(`cfe05fd`, Plan B), native handle araştırması (`e909197`), PoolCore
(`1458c88`), ListCore (`badd174`), SharedSlots (`7f208e6`), birim testleri
(`6d968a1`). Faz 3 (dil yüzeyi): lexer/parser (`e3f5e05`), semantik
(`46f8c35`), IR opcode'ları + lambda lifting (`6c3ab80`), Interpreter
(`c5d2140`), JIT (`0b8eded`), örnekler/negatif testler/rehber (`093459a`).
Faz 4 (DAP): thread listesi/olayları, all-stop, işçi inceleme, Shared scope
(`19b99aa`, breakpoint/step yalnız main — Plan B). Debug assert ölüm
testleri (`1944305`).

**Durum: Uygulandı + Test Edildi (dal içinde), Release Edilmedi.** Toplu
doğrulama (handoff Bölüm 7.1) koşuldu: Release ve Debug `tests/run.sh`
yeşil, Debug assert ölüm testleri 8/8, GC stres (VM+JIT) yeşil, TSan
(primitifler, isolate testi, tüm `examples/threading` VM+JIT) 0 uyarı,
negatif testler yeşil, `saqut bench`/`--profile` ve DAP thread senaryosu
duman testleri geçti. Tek thread VM'de heavy benchmark'ta ~%5–9 gerileme
ölçüldü (JIT'te yok); ayrıntı ve açık soru `docs/threading-decisions.md`
"Bölüm 7".

Global başlatma kararı (Plan B, `docs/threading-decisions.md` [1-g]):
main'in başındaki global init prelude'u korunur; thread kullanan programlarda
IRGenerator ayrıca `__init_globals()` (shared olmayan globaller) üretir ve her
yeni thread'in isolate'i gövdeden önce onu çağırır. **Global initializer'ların
yan etkileri thread başına yeniden çalışır.** shared globaller main thread'inde
spawn'dan önce bir kez kurulur.

Otonom uygulama turunun kararları ve Plan B geçişleri
`docs/threading-decisions.md`'dedir; sıradaki adımlar devir belgesi
`docs/threading-handoff.md` Bölüm E'de. Faz 1–4
sonuna kadar, kanıt üretilmeden karar "Uygulandı" veya "Test Edildi" sayılamaz.

## Doğrulama

- Faz 2 için C++ birim testleri ve TSan koşusu (Pool/List, park/unpark,
  deadlock sayaçları).
- Faz 3 için: `shared` görünürlüğü, `wait` koşul kuralı (shared zorunluluğu),
  `lock` sıra kuralı, `t.stop()` temiz çıkış.
- `main` dönüşünde açık thread bekletme ve deadlock hatası senaryoları.
- İzolasyon kanıtı: bir isolate'in diğerinin heap'ine dokunmadığı ve GC'lerin
  kilitsiz/STW'siz çalıştığı gösterilmeli.
