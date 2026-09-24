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

## Faz 2

- **[2-a+g+h]** Üç madde tek commit: ThreadTable'ın yaşam döngüsü (canlı
  sayacı, bitişte uyandırma) park katmanını, park'ın deadlock raporu
  ThreadTable'ı çağırır; dedektör park döngüsünün içindedir. | Her commit
  derlenmeli/bağlanmalı kuralı (OBJECT kütüphanesi tüm nesneleri bağlar). |
  Sahte ara arayüz (callback) ile üç ayrı commit.
- **[2-a]** Thread durdurma için `ThreadCore::stopSource` (kendi
  `std::stop_source`'u) kullanılır, jthread'in iç stop kaynağı değil. |
  jthread kurucusu thread'i hemen başlatır; gövde jthread nesnesine erişirse
  atamayla yarışırdı. jthread yine OS thread'ini taşır ve yıkımda join eder.
  | Gövdeye jthread'in stop_token'ını parametre olarak geçmek (DAP/park
  tarafında token'a thread dışından erişim zorlaşırdı).
- **[2-a]** saQut `t.join()` = park ile "hedef finished" beklemesi; OS
  seviyesinde `std::jthread::join` yalnız `joinAll` (main dönüşü) ve tablo
  yıkıcısında. | Aynı handle'ı birden çok thread join edebilir; OS join'i
  tek çağırana izin verir. | —
- **[2-g]** Park modeli: tek mutex + tek `condition_variable_any` + epoch;
  pred park mutex'i altında değerlendirilir ve yan etkili olabilir (Pool'da
  "kuyruktan al"). `notifyShared` epoch'u mutex altında artırır → kayıp
  uyandırma yok. Stop, `condition_variable_any::wait(lock, stop_token, pred)`
  ile uyandırır. Park öncesi GC bir kanca (`setBeforeParkHook`) ile bağlı
  isolate'in heap'inde `Heap::collectBeforePark()` (son toplamadan beri
  tahsis ≥ eşik/2). | ADR-045 §RUNTIME 5 "tek park katmanı". | —
- **[2-h]** Dedektör: `parked` sayacı her `notifyShared`'de sıfırlanır (tüm
  bekleyenler pred'i yeniden deneyecek, "uyanık" sayılır) ve her bekleyen
  yeniden kayıt olur; `parked >= live` ⇒ deadlock. Bu, "bildirildi ama henüz
  mutex'i geri almadı" yarışındaki yanlış pozitifi önler. Varsayılan
  işleyici raporu stderr'e yazıp `_Exit(70)` (statik yıkıcılar bloklu
  thread'leri join etmeye çalışıp asılırdı). **PLAN B (önlem):**
  `SAQUT_NO_DEADLOCK_DETECT=1` dedektörü kapatır; varsayılan açık.
  `lock` (std::mutex) beklemesi dedektörde sayılmaz ve stop ile kesilemez
  (bilinen kısıt; `lock a, b` sıralı alındığı için kilit-kilit deadlock'u
  statik olarak önlenir). | — | Sembol başına bekleyen listeleri (TODO).
- **[2-c] PLAN B** Serileştirme tip yönlü değil **yapısal**:
  `serialize(const Value&, MessageBuffer&)` / `deserialize(MessageBuffer&,
  Heap&)`. Value kind'ı + nesne başlığı (Array+elemKind / Struct / String /
  Decimal) yeterli; runtime tip tablosu gerekmedi. Graph kopyası: ilk görüşte
  nesneye indeks, sonra geri-ref (aliasing + döngü); deserialize nesneyi
  çocuklarından önce indeksler. Struct alan adları (`fieldNames`, değişmez
  `shared_ptr`) mesajda paylaşımlı referans olarak taşınır — heap nesnesi
  değil, refcount atomik. String için `Value::fromStringObject` fabrikası
  eklendi (alıcı heap'e doğrudan tahsis). Özyinelemeli; çok derin bağlı
  yapılarda native yığın riski (bilinen kısıt). | Tip bilgisi zaten derleme
  zamanında gönderilebilirlik denetimi için kullanılıyor; runtime'da nesne
  başlığı eşdeğer bilgiyi taşıyor. | Tip yönlü serileştirme + CompiledProgram
  tip tablosu.
- **[2-d] Araştırma sonucu (handoff D6 cevabı):** v1 FFI'de **native handle
  yoktur.** `src/ffi/functions/fs.cpp` başlığı: "Handle/descriptor YOK — tek
  atımlık read/write (ADR-034 §5)"; `io.cpp`: "handle'sız model"; soket
  modülü yok. Host sınırındaki tüm türler değerdir (`HostSlot`: Int/LongInt/
  Date/Float/Float32/Str/Decimal/Ref/Null, `src/ffi/host_abi.hpp:86-107`);
  dosya içeriği `byte[]` olarak taşınır. **Karar:** v1'de sahiplik devri
  kuralının uygulanacağı bir tür yok; FFI'nin ürettiği tüm değerler olağan
  deep copy ile gönderilebilir. İleride handle eklenirse prompt'taki karar
  tablosu uygulanır (süreç-global kilitli tabloya int id → kopyalanır ve
  paylaşılır; isolate'e özel tablo → sahiplik devri; ham void* → Faz 3'te
  "type X is not sendable"). | Kanıt koddan. | —
- **[2-e]** PoolCore'un kendi `notEmpty/notFull` condition variable'ları
  **yok**; push/pop park katmanından geçer (pred = kuyruk mutex'i altında
  `tryPush`/`tryPop`, park mutex'i → kuyruk mutex'i sabit kilit sırası).
  Başarılı push/pop/setMax `notifyShared` yapar. | ADR-045 §RUNTIME 5 "tüm
  bloklamalar tek park katmanından" (ADR prompt'taki uygulama detayından
  önce gelir) ve deadlock dedektörünün pop/push beklemelerini görebilmesi
  için tek sayaç noktası gerekir. | Spec'teki gibi Pool başına iki CV +
  dedektöre ayrı kayıt.
