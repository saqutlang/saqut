// ============================================================================
// saQut GC — Heap Gerçeklemesi (mark-sweep)
// ============================================================================
//
// DİZİN:   src/gc/gc_heap.cpp
// KATMAN:  GC — backend'lerden BAĞIMSIZ ortak çalışma zamanı katmanı
//
// TOPLAMA İKİ GEÇİŞTİR:
//
//   1. MARK   — köklerden başlayarak erişilebilir olan her nesne işaretlenir.
//               Geçiş özyinelemeli DEĞİL, açık bir iş listesiyle yapılır
//               (pendingChildren_): derin nesne grafı C++ çağrı yığınını
//               taşırmasın diye. Uzun bağlı liste veya derin iç içe struct
//               özyinelemede yığın taşmasıyla çökerdi.
//
//   2. SWEEP  — tahsis zinciri baştan sona gezilir; işaretsiz nesneler
//               silinir, işaretli olanların bayrağı sonraki tur için
//               sıfırlanır.
//
// NEDEN INCREMENTAL MARKING YOK (#217 kaldırıldı):
//   Tricolor + write barrier altyapısı denendi. Ölçümde marking hiçbir zaman
//   birden fazla safepoint'e YAYILMADI: kökler işaretlenip transitif kapanış
//   aynı çağrıda tamamlanıyor, sweep de orada yapılıyordu — yani mekanizma
//   fiilen stop-the-world çalışıyor, karşılığında iki gizli hata taşıyordu:
//   write barrier yalnız Ref türünü kapsıyordu (tek-string-modelinden sonra
//   String de heap nesnesi olduğu halde barrier'a girmiyordu) ve yeni
//   nesneler beyaz doğuyordu (allocate-black yok) — marking gerçekten
//   yayılsaydı ikisi de canlı nesnenin süpürülmesi demekti.
//
//   Ayrıca incremental'ın çözdüğü problem saQut'un hedef modelinde doğmuyor:
//   toplama sırasında duran tek şey, heap'in kendi mutator'udur. Her heap'in
//   tek mutator'u olduğu sürece (izolasyon modeli) "dünya" zaten tek bir
//   iş parçacığıdır. Duraklama süresi sorun olursa doğru cevap toplamayı
//   bölmek değil, toplanacak nesne sayısını azaltmaktır.
//
// TOPLAMA NEREDE TETİKLENİR:
//   Tahsiste DEĞİL, backend'in safepoint'inde. Tahsis anında toplamak, yeni
//   nesne henüz hiçbir köke bağlanmadığı için onu süpürerdi. Bu yüzden
//   alloc* yalnızca muhasebe yapar; kararı collectIfNeeded() verir.
// ============================================================================

#include "gc/gc_heap.hpp"
#include "vm/value.hpp"
#include "runtime/isolate.hpp"

#include <algorithm>

namespace {

// Nesnenin çocuklarını sink'e bildirir. Sanal markChildren yerine tip
// etiketi switch'i — nesne başına vptr ve sanal çağrı maliyeti kalkar.
//
// Yeni bir ObjectType eklendiğinde BURAYA ve deleteObject'e case eklenmesi
// gerekir; iki switch bilerek yan yana tutuluyor.
template <typename VisitFn>
void visitChildren(Object* object, VisitFn&& visit) {
    switch (object->type) {
        case ObjectType::Array: {
            auto* array = static_cast<ArrayObject*>(object);
            // Sayısal elemanlı diziler (byte/int/long/float/decimal) referans
            // taşımaz — GC için yapraktır, taranmaz. #206 packed temsilinin
            // GC tarafındaki kazancı budur: 10 MB'lık byte[] mark'ta O(1).
            if (array->elemKind != ArrayElemKind::Ref) return;
            for (const Value& element : array->elements) visit(element);
            return;
        }
        case ObjectType::Struct:
            for (const Value& field : static_cast<StructObject*>(object)->fields)
                visit(field);
            return;
        case ObjectType::String:   // string'in referans çocuğu yok
        case ObjectType::Decimal:  // decimal'in referans çocuğu yok
            return;
    }
}

// Tip-güvenli silme: nesnelerde vptr yoktur, taban işaretçiden delete
// edilemez (yıkıcı sanal değil) — tip etiketiyle gerçek tipe indirilir.
void deleteObject(Object* object) {
    switch (object->type) {
        case ObjectType::Array:   delete static_cast<ArrayObject*>(object);   return;
        case ObjectType::Struct:  delete static_cast<StructObject*>(object);  return;
        case ObjectType::String:  delete static_cast<StringObject*>(object);  return;
        case ObjectType::Decimal: delete static_cast<DecimalObject*>(object); return;
    }
}

// Bir Value referans taşıyor mu? Tek-string-modelinde String de heap
// nesnesidir, bu yüzden Ref ile aynı muameleyi görür.
bool holdsObject(const Value& value) {
    return value.kind == ValueKind::Ref || value.kind == ValueKind::String;
}

}  // namespace

