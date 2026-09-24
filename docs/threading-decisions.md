# Threading karar günlüğü — `v1.0.1-multithread`

**İlişkili:** [ADR-045](adr/ADR-045-isolate-threading.md),
[devir belgesi](threading-handoff.md)

Biçim: `[madde] karar | gerekçe | alternatif`. Plan B'ye geçilen her durum
**PLAN B** etiketi taşır. Bu günlük otonom uygulama turunun final raporunun
omurgasıdır; kararlar ürün sahibinin onayına açıktır.

## Faz 1

- **[1-c2]** Ana thread isolate'i `main.cpp` girişinde
  `Isolate::currentOrCreate()` ile bağlanır; `Isolate::current()` ve `rt()`
  assert'li kontrolsüz TLS okumasına döndü. | c2 `current()`'ı assert'e
  çeviriyor; VM/LSP/DAP yolları da isolate kullanır (string heap kancası,
  shadow stack, RNG), hepsine ayrı guard koymak yerine ana thread'in süreç
  boyu isolate'i en küçük diff. | Her komut yoluna (`run`, `exec`, `bench`,
  `lsp`, `dap`) ayrı `IsolateGuard`.
- **[1-c2]** `compileProgram` `NoIsolateScope` ile isolate'i kapsam boyunca
  ayırır (t_isolate = nullptr). | ADR-045 §RUNTIME 6 "compileProgram hiçbir
  isolate bağlı değilken çalışır" zorlaması; çağıranın ana isolate'i bağlı
  olsa bile. | Çağıranlara "compile'dan önce guard açma" sözleşmesi (zorlanamaz).
- **[1-c3]** ConstPool `CompiledProgram::constPool` oldu (Plan B gerekmedi:
  runtime'dan ConstPool'a erişen yol yok). Trace çerçevesinin `fn.name`/dosya
  C-string'leri artık IRProgram'dan değil `CompiledProgram::programStrings`
  (deque, sabit adres) kopyalarından gömülür. `embedProgramPtr` debug'da
  `CompiledProgram::ownsEmbeddable` (unordered_set kaydı) ile üyelik
  doğrular. | Gömülü her adres tek bir sahibe (CompiledProgram) bağlanır;
  IRProgram ömrüne bağımlılık kalkar; O(1) denetim. | IRProgram adres
  aralığı kaydı (IRProgram'ın dağınık std::string'leri için aralık yok).
- **[1-c4]** `run`, `exec` ve `bench` komutları `compileProgram` →
  `IsolateGuard` → `runOnIsolate` dizisini kendileri kurar; `unique_ptr`
  sahipliği komuttadır. Bench için Plan B gerekmedi (tekrar döngüsü zaten
  `runOnIsolate`'in `executionRuns` parametresinde). Bench'te
  `compiled.reset()` ölçüm aralığının içinde tutuldu (eskiden MIR_finish
  `tryCompileAndRunProgram` içindeydi). `tryCompileAndRunProgram` API olarak
  kaldı, CLI kullanmıyor. | Sahiplik ADR-045 "CompiledProgram ömrü"ne göre
  koşu komutunda. | Yalnız run.hpp'yi taşımak (Plan B).
- **[1-d] PLAN B** Interpreter `heap_`/`globalSlots_` üyelerini korur;
  kurucu bağlı isolate'e `Isolate::heap`/`Isolate::globalSlots` işaretçilerini
  bağlar, yıkıcı önceki bağı geri koyar. `Interpreter` artık
  `const IRProgram&` tutar (`IRProgram::findFunction` const aşırı yüklemesi
  eklendi); koşu sırasında IRProgram'ı değiştiren yol bulunmadı (yalnız iki
  `findFunction` çağrısı const olmayan pointer alıyordu). | Interpreter
  heap'i bir RootSource olarak kendine kaydediyor ve DAP/bench API'leri
  üyelere doğrudan bakıyor; sahipliği Isolate'e almak bu API'leri gereksiz
  yere değiştirirdi. Thread başına bir Interpreter olduğundan izolasyon
  aynıdır. | Heap/globalSlots'un sahibi Isolate, Interpreter referans tutar.
