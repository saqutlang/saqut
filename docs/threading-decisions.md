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