// ── Tahsis ──────────────────────────────────────────────────────────────────

template <typename ObjectT>
ObjectT* Heap::track(ObjectT* object, long long byteSize) {
    object->next = allocList_;
    allocList_   = object;

    ++stats_.liveObjects;
    stats_.liveBytes += byteSize;
    allocatedSinceCollect_ += byteSize;
    stats_.peakLiveBytes = std::max(stats_.peakLiveBytes, stats_.liveBytes);
    return object;
}

ArrayObject* Heap::allocArray(int capacity, ArrayElemKind elemKind) {
    auto* array = new ArrayObject(capacity, elemKind);
    return track(array, estimateBytes(array));
}

StructObject* Heap::allocStruct(int fieldCount) {
    auto* structObject = new StructObject(fieldCount);
    return track(structObject, estimateBytes(structObject));
}

StringObject* Heap::allocString(std::string text) {
    auto* stringObject = new StringObject(std::move(text));
    return track(stringObject, estimateBytes(stringObject));
}

DecimalObject* Heap::allocDecimal(const DecimalValue& value) {
    auto* decimalObject = new DecimalObject(value);
    return track(decimalObject, estimateBytes(decimalObject));
}

// ── Ayak izi tahmini ────────────────────────────────────────────────────────
//
// Tempo kararı için kullanılır, muhasebe için değil: mantıksal boyut sayılır
// (vector'ün ayırdığı fazladan kapasite değil). Mertebe doğruluğu yeterlidir
// ve tahsis yolunda ucuz kalması önemlidir.

long long Heap::estimateBytes(const Object* object) {
    switch (object->type) {
        case ObjectType::Array: {
            const auto* array = static_cast<const ArrayObject*>(object);
            long long   base  = (long long)sizeof(ArrayObject);
            switch (array->elemKind) {
                case ArrayElemKind::Ref:
                    return base + (long long)(array->elements.size() * sizeof(Value));
                case ArrayElemKind::Byte:
                    return base + (long long)array->bytes.size();
                case ArrayElemKind::Int:
                    return base + (long long)(array->ints.size() * sizeof(int32_t));
                case ArrayElemKind::LongInt:
                    return base + (long long)(array->longs.size() * sizeof(int64_t));
                case ArrayElemKind::Float32:
                    return base + (long long)(array->f32s.size() * sizeof(float));
                case ArrayElemKind::Float64:
                    return base + (long long)(array->f64s.size() * sizeof(double));
                case ArrayElemKind::Decimal:
                    return base + (long long)(array->decimals.size() * sizeof(DecimalValue));
            }
            return base;
        }
        case ObjectType::Struct: {
            const auto* structObject = static_cast<const StructObject*>(object);
            return (long long)sizeof(StructObject) +
                   (long long)(structObject->fields.size() * sizeof(Value));
        }
        case ObjectType::String: {
            const auto* stringObject = static_cast<const StringObject*>(object);
            return (long long)sizeof(StringObject) +
                   (long long)stringObject->data.size();
        }
        case ObjectType::Decimal:
            return (long long)sizeof(DecimalObject);
    }
    return (long long)sizeof(Object);
}

// ── Kök sağlayıcı kaydı ─────────────────────────────────────────────────────

void Heap::addRootSource(RootSource* source) {
    if (!source) return;
    // Aynı sağlayıcının iki kez kaydı zararsız olurdu (kökler idempotent
    // işaretlenir) ama kaydı tutarlı tutmak removeRootSource'u öngörülebilir
    // kılar: tek kayıt, tek silme.
    if (std::find(rootSources_.begin(), rootSources_.end(), source) !=
        rootSources_.end())
        return;
    rootSources_.push_back(source);
}

void Heap::removeRootSource(RootSource* source) {
    rootSources_.erase(
        std::remove(rootSources_.begin(), rootSources_.end(), source),
        rootSources_.end());
}

// ── Mark ────────────────────────────────────────────────────────────────────

void Heap::markObject(Object* object) {
    if (!object || object->marked || object->immortal) return;
    object->marked = true;
    // Çocukları burada TARANMAZ: iş listesine konur. Özyineleme yerine
    // açık liste kullanmanın sebebi budur (dosya başındaki not).
    pendingChildren_.push_back(object);
}

