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