- **[2-c/2-f]** `deserialize` artık `const MessageBuffer&` alır (okuma imleci
  yerel); List elemanları kilitsiz ve eşzamanlı okunabildiği için mesaj
  değişmez olmalı.

## Faz 3 — mimari (uygulama başlamadan)

- **[3-c] Lambda lifting yeri:** ayrı bir AST geçişi yerine **IRGenerator
  içinde** yapılır. SymbolCollector `thread { }` gövdesinin yakaladığı
  yerelleri (thread gövdesi kapsamı dışında, global olmayan semboller)
  `ThreadExprNode::captures`'a yazar ve yakalanan adlara atamayı işaretler
  (TypeChecker hata verir). IRGenerator `thread {}` gördüğünde gövdeyi
  `__thread_<fn>_<n>` adlı **0 parametreli** sentetik fonksiyona üretir:
  başında `CALL __init_globals` (debugHidden), ardından her yakalanan için
  `THREAD_ARG i` → yerel slot. Çağıran taraf `THREAD_SPAWN` ile yakalanan
  slotları tek bir mesaja serileştirir. | IRGenerator zaten ad→slot/kapsam
  haritasını tutuyor; AST geçişi kapsam çözümünü çoğaltırdı. Sabit 0-arite
  giriş JIT'te değişken imzalı fonksiyon işaretçisi çağırma sorununu
  ortadan kaldırır (VM ve JIT aynı yoldan). | Spec: ayrı
  `thread_lifting.cpp` AST geçişi + yakalananları parametre olarak alan
  sentetik fonksiyon.
- **[3-b/3-d] `wait` predicate'i:** `__wait_pred_<n>` fonksiyonu yerine
  döngü: `e = SHARED_EPOCH; c = <koşul>; JIF_TRUE c → son; WAIT e; JMP başa`.
  Epoch koşuldan ÖNCE okunduğu için arada olan mutasyon kaçmaz (WAIT,
  epoch `e`'den farklıysa hemen döner). | Aynı semantik, callback yok (JIT
  kodunu C++'tan geri çağırma gerekmez), koşuldaki yereller doğal olarak
  erişilir. | Spec'teki sentetik predicate fonksiyonu + runtime park(pred).
- **[3-e/3-f] stop (iptal):** VM'de bloklayan opcode stop ile dönerse (ya da
  geri kenar yoklaması stop bitini görürse) `ThreadStopRequested` C++
  istisnası atılır ve thread girişinde yakalanır — saQut `try/catch`
  VM-içi olduğundan bunu yakalayamaz. JIT'te stop, özel bir "stop"
  hatası olarak `pendingError` üzerinden yayılır; thread programlarında
  catch dallarının önüne "durduruluyorsa catch'e girme, yay" kontrolü
  eklenir. Yoklama ve bu kontroller yalnız thread kullanan programlarda
  üretilir (tek-thread VM/JIT kodu ve performansı değişmez).
