# saQut'ta thread'ler — kısa rehber (ADR-045)

> **Durum:** `v1.0.1-multithread` dalında uygulanmış bir **önizlemedir**; v1.0
> sözleşmesinin parçası değildir (ADR-042 §6). VM normatif backend'dir, JIT
> `[EXPERIMENTAL]`.

## Model: her thread ayrı bir dünya

Her `Thread` kendi **isolate**'inde çalışır: kendi heap'i, kendi GC'si, kendi
global değişken kopyası. Bir thread başka bir thread'in nesnelerine asla
dokunmaz. Thread'ler arasında veri yalnız iki yolla geçer:

1. **`shared` globaller** — `int`, `float`, `bool` (atomik) ve `Pool` / `List`.
2. **Kopya** — `thread { }` gövdesinin yakaladığı yereller ve Pool/List'e
   konan her değer **derin kopyalanır** (struct/dizi grafı, içteki paylaşımlar
   ve döngüler korunarak).

## Thread başlatmak

```saqut
int main() {
    int n = 5;
    Thread t = thread {
        print(n);          // n'nin kopyası
    };
    t.join();
    return 0;
}
```

- `thread { ... }` bir ifadedir, tipi `Thread`'dir ve **hemen başlar**.
- Gövde, çevreleyen fonksiyonun yerellerini **başlangıçtaki değerleriyle
  kopya** olarak görür. Gövdede yakalanan bir değişkene atama derleme hatasıdır
  (E016) — bir kopyayı değiştirmek dıştakini değiştirmez.
- Gövdede `return;` thread'i bitirir. İç içe `thread { }` desteklenir.
- Metotlar: `t.join()` (bitene dek bekler), `t.stop()` (bloklamaz; durdurma
  ister), `t.running()` (`bool`).
- `main` döndüğünde açık thread'ler beklenir.

## Globaller thread başınadır

`shared` olmayan globaller her thread'de ayrıdır ve **başlatıcıları her
thread'de yeniden çalışır** (yan etkileri dahil):

```saqut
int g = 10;
int main() {
    g = 20;
    Thread t = thread { print(g); };   // 10 yazar
    t.join();
    return 0;
}
```

## `shared` değişkenler

```saqut
shared int processed = 0;
shared float load = 0.0;
shared bool done = false;
```

- Yalnız modül kapsamında (global) yazılabilir; tipler: `int`, `float`,
  `bool`, `Pool`, `List` (E014).
- Okuma ve yazma atomiktir. `+=`, `-=`, `++`, `--` **atomik**
  okuma-değiştirme-yazmadır.
- `x = x + 1` gibi ayrı okuma + yazma atomik değildir → uyarı W008:
  `+=` kullanın ya da `lock` alın.

## `lock`

```saqut
shared int a = 0;
shared int b = 0;

void move() {
    lock a, b;         // blok sonunda otomatik bırakılır
    a = a + 1;
    b = b - 1;
}
```

- Hedef bir shared `int`/`float`/`bool` olmalı (E017).
- `lock a, b;` kilitleri sabit bir sırayla alır (kilit-kilit deadlock olmaz).
- Kilit blok sonunda, `return`/`break`/`continue` ile çıkışta ve thread
  bitince bırakılır; açık `unlock a;` da yazılabilir.
- Aynı kilidi iki kez almak / tutulmayan kilidi bırakmak derleme hatasıdır
  (E017). Kilit içinde `wait` yasaktır (E019); kilit içinde `pop`/`push`/
  `join` uyarı verir (W009).

## `wait`

```saqut
wait(processed >= 10);
```

Koşul doğru olana dek bekler; koşul en az bir `shared` sembol içermelidir
(E018). Shared bir değer her değiştiğinde koşul yeniden denenir.

## `Pool` — thread'ler arası kuyruk

```saqut
struct Job { int id; }
shared Pool jobs = Pool(Job);

int main() {
    jobs.setMax(100);                     // 0 = sınırsız
    Thread worker = thread {
        while (true) {
            Job j = jobs.pop();           // boşsa bekler
            if (j.id < 0) { return; }
            print(j.id);
        }
    };
    Job j;
    j.id = 1;
    jobs.push(j);                          // doluysa bekler
    j.id = -1;
    jobs.push(j);
    worker.join();
    return 0;
}
```

Metotlar: `push(T)`, `pop()` → `T`, `setMax(int)`, `length()`.

## `List` — ekle-yalnız paylaşımlı liste

```saqut
shared List log = List(string);
...
log.append("hazır");
string first = log.get(0);   // i >= length() → çalışma zamanı hatası (E_LIST_INDEX)
int n = log.length();
```

## `Pool(T)` / `List(T)` kuralları (v1)

- Yalnız bir `shared` global'in başlatıcısında yazılabilir (E014).
- `T` gönderilebilir olmalıdır (E015): `int`, `float`, `bool`, `byte`,
  `decimal`, `string`, `date`, enum, nullable'ları, bunlardan oluşan
  diziler/struct'lar ve `Thread`. `Pool`/`List` eleman olamaz.

## Durdurma ve hatalar

- `t.stop()` bloklayan noktalarda (`pop`, `push`, `wait`, `join`) ve döngü
  geri dönüşlerinde etkili olur; thread temiz çıkar, tuttuğu kilitler bırakılır.
- Bir thread'de yakalanmayan hata tüm programı durdurur:
  `runtime error in thread#N @ dosya:satır: mesaj`, çıkış kodu **1**.
- Tüm canlı thread'ler birbirini bekliyorsa program
  `runtime error: all threads are blocked (deadlock)` ile her thread'in
  beklediği yeri yazar ve **70** ile çıkar.
- `print` çağrı başına atomiktir (iki thread'in çıktısı bir çağrının ortasında
  karışmaz; sıraları karışabilir).

## Örnekler

`examples/threading/`: üretici-tüketici, ekle-yalnız List, stop, lock ile iki
değişkenli tutarlılık, beklenen deadlock, yakalama ve iç içe thread.
