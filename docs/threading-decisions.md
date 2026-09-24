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