- **[3-h] Kilitler:** isolate başına tutulan-kilit yığını. Sözcüksel
  çıkışlar (`return`/`break`/`continue` ve blok sonu) IRGenerator'ın ürettiği
  `UNLOCK`'larla; VM'de catch'e unwind edilirken try içinde alınmış
  kilitler bırakılır; thread sonunda (normal/stop/hata) kalan tüm kilitler
  bırakılır. JIT'te catch'e unwind edilirken kilit bırakma YOK (**PLAN B**,
  bilinen kısıt — thread sonunda yine bırakılır).

## Faz 3 — uygulama

- **[3-c+3-d]** Tek commit (`6c3ab80`): lifting yeni opcode'ları
  (THREAD_SPAWN/ARG) üretmeden var olamaz. Spec listesine ek opcode'lar:
  `SHARED_EPOCH` (wait döngüsü), `THREAD_ARG` (yakalanan argüman).
  `SHARED_RMW`'nin işlem kodu `int64Value`'da (liveness `left`'i okunan slot
  sayar). shared `++`/`--` de atomik RMW'dir (spec yalnız `+=`/`-=`
  diyordu; `x++`'ı atomik olmayan yükle+yaz yapıp uyarmak yerine doğal
  atomik semantik seçildi). saQut `float`'ı 32-bit olduğu için shared float
  slotu `SlotType::Float32`. `unlock a;` derleme zamanı kilit kaydını
  silmez; blok sonu yine UNLOCK üretir, runtime tutulmayan kilidi bırakmayı
  etkisiz sayar (yol-bağımlı açık unlock'ta sızıntı olmaz). Pool/List'e
  giren int değer eleman tipine (float/double/decimal/longint) açıkça
  genişletilir.
- **[3-e]** VM: spawn = `ThreadTable::spawn` + yeni thread'de kendi
  `Isolate`'i ve kendi `Interpreter`'ı (IRProgram paylaşılır); başlangıç
  mesajı alıcı heap'e açılır ve `threadArgs_` GC köküdür. Bloklayan opcode
  stop ile dönerse `ThreadStopRequested` C++ istisnası; thread girişi yakalar.
  Thread'de yakalanmayan hata: "runtime error in thread#N @ f:l: msg" +
  `_Exit(1)`. main'de yakalanmayan hata (thread programı): mesaj +
  `_Exit(70)` (statik yıkım koşan thread'leri beklerken asılırdı). Ana
  Interpreter program başını (`programBegin`) initForDebug'da kurar.
- **[3-f] JIT tam destek (Plan B gerekmedi, doğrulama Bölüm 7'de):** tüm
  thread opcode'ları `rt_jit_*` trampolinlerine iner (değer türüne göre
  i/d/p varyantları; Float32 hedef D2F; nullable hedef için
  `rt_jit_thread_last_null`). Spawn: `rt_jit_spawn_begin/arg_*/commit`
  (değişken sayıda argüman sabit tampona yazılır, commit tek mesaja
  serileştirir); giriş fonksiyonu `CompiledProgram::entries`'ten, yeni
  thread `runOnIsolate(..., entryName, startMsg)` ile aynı makine kodunu
  koşar. Stop: ölümsüz bir "stop nöbetçisi" `pendingError`'a konur; thread
  programlarında her catch dalının önüne `rt_jit_error_catchable` kontrolü
  üretilir (nöbetçi catch'e girmez, yayılır); `runOnIsolate` thread
  girişinde nöbetçiyi temiz çıkış sayar. Mesaj değerleri JIT kuralıyla
  (int → LongInt Value, float → Float Value) üretilir; aynı programın tüm
  thread'leri aynı backend'de koştuğundan tutarlıdır.
- **[3-g]** Geri kenar yoklaması: VM'de `pollFlags_` bağlıyken (yalnız
  thread programı) geri JMP/JIF'lerde; JIT'te thread programlarında geri
  atlamalardan önce `rt_jit_poll` (stop → nöbetçi + yay). Tek thread
  programlarında kod ve performans değişmez. Ayrı commit yok: VM kısmı
  `c5d2140` (3-e), JIT kısmı 3-f commit'inde. bit1 (DAP duraklatma) Faz 4.
- **[3-h]** Ayrı commit yok; parçaları: sözcüksel UNLOCK'lar (3-d,
  `6c3ab80`), VM catch unwind'da try-etiketli kilit bırakma ve thread
  sonunda tümünü bırakma (3-e, `c5d2140`), JIT thread sonunda bırakma
  (3-f). JIT'te catch'e unwind'da bırakma yok (**PLAN B**, [3-h] mimari
  kaydı). `lock` beklemesi (std::mutex) stop ile kesilemez ve deadlock
  dedektöründe sayılmaz (bilinen kısıt).

