// ============================================================================
// saQut GC — Heap (tahsis, mark-sweep, tempo politikası)
// ============================================================================
//
// DİZİN:   src/gc/gc_heap.hpp
// KATMAN:  GC — backend'lerden BAĞIMSIZ ortak çalışma zamanı katmanı
//
// AMAÇ:
//   Heap nesnelerinin tahsisi, erişilebilirlik analizi (mark-sweep) ve
//   toplamanın NE ZAMAN yapılacağına karar veren tempo politikası. VM ve
//   MIR JIT aynı Heap örneğini paylaşır — iki backend'in aynı programda
//   aynı toplama davranışını göstermesi bunun sonucudur (ADR-038).
//
// TOPLAMA MODELİ — ADR-022: taşımasız, stop-the-world, deterministik.
//
//   Taşımasız: nesne adresi ömrü boyunca sabittir. ZORUNLUDUR, çünkü
//   ArrayObject::jitData JIT'in doğrudan bellek görüntüsüdür.
//
//   Stop-the-world: toplama başladığında program durur, bitince devam eder.
//   Ara durum yoktur, dolayısıyla write barrier de yoktur. (Incremental
//   marking #217 denendi ve KALDIRILDI — gerekçe gc_heap.cpp başında.)
//
//   Deterministik: aynı program aynı noktalarda toplar. Toplama ZAMANI
//   gözlemlenebilir değildir (finalizer yok — ADR-038), ama toplanan KÜME
//   erişilebilirlikle belirlenir ve backend'e göre değişmez.
//
// KÖKLER — kimin sorduğu değil, kimin cevapladığı:
//   Heap kökleri KENDİ bilmez; kök sağlayıcılar ona kaydolur (RootSource,
//   gc_roots.hpp). VM kendi çağrı yığınını, JIT kendi shadow stack'ini
//   sağlar. collect() kayıtlı her sağlayıcıya sorar. Böylece toplama kodu
//   backend sayısından bağımsızdır.
//
// TEMPO — neden bayt, neden nesne sayısı değil:
//   Eşik nesne SAYISINA bağlanırsa, 10 baytlık string ile 10 MB'lık byte[]
//   aynı ağırlıkta sayılır; büyük tahsis yapan program neredeyse hiç
//   toplamaz, küçük tahsis yapan program sürekli toplar. Eşik tahmini
//   bayta bağlanır: toplama, ayak izi anlamlı büyüdüğünde tetiklenir.
// ============================================================================

#ifndef SAQUT_GC_HEAP
#define SAQUT_GC_HEAP

#include <cstddef>
#include <ostream>
#include <vector>

#include "gc/gc_object.hpp"
#include "gc/gc_roots.hpp"

struct Value;

// ── GC sayaçları ────────────────────────────────────────────────────────────
//
// Gözlemlenebilirlik sözleşmesi: bu sayaçlar --gc-stats ile dışa verilir ve
// SAYIM TABANLI testlerin dayanağıdır ("şu program N turdan fazla toplamaz",
// "şu blokta tahsis artışı sıfırdır"). Süreye değil olaya bağlı oldukları
// için makineden ve backend'den bağımsız olarak tekrarlanabilirler.
struct GcStats {
    long long liveObjects   = 0;  // şu an listede duran nesne sayısı
    long long liveBytes     = 0;  // onların tahmini ayak izi (bkz. estimateBytes)
    long long collections   = 0;  // tamamlanan toplama turu sayısı
    long long freedObjects  = 0;  // tüm turlarda serbest bırakılan toplam nesne
    long long peakLiveBytes = 0;  // gözlenen en yüksek canlı ayak izi
};

// Sayaçları insan-okur tek satır olarak yazar. stdout DEĞİL çağıranın
// verdiği akışa (pratikte stderr) — golden testler stdout'u karşılaştırır ve
// GC gözlemi programın çıktısı değildir.
void printGcStats(std::ostream& out, const GcStats& stats);

// ── Heap ────────────────────────────────────────────────────────────────────

struct Heap {
    // ── Tahsis ──────────────────────────────────────────────────────────────
    //
    // Her tahsis, eşik aşıldıysa toplama TETİKLEMEZ; yalnızca muhasebe
    // yapar. Toplama kararı safepoint'te (backend'in güvenli noktasında)
    // shouldCollect()/collectIfNeeded() ile verilir — tahsisin ortasında
    // toplamak, henüz hiçbir köke bağlanmamış yeni nesneyi süpürürdü.
    ArrayObject*   allocArray(int capacity = 0,
                              ArrayElemKind elemKind = ArrayElemKind::Ref);
    StructObject*  allocStruct(int fieldCount);
    StringObject*  allocString(std::string text = "");
    DecimalObject* allocDecimal(const DecimalValue& value);