void Heap::drainPendingChildren() {
    while (!pendingChildren_.empty()) {
        Object* object = pendingChildren_.back();
        pendingChildren_.pop_back();
        visitChildren(object, [this](const Value& value) {
            if (holdsObject(value)) markObject(value.ref());
        });
    }
}

void Heap::markFromRoots() {
    // Sağlayıcıların gördüğü bildirim kanalı. Heap'in iç işaretleme
    // ayrıntısını (iş listesi, bayrak) dışarı sızdırmaz.
    struct MarkingSink : RootSink {
        Heap* heap;
        explicit MarkingSink(Heap* h) : heap(h) {}

        void acceptValue(const Value& value) override {
            if (holdsObject(value)) heap->markObject(value.ref());
        }
        void acceptObject(Object* object) override { heap->markObject(object); }
    };

    MarkingSink sink(this);
    for (RootSource* source : rootSources_) source->collectRoots(sink);
    drainPendingChildren();
}

// ── Sweep ───────────────────────────────────────────────────────────────────

long long Heap::sweep() {
    long long freed     = 0;
    long long liveBytes = 0;
    long long liveCount = 0;

    // Zincir tek yönlü gezilir; silinecek düğümü zincirden çıkarabilmek için
    // bir önceki düğümün "next" alanına yazılır.
    Object*  current  = allocList_;
    Object** linkSlot = &allocList_;

    while (current) {
        Object* next = current->next;
        if (current->marked) {
            current->marked = false;  // sonraki tur için sıfırla
            liveBytes += estimateBytes(current);
            ++liveCount;
            linkSlot  = &current->next;
        } else {
            *linkSlot = next;
            deleteObject(current);
            ++freed;
        }
        current = next;
    }

    stats_.liveObjects = liveCount;
    stats_.liveBytes   = liveBytes;
    stats_.freedObjects += freed;
    return freed;
}

// ── Toplama ─────────────────────────────────────────────────────────────────

long long Heap::collect() {
    markFromRoots();
    const long long freed = sweep();

    ++stats_.collections;
    allocatedSinceCollect_ = 0;

    // Bir sonraki eşik canlı ayak izinden türetilir: program büyüdükçe
    // daha seyrek, küçüldükçe daha sık toplanır. Taban, küçük programların
    // her tahsiste toplamasını önler.
    nextCollectBytes_ = std::max(minCollectBytes_,
                                 stats_.liveBytes * kHeapGrowthFactor);
    return freed;
}

// ── Ömür ────────────────────────────────────────────────────────────────────

Heap::~Heap() {
    Object* current = allocList_;
    while (current) {
        Object* next = current->next;
        deleteObject(current);
        current = next;
    }
}

// ── Gözlem ──────────────────────────────────────────────────────────────────

void printGcStats(std::ostream& out, const GcStats& stats) {
    out << "gc: collections=" << stats.collections
        << " freed=" << stats.freedObjects
        << " live=" << stats.liveObjects
        << " liveBytes=" << stats.liveBytes
        << " peakBytes=" << stats.peakLiveBytes << "\n";
}

// ── Value string tahsis kancası (tek string modeli) ─────────────────────────
//
// Value::fromString bir StringObject tahsis etmek zorundadır ama value.hpp
// Heap'i göremez (katman sırası: gc → value, tersi değil). Kanca bu yönü
// tersine çevirmeden bağlar: çalışan backend başlangıçta aktif heap'i
// bağlar, Value::fromString onu kullanır.
//
// Kanca isolate başına durur (ADR-045, Faz 1): bağlı heap Isolate üyesidir,
// böylece her iş parçacığı kendi heap'ini görür.
void setValueStringHeap(Heap* heap) { Isolate::current().stringHeap = heap; }

Object* allocValueString(std::string text) {
    if (Heap* active = Isolate::current().stringHeap)
        return active->allocString(std::move(text));

    // Heap bağlı değil (birim testi, izole kullanım): nesne toplanmaz ama
    // sızmaz da — süreç ömrü boyunca yedek havuzda tutulur. Bağlı heap
    // durumuyla gözlenen davranış aynıdır, yalnızca geri kazanım yoktur.
    static thread_local std::vector<std::unique_ptr<StringObject>> fallbackPool;
    fallbackPool.push_back(std::make_unique<StringObject>(std::move(text)));
    return fallbackPool.back().get();
}

// Value::stringValue gövdesi burada: StringObject'in tam tanımı yalnızca bu
// katmanda görünür (value.hpp yalnız ileri bildirim taşır).
const std::string& Value::stringValue() const {
    return static_cast<StringObject*>(p.r)->data;
}
