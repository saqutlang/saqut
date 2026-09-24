# Threading Uygunluk Raporu (olgusal)

**Referans:** [ADR-045](adr/ADR-045-isolate-threading.md)  
**Tarih:** 2026-09-24  
**Dal:** v1.0.1-multithread  
**Yöntem:** kod okuma; her iddia `dosya:satır`. Emin olunmayan yerler
**BİLİNMİYOR**.

Bu rapor, ADR-045'in dil yüzeyini ve isolate refactor'unu mevcut kod tabanına
oturtmanın önündeki somut engelleri kaydeder. Tasarım önerisi içermez.

## 1. `Pool(T)` / `List(T)` parse ve intrinsic mekanizması

- Çağrı ayrıştırma: `CallExpressionNode` (`src/parser/parser.cpp:695`);
  argümanlar `expectExpression("in argument list")` (`src/parser/parser.cpp:706,716`)
  ile **ifade** olarak okunur.
- Tip keyword'leri yalnız tip/bildirim bağlamında tanınır
  (`src/parser/parser.cpp:303,366,490`; `isStatementStartToken`
  `src/parser/parser.cpp:118-144`). `parseNullDenotation` gövdesinde
  (`src/parser/parser.cpp:498-700`) tip keyword'ü ve `{` için **case yoktur**.
- Sonuç: `Pool(int)` bugün **parse edilemez** (argüman konumunda `int` ifade
  olarak çözülemez); `Pool(SomeStruct)` (IDENTIFIER) parse edilir ama
  semantikte "tanımsız ad" olur. Yani Pool/List için parser değişikliği
  gerekir (argümanı tip olarak okumak).
- Mevcut intrinsic/builtin mekanizması:
  - `print` seeded builtin (`src/symbol/symbol_collector.cpp:80`); argüman
    sayısı kontrolünde arity istisnası (`src/semantic/type_checker.cpp:1223`).
  - UFCS / builtin metot çözümü `ScopeCall` yolunda
    (`src/semantic/type_checker.cpp:1455-1680`); `builtinId` ataması
    (`src/semantic/type_checker.cpp:1677-1680`).
  - `as` = `CastExpression` (`src/parser/parser.cpp:743-745`).
- **Callee**'ye göre argümanı tip adı çözen bir yol **yoktur**; builtin
  çözümü receiver **tipinden** yapılır (`src/semantic/type_checker.cpp:1484-1522`),
  argümandan değil. Pool/List intrinsic'i yeni bir tanıma yolu gerektirir.

## 2. Yeni anahtar kelimeler ve çakışmalar

- `shared`, `lock`, `unlock`, `wait`, `thread`, `Pool`, `List`, `Thread`
  kelimelerinin **hiçbiri** anahtar tablosunda yok
  (`src/parser/token.hpp:427+`; tokenizer keywords[] boş).
- Identifier olarak kullanım: `List` örneklerde geçiyor,
  `examples/parser-stress/Final.sqt:42` (`struct List`) ve `:47`
  (`createList()`). Kalan kelimeler için `.sqt` identifier kullanımı
  **bulunmadı**.
- Çakışma riski yalnız `List` içindir; `Final.sqt` zaten pointer kullanan
  eski bir stress örneğidir.

## 3. `thread { ... }` ifade olarak ayrıştırma noktası

- Blok-ifade (block expression) **yoktur**: `{` yalnız `parseStatement`
  içinde blok deyimidir (`src/parser/parser.cpp:1218`);
  `parseNullDenotation`'da (`src/parser/parser.cpp:498`) LBRACE case yoktur.
- Uygun yer: `parseNullDenotation`'a `thread` için bir case eklemek ve gövdeyi
  `parseBlock`'tan almak. Bugün `thread` IDENTIFIER olduğundan `thread { ... }`
  Identifier + blok olarak ayrışıp hata verir.
- **BİLİNMİYOR:** lambda-lifting'in hangi katmanda (semantik/IR) yapılacağı ve
  blok gövdesinin fonksiyona çevrilmesinde mevcut altyapının ne kadarının
  (closure yok) yeniden kullanılabileceği.

## 4. Global değişken başlatma

- IR: `IRGenerator` tüm modül globallerine slot atar
  (`src/ir/ir_generator.cpp:65-75,149-162`).
- VM: `std::vector<Value> globalSlots_;` **Interpreter üyesidir**
  (`src/vm/interpreter.hpp:152`); sıfırlama
  `globalSlots_.assign(program_.globalCount, Value::fromInt(0))`
  (`src/vm/interpreter.cpp:1494-1495`); GC kökü olarak taranır
  (`src/vm/interpreter.cpp:341`).
- Global **init ifadeleri `main`'in başında** üretilir
  (`src/ir/ir_generator.cpp:187`). Her isolate kendi `Interpreter`'ını alırsa
  `globalSlots_` doğal olarak ayrıdır; ancak init kodunun yalnız `main`
  başında üretilmesi, yeni thread'lerin globallerini yeniden başlatmasını
  zorlaştıran somut noktadır. Runtime'da nasıl ele alınacağı
  **BİLİNMİYOR** (tasarlanmadı).

## 5. Tip sisteminde eleman tipi

- `enum class TypeKind { Primitive, Array, Struct, Enum, Function, Error };`
  (`src/core/type.hpp:45`).
- Yalnız `Array` bir eleman tipi taşır
  (`std::shared_ptr<Type> elementType`, `src/core/type.hpp:72`); genel tip
  parametresi yoktur (Struct/Enum adla).
- `Pool(T)`/`List(T)` için ya yeni bir `TypeKind` (+ element tipi alanı) ya da
  Array-benzeri bir taşıyıcı gerekir; mevcut yapıda keyfi tipe "ek parametre"
  iliştirme ve jenerik tip **yoktur**.

## Kapsam dışı / doğrulanmayan

- LSP/DAP oturum state'i ve derleyici CLI yolları bu raporda incelenmedi.
- İki eşzamanlı **derlemenin** davranışı **BİLİNMİYOR**.