## Faz 4 — DAP

- **[4 araştırma]** DAP yalnız **VM** ile çalışır (`src/dap/dap_handler.cpp`,
  `vm_ = std::make_unique<Interpreter>`); ana thread'in Interpreter'ı DAP
  thread'inde bütçeli `runUntilEvent` turlarıyla koşar. Breakpoint/adım
  kancaları Interpreter'dadır (`isBreakpoint`, `stepStartLine_`,
  `runUntilEvent`). Tek thread varsayımları: `threadId: 1` sabitleri,
  `threads` yanıtında yalnız main, `vm_` tekilliği, `evaluate`'in yalnız
  `vm_`'e bakması, frame id = derinlik. İşçi thread'ler DAP'ın dışında
  kendi `Interpreter::run()`'larıyla koşar.
- **[4] All-stop:** ana thread durduğunda (breakpoint/adım/pause/entry/
  exception) DAP tüm işçilere `kDebugPause` bitini koyar; VM işçisi bir
  sonraki geri kenarda ya da bloklayan bir çağrıdan (pop/push/wait/join)
  dönüşte, kullanıcı koduna geçmeden park eder (deadlock'a sayılmaz).
  `stopped` olayı `allThreadsStopped: true` taşır. continue/next/stepIn/
  stepOut önce bitleri kaldırır (hepsi sürer); `supportsSingleThreadExecution
  Requests: false`.
- **[4] Thread listesi/olaylar:** `threads` = main + canlı işçiler
  ("thread#N @ dosya:satır"); `thread` started/exited olayları DAP
  thread'inde her koşu turu ve durma noktasında ThreadTable farkından.
- **[4] İnceleme:** frame id = derinlik (main) / `T*100 + derinlik` (işçi
  T). İşçinin çerçeveleri yalnız park'tayken (duraklatılmış ya da
  bloklanmış) okunur; bloklanmış thread'in üst frame adı
  "[bekliyor: pop jobs]". Park'ta değilse tek bir yapay frame
  ("[çalışıyor]"). Ek "Shared" scope: shared primitifler ve Pool/List
  uzunlukları.
- **[4] PLAN B (kısmi):** breakpoint'ler ve adımlama yalnız **ana
  thread**'de (işçi thread'de breakpoint tetiklenmez; threadId≠1 adım
  isteği hata yanıtı). `evaluate` yalnız ana thread çerçevelerinde. Tam
  per-thread breakpoint/step her işçi Interpreter'ının DAP olay döngüsüne
  bağlanmasını gerektirir (ayrı tur). Ana-thread DAP davranışı birebir.
- **[4] İşçi çıktısı:** işçilerin `print`'i protokol akışına doğrudan
  yazmaz (DAP thread'inin yazdığı çerçevelerle karışırdı); kuyruğa girer,
  DAP thread'i tur aralarında `output` olayı olarak boşaltır.
- **[4] Deadlock (debugger altında):** park katmanının işleyicisi süreci
  öldürmek yerine `stopped` (reason "exception", açıklama + rapor) gönderir
  ve istekleri yanıtlamaya devam eder; ilerleme istekleri reddedilir,
  terminate/disconnect süreci 70 ile sonlandırır.
- **[4] Yaşam döngüsü:** main bitince işçiler beklenir (CLI ile aynı);
  terminate/disconnect'te işçilere stop istenir ve join edilir (IRProgram
  yıkılmadan önce).
