# saQut derleyici tanı kodları — thread ekleri (ADR-045)

Bu belge ADR-045 (izole thread modeli) ile eklenen tanı kodlarını listeler.
Mevcut son kodlar `E013` / `W007` idi; yeni kodlar oradan devam eder. Önceki
kodların (E001–E013, E901–E907, W001–W007) tek kaynağı derleyici kaynağıdır
(`src/semantic/`, `src/symbol/`, `src/parser/`); bu belge onları yeniden
tanımlamaz.

Her kod için `tests/golden/threading_errors/` altında bir negatif örnek vardır
(`.compile_error` fixture'ı, bkz. `tests/run.sh`).

| Kod | Seviye | Anlamı | Tipik neden |
|-----|--------|--------|-------------|
| E014 | hata | `shared` / `Pool` / `List` bildirim kuralı ihlali | `shared` yerel bildirimde; shared tipi int/float/bool/Pool/List değil; `Pool(T)`/`List(T)` shared global başlatıcısı dışında; Pool/List değeri shared global adı dışında kullanıldı |
| E015 | hata | Tip gönderilebilir değil | Pool/List eleman tipi ya da `thread { }` yakalaması Pool/List/fonksiyon içeriyor |
| E016 | hata | Thread gövdesinde yakalanan değişkene atama | `thread { x = 5; }` — `x` çevreleyen fonksiyonun yereli; gövde bir kopya görür |
| E017 | hata | `lock` / `unlock` kuralı | hedef shared int/float/bool değil; aynı kilit ikinci kez alınıyor; tutulmayan kilit bırakılıyor |
| E018 | hata | `wait` koşulunda shared sembol yok | `wait(i > 3)` — koşul beklerken hiç değişemez |
| E019 | hata | `lock` kapsamı içinde `wait` (v1) | `lock a; wait(b > 0);` |
| W008 | uyarı | shared değişkende atomik olmayan güncelleme | `x = x + 1`, `x *= 2` (ayrı yükle + yaz); `+=` / `-=` / `++` / `--` atomiktir |
| W009 | uyarı | lock kapsamı içinde bloklayan çağrı | `lock a; jobs.pop();` — kilit tutulurken bekleme |

Çalışma zamanı hataları (derleme tanısı değil):

| Kod | Anlamı |
|-----|--------|
| `E_LIST_INDEX` | `List.get(i)` için `i < 0` ya da `i >= length()` |
| `E_LIST_FULL` | List kapasitesi (≈16,7 milyon eleman) doldu |
| deadlock | "runtime error: all threads are blocked (deadlock)" + her thread'in adı ve beklediği yer; çıkış kodu 70 |