- **[1-e]** FileRegistry'ye `freeze()`; `run` ve `exec` IR üretiminden sonra
  çağırır; sonrasında YENİ yol kaydı debug'da assert (kayıtlı yolun aranması
  serbest). Koşu sırasında FileRegistry'ye yazan yol bulunmadı (vm/mir/ffi
  SourceLocation üretmiyor) → Plan B (shared_mutex) gerekmedi. LSP ve bench
  dondurmaz (süreç içinde tekrar derler). FfiCatalog'a freeze eklenmedi:
  kurucudan sonra mutatörü yok, `instance()` const döner — tip zaten donuk;
  ilk erişim magic-static ile thread-safe. | En küçük diff, gerçek risk
  (koşuda yeni kayıt) assert'le yakalanır. | FfiCatalog'a boş bir freeze().
- **[1-f]** Tek süreç-global `programOutputMutex()` (`runtime/output_lock.hpp`);
  `print` (CORE_PRINT), `stdout/stderr::write`, `writeBytes` ve JIT
  `rt_jit_print_*` trampolinleri metni önce üretip kilit altında tek parça
  yazar. print satır sonu eklemediği için atomiklik birimi "çağrı"dır.
  stdout ve stderr aynı kilidi paylaşır; DAP output sink'i de kilit altında
  çağrılır. | Tek kilit sıralama sorununu ve iki akış arası ara-kesimi
  önler; print sıcak yolu değil. | Akış başına ayrı mutex.
- **[1-g] PLAN B (baştan)** main'in başındaki global init prelude'u
  olduğu gibi kaldı (tek thread IR'ı, `ir` golden'ları, DAP adımları ve stack
  trace'ler birebir). Thread kullanan programlar için IRGenerator ayrıca
  `__init_globals()` üretir (shared olmayan globaller; debugHidden;
  `needsThreadGlobalInit_` bayrağı Faz 3'te `thread {}` görülünce set edilir,
  aksi hâlde hiçbir şey üretilmez). `__init_shared` ayrı fonksiyon olarak
  üretilmez: shared globaller main'in prelude'unda, main thread'inde, bir kez
  (spawn'dan önce) kurulur — spec'teki "süreç başında bir kez, main
  thread'inde" semantiğinin aynısı. İki init döngüsü tek
  `emitGlobalInitializers` yardımcısına indirildi. `VariableDeclNode::isShared`
  eklendi. | Spec'in birinci seçeneği tüm tek-thread IR dump'larını
  değiştirirdi; prompt tek thread regresyonunu mutlak öncelik sayıyor. |
  Spec: init'i main'den `__init_shared`/`__init_globals`'e taşıyıp çağırmak.
- **[1-h ön]** Paylaşılan mutable yapı taraması (Bölüm 0 "dur ve raporla"
  sınıfı, otonom modda düzeltildi): `lastRunGcStatsStorage` süreç-global
  `static` idi ve her JIT koşusu sonunda yazılıyordu → `thread_local`.
  `gcThresholdForNextRunStorage` → `std::atomic<int>` (ana thread yazar,
  isolate'ler okur). `gMaxCallDepth` yalnız argüman ayrıştırmada yazılıyor
  (salt okunur). Codegen'deki `static int` sayaçlar yalnız tek thread'li
  derlemede kullanılıyor. | En küçük diff. | Her ikisini Isolate alanı yapmak.
- **[1-h]** C++ testleri için CMake'te `main.cpp` dışındaki kaynaklar
  `saqut_core` OBJECT kütüphanesine alındı (statik arşiv kullanılmayan
  nesneleri düşürebilirdi); `saqut` ve testler ona bağlanır. Threading
  testleri `EXCLUDE_FROM_ALL` + `saqut_threading_tests` hedefi; varsayılan
  build ve `tests/run.sh` değişmez. `Threads::Threads` saqut_core'a bağlandı.
  Test karşılaştırması dönüş değeri (checksum) + VM'de thread başına output
  sink; JIT print'i doğrudan stdout'a gittiği için JIT'te yalnız dönüş
  değeri karşılaştırılır. | Tam derleyici hattına bağlanmanın tek temiz yolu;
  run.sh'deki tek-dosya g++ derlemesi tüm kaynakları elle saymayı
  gerektirirdi. | Gizli bir CLI alt komutu (CLI yüzeyi ürün kararı).