    // ── Toplama ─────────────────────────────────────────────────────────────

    // Kayıtlı kök sağlayıcılardan tarayıp erişilemeyenleri serbest bırakır.
    // Dönüş: serbest bırakılan nesne sayısı. Çağrıldığı yer bir SAFEPOINT
    // olmalıdır — yani her canlı nesne ya bir kök sağlayıcıdan görünür ya
    // da görünür bir nesneden erişilebilir olmalıdır.
    long long collect();

    // Eşik aşıldıysa collect() çağırır. Backend'ler bunu safepoint'lerinde
    // çağırır; eşik aşılmadıysa maliyeti tek bir karşılaştırmadır.
    long long collectIfNeeded() { return shouldCollect() ? collect() : 0; }

    bool shouldCollect() const {
        return collectionEnabled_ && stats_.liveBytes + allocatedSinceCollect_
                                         >= nextCollectBytes_;
    }

    // ADR-045 (Faz 2-g): thread uyumadan (park) önce — son toplamadan beri
    // tahsis eşiğin yarısına ulaştıysa şimdi topla; uyuyan thread'in çöpü
    // uzun süre tutulmasın. Çağrıldığı yer bir safepoint olmalıdır.
    long long collectBeforePark() {
        if (!collectionEnabled_) return 0;
        return allocatedSinceCollect_ * 2 >= nextCollectBytes_ ? collect() : 0;
    }

    // ── Kök sağlayıcılar ────────────────────────────────────────────────────
    //
    // Sağlayıcının ömrü kaydından uzun olmalıdır; Heap sahiplenmez.
    void addRootSource(RootSource* source);
    void removeRootSource(RootSource* source);

    // ── Politika ────────────────────────────────────────────────────────────

    // Bir sonraki toplamanın tetikleneceği canlı-bayt eşiği. Her toplama
    // sonunda "canlı ayak izi × growthFactor" olarak yenilenir, minimum
    // minCollectBytes_ ile taban yapılır.
    void setMinCollectBytes(long long bytes) {
        minCollectBytes_  = bytes > 0 ? bytes : 1;
        nextCollectBytes_ = minCollectBytes_;
    }

    // Toplamayı tamamen kapatır/açar (--gc-threshold negatifken kapalı).
    // Kapalıyken tahsis serbesttir; hiçbir şey toplanmaz.
    void setCollectionEnabled(bool enabled) { collectionEnabled_ = enabled; }

    // ── Gözlem ──────────────────────────────────────────────────────────────
    const GcStats& stats() const { return stats_; }

    // ── Ömür ────────────────────────────────────────────────────────────────
    Heap() = default;
    ~Heap();
    Heap(const Heap&)            = delete;
    Heap& operator=(const Heap&) = delete;

private:
    // Tahsis edilen her nesnenin zinciri (sweep bunu gezer).
    Object* allocList_ = nullptr;

    // Mark aşamasının iş listesi: "işaretlendi, çocukları henüz taranmadı".
    // Üye olmasının sebebi performans: her toplamada yeniden tahsis etmek
    // yerine kapasitesi korunur. Toplama dışında daima boştur.
    std::vector<Object*> pendingChildren_;

    std::vector<RootSource*> rootSources_;

    GcStats   stats_;
    long long allocatedSinceCollect_ = 0;
    long long minCollectBytes_       = kDefaultMinCollectBytes;
    long long nextCollectBytes_      = kDefaultMinCollectBytes;
    bool      collectionEnabled_     = true;

    // Toplamanın tetikleneceği en küçük ayak izi. Bunun altında toplama
    // yapmak, kazanılacak bellekten çok tarama maliyeti getirir.
    static constexpr long long kDefaultMinCollectBytes = 1 << 20;  // 1 MiB

    // Toplama sonrası eşik = canlı ayak izi × bu çarpan. 2 = "ayak izi iki
    // katına çıkınca yeniden topla" — mark-sweep için yaygın ve dengeli
    // seçim: küçültmek toplama sıklığını, büyütmek bellek tavanını artırır.
    static constexpr long long kHeapGrowthFactor = 2;

    // Nesneyi zincire ekler ve muhasebesini tutar.
    template <typename ObjectT>
    ObjectT* track(ObjectT* object, long long byteSize);

    void markFromRoots();
    void markObject(Object* object);
    void drainPendingChildren();
    long long sweep();

    // Nesnenin tahmini ayak izi: başlığı + sahip olduğu tampon. Tahmindir
    // (STL'in gerçek kapasitesini değil mantıksal boyutu sayar) çünkü tempo
    // kararı için mertebe yeterlidir; kesin ölçüm allocator'ın işidir.
    static long long estimateBytes(const Object* object);

    friend void markValueForGc(Heap& heap, const Value& value);
};

#endif // SAQUT_GC_HEAP
