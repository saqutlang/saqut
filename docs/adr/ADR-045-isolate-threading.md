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
   String sabitleri süreç ömrü boyunca yaşayan, **immutable bir ConstPool**'da
   tutulur; GC onu ne işaretler ne süpürür.
4. Bir thread uyumadan önce (pop/wait), son toplamadan bu yana eşiğin anlamlı
   bir kısmı kadar tahsis yapıldıysa GC çalışır.
5. Tüm bloklamalar **tek bir park/unpark katmanından** geçer; böylece ileride
   event loop eklenebilir.

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

## Reddedilen alternatifler

- **Multiprocess + shared memory:** GC nesneleri pointer içerir; process'ler
  arasında taşınamaz.
- **Paylaşımlı heap + global GC:** kilitli allocator, stop-the-world, veri
  yarışı.

## Ertelendi

- **ucontext green thread'leri:** Reddedilmedi. `threading/` altındaki deneme
  olduğu gibi kalır; ileride bir isolate içinde M:1 hafif thread katmanı
  olarak değerlendirilebilir.

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

**Tasarlandı.** Hiçbir dil yüzeyi veya runtime primitifi uygulanmamıştır
(v1.0.1-multithread dalı). `threading/` altındaki ucontext denemesi bu ADR'nin
dışında, bağımsız bir çalışmadır. Faz 1–4 tamamlanıp test kanıtı üretilmeden
karar "Uygulandı" veya "Test Edildi" sayılamaz.

## Doğrulama

- Faz 2 için C++ birim testleri ve TSan koşusu (Pool/List, park/unpark,
  deadlock sayaçları).
- Faz 3 için: `shared` görünürlüğü, `wait` koşul kuralı (shared zorunluluğu),
  `lock` sıra kuralı, `t.stop()` temiz çıkış.
- `main` dönüşünde açık thread bekletme ve deadlock hatası senaryoları.
- İzolasyon kanıtı: bir isolate'in diğerinin heap'ine dokunmadığı ve GC'lerin
  kilitsiz/STW'siz çalıştığı gösterilmeli.
