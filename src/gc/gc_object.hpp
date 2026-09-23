// ============================================================================
// saQut GC — Heap Nesne Modeli
// ============================================================================
//
// DİZİN:   src/gc/gc_object.hpp
// KATMAN:  GC — backend'lerden BAĞIMSIZ ortak çalışma zamanı katmanı
//
// AMAÇ:
//   Heap'te yaşayan nesnelerin (Array/Struct/String/Decimal) ortak taban
//   tipi ve tahsis edildikleri Heap. Hem VM (src/vm/) hem MIR JIT
//   (src/mir/) AYNI nesne modelini ve AYNI Heap'i kullanır — nesne modeli
//   bir backend'in iç detayı değil, iki backend'in ortak sözleşmesidir.
//
// KATMAN KURALI:
//   Bu başlık hiçbir backend başlığına bağımlı olmamalıdır. Bugünkü tek
//   kalıntı `vm/value.hpp`'dir (Value henüz src/vm/ altında yaşıyor).
//   TODO(gc-katman): Value core/ veya gc/ altına taşınınca bu bağımlılık
//   da kalkar; o taşıma ayrı bir turdur (tüketici kümesi geniş: DAP, FFI,
//   data/, json).
//
// ADR-022: Taşımasız (non-moving), stop-the-world, deterministik mark-sweep.
//   Taşımasızlık ZORUNLUDUR: ArrayObject::jitData JIT'in doğrudan bellek
//   görüntüsüdür (sabit offset'ten yüklenir) — nesne adresi oynayamaz.
//
// Nesne başlığındaki alanlar:
//   type   — Array/Struct/String/Decimal (işaretleme/silme switch'i)
//   marked — bu toplama turunda erişilebilir bulundu mu (tek canlılık kaynağı)
//   next   — Heap'in "tüm nesneler" tek yönlü intrusive listesi
// ============================================================================

#ifndef SAQUT_GC_OBJECT
#define SAQUT_GC_OBJECT

#include <memory>
#include <string>
#include <vector>

#include "core/array_elem_kind.hpp"
#include "core/decimal.hpp"  // DecimalObject (JIT decimal kutulama, ADR-037)

enum class ObjectType { Array, Struct, String, Decimal };

struct Value; // object.hpp <-> value.hpp çapraz bağımlılık; tam tanım value.hpp'de

struct Object {
    ObjectType type;

    // Bu toplama turunda köklerden erişilebilir bulundu mu. Sweep sonunda
    // canlı kalanlarda false'a döner — TEK canlılık kaynağıdır.
    //
    // Neden tek bayrak (tricolor DEĞİL): tricolor, toplama sırasında
    // programın çalışmaya devam etmesi (incremental/concurrent marking)
    // için gerekir. saQut'un toplaması stop-the-world'dür: mutator mark
    // boyunca durur, dolayısıyla "işaretlendi ama çocukları taranmadı"
    // ara durumunun gözlemleyicisi yoktur. Ara durum olmayınca write
    // barrier de gerekmez.
    bool marked = false;

    // Heap'in "tüm tahsis edilenler" zinciri. Sweep bu zinciri gezer.
    Object* next = nullptr;

    // NOT: sanal markChildren/yıkıcı YOKTUR. İşaretleme ve silme tip
    // etiketi (type) üzerinden switch ile gc_heap.cpp'te tek yerde yapılır
    // — nesne başına vptr (8 bayt + sanal çağrı) kalkar. Yeni ObjectType
    // eklendiğinde markObjectChildren/deleteObject switch'lerine case
    // girmelidir (iki switch yan yana ve yorumlu).
};

// ── ArrayObject (ADR-020: referans semantiği, #206: packed type-tagged array) ──
//
// Eleman tipi elemKind ile belirtilir. Sadece ilgili buffer kullanılır;
// diğerleri boştur.
//   Ref:      elements (vector<Value>)
//   Byte:     bytes    (vector<uint8_t>)
//   Int:      ints     (vector<int32_t>)
//   LongInt:  longs    (vector<int64_t>)
//   Float32:  f32s     (vector<float>)
//   Float64:  f64s     (vector<double>)
//   Decimal:  decimals (vector<DecimalValue>)

struct ArrayObject : Object {
    ArrayElemKind elemKind = ArrayElemKind::Ref;
    std::vector<Value>        elements;   // elemKind == Ref
    std::vector<uint8_t>      bytes;      // elemKind == Byte
    std::vector<int32_t>      ints;       // elemKind == Int
    std::vector<int64_t>      longs;      // elemKind == LongInt
    std::vector<float>        f32s;       // elemKind == Float32
    std::vector<double>       f64s;       // elemKind == Float64
    std::vector<DecimalValue> decimals;   // elemKind == Decimal

    // JIT direct-memory view (non-owning). MIR, std::vector::data()'ı
    // çağıramaz; bu alanlar scalar buffer'ın güncel adresini/length'ini
    // sabit offset'ten yüklemesi için tutulur. GC tarafından taranmaz —
    // canonical elements/fields GC hakikat kaynağıdır (markChildren DEĞİŞMEZ).
    void*          jitData     = nullptr;
    int64_t        jitLength   = 0;
    ArrayElemKind  jitElemKind = ArrayElemKind::Ref;

    // Scalar buffer'a işaret eden view'ı vector'ün güncel durumundan
    // senkronize eder. Çağrı sırası zorunlu: allocArray(reserve) → resize()
    // → syncJitView(). allocArray içine konulmaz — reserve aşamasında
    // data() geçerli bir eleman buffer'ı göstermeyebilir.
    void syncJitView() {
        jitElemKind = elemKind;
        switch (elemKind) {
            case ArrayElemKind::Byte:    jitData = bytes.data();    jitLength = (int64_t)bytes.size();    break;
            case ArrayElemKind::Int:     jitData = ints.data();     jitLength = (int64_t)ints.size();     break;
            case ArrayElemKind::LongInt: jitData = longs.data();    jitLength = (int64_t)longs.size();    break;
            case ArrayElemKind::Float32: jitData = f32s.data();     jitLength = (int64_t)f32s.size();     break;
            case ArrayElemKind::Float64: jitData = f64s.data();     jitLength = (int64_t)f64s.size();     break;
            case ArrayElemKind::Decimal: jitData = decimals.data(); jitLength = (int64_t)decimals.size(); break;
            default:
                // Ref array: vector<Value> adresi — pointer elemanlar için
                // doğrudan lowering yok (Aşama 4); view yine de senkron tutulur.
                jitData = elements.data(); jitLength = (int64_t)elements.size(); break;
        }
    }

    explicit ArrayObject(int capacity = 0, ArrayElemKind k = ArrayElemKind::Ref) : elemKind(k) {
        type = ObjectType::Array;
        // reserve kullan — resize DEĞİL. #206: slice/push builtin'leri push_back
        // ile eleman ekler; resize ön-doldurma yaparsa boyut iki katına çıkar.
        if (capacity <= 0) return;
        switch (elemKind) {
            case ArrayElemKind::Ref:     elements.reserve(capacity); break;
            case ArrayElemKind::Byte:    bytes.reserve(capacity);    break;
            case ArrayElemKind::Int:     ints.reserve(capacity);     break;
            case ArrayElemKind::LongInt: longs.reserve(capacity);    break;
            case ArrayElemKind::Float32: f32s.reserve(capacity);     break;
            case ArrayElemKind::Float64: f64s.reserve(capacity);     break;
            case ArrayElemKind::Decimal: decimals.reserve(capacity); break;
        }
    }

};

// ── StructObject (ADR-037, #206) ─────────────────────────────────────────────
//
// fieldNames tip başına bir kez tutulur (shared_ptr). Tüm örnekler aynı
// metadata'yı paylaşır. Registry: Interpreter (veya Heap) tip-adı → names
// eşlemesini tutar.

struct StructObject : Object {
    std::vector<Value>                        fields;
    std::shared_ptr<std::vector<std::string>> fieldNames; // paylaşımlı metadata

    explicit StructObject(int fieldCount = 0) {
        type = ObjectType::Struct;
        fields.resize(fieldCount);
    }

};

// Yerleşik Error struct'ının alan adları — sıra seedBuiltins() ve
// makeErrorValue ile birebir: [line, col, message, trace, code]. Runtime'da
// üretilen Error nesneleri (VM makeErrorValue, JIT jitMakeError) bu tabloyu
// taşır; aksi halde toJson/dump alan adları yerine field0..4 basıyordu (#260).
inline const std::shared_ptr<std::vector<std::string>>& errorStructFieldNames() {
    static const auto names = std::make_shared<std::vector<std::string>>(
        std::vector<std::string>{"line", "col", "message", "trace", "code"});
    return names;
}

// ── StringObject (tek string modeli, ADR-024 / ADR-037) ─────────────────────
//
// saQut string'i DEĞİŞMEZDİR (ADR-024) ve heap'te bu nesne olarak yaşar.
// İki backend AYNI temsili kullanır: Value::String bir StringObject*
// taşır (value.hpp), MIR register'ı da aynı pointer'ı taşır (ADR-037).
// Ortak temsilin iki sonucu: string'ler toplanabilir, ve VM ile JIT
// arasında string dönüştürme/kopyalama katmanı yoktur.
//
// GC notu: string'in ref çocuğu yoktur (markObjectChildren'da case yok).
struct StringObject : Object {
    std::string data;

    explicit StringObject(std::string s = "") : data(std::move(s)) {
        type = ObjectType::String;
    }

    // string'in ref çocuğu yok — markObjectChildren'da case yok
};

// ── DecimalObject (JIT sınırı, ADR-037) ──────────────────────────────────────
// DecimalValue (coeff+exp, >8 byte) MIR register'ına sığmaz → JIT'te decimal
// heap'e kutulanır, register pointer taşır. Aritmetik runtime call'a gider
// (ADR-037: decimal v1'de her zaman kutulu). VM'de Value içine inline. GC çocuğu yok.
struct DecimalObject : Object {
    DecimalValue val;

    explicit DecimalObject(const DecimalValue& v) : val(v) {
        type = ObjectType::Decimal;
    }

    // decimal'in ref çocuğu yok — markObjectChildren'da case yok
};

// ── Heap'e ileri bildirim ────────────────────────────────────────────────────
//
// Heap'in kendisi gc/gc_heap.hpp'de yaşar (tahsis + mark/sweep + politika).
// Nesne tipleri Heap'i bilmek zorunda değildir; Heap nesne tiplerini bilir.
struct Heap;

// Tek-string-modeli: Value::fromString'in tahsis yapacağı aktif heap'i
// bağla/çöz. Interpreter ve JIT çalışma başında bağlar. Ayrıntı:
// gc_heap.cpp içindeki "Value string tahsis kancası" bölümü.
void setValueStringHeap(Heap* h);

#endif // SAQUT_GC_OBJECT
