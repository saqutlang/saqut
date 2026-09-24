// ============================================================================
// saQut IR — IRProgram (Bir .sqt Dosyasının Tüm IR İçeriği)
//
// IRProgram, üretilen tüm fonksiyonları tutar.
// Interpreter programı çalıştırmak için bu yapıyı kullanır.
//
// NEDEN İKİ YAPIDA TUTUYORUZ?
//   - functionOrder: fonksiyonları tanımlandıkları sırayla tutar (dump için)
//   - functions (unordered_map): CALL instruction'larında isimle hızlı arama için
//
// NOT: unordered_map değer semantiğiyle (IRFunction by value) tutar.
//   findFunction() bir pointer döndürür — bu pointer tüm addFunction() çağrıları
//   bittikten sonra alınmalıdır. Interpreter program üretildikten sonra çalıştığı
//   için bu kural otomatik olarak sağlanır.
// ============================================================================

#ifndef SAQUT_IR_PROGRAM
#define SAQUT_IR_PROGRAM

#include <string>
#include <unordered_map>
#include <vector>
#include "ir/ir_function.hpp"
#include "core/module_registry.hpp"

struct IRProgram {
    // Fonksiyon adı → IRFunction (hızlı arama için)
    std::unordered_map<std::string, IRFunction> functions;

    // Ekleme sırası (dump'ta orijinal sırayla göstermek için)
    std::vector<std::string> functionOrder;

    // Modül adı havuzu — dosya yolları burada, her yerde int ID kullanılır.
    ModuleRegistry moduleRegistry;

    // Modül-düzeyi değişkenler (LOAD_GLOBAL / STORE_GLOBAL için)
    int                      globalCount = 0;
    std::vector<std::string> globalNames; // index → isim (dump için)

    // Modül başına global slot sayısı: moduleId (int) → slot count
    std::unordered_map<int, int> moduleGlobalCounts;

    // ADR-039: global slot statik tipleri. moduleId → (globalIdx → SlotType).
    // LOAD_GLOBAL valueType'ı ve JIT global storage tipi buradan gelir. IRGenerator
    // VarDecl'den doldurur; tek-modül fallback için INVALID_ID anahtarı kullanılır.
    std::unordered_map<int, std::vector<SlotType>> globalSlotTypes;

    // ── ADR-045: izole thread modeli ─────────────────────────────────────
    // `shared` globaller: SHARED_*/POOL_*/LIST_* opcode'larının intValue'su bu
    // dizinin indeksidir. kind runtime SharedKind ile aynı sırada:
    // 0 int, 1 float, 2 bool, 3 Pool, 4 List. Koşu başında (ana thread,
    // spawn öncesi) SharedSlots bu tabloyla kurulur.
    struct SharedSlotDesc {
        int         kind = 0;
        std::string name;
    };
    std::vector<SharedSlotDesc> sharedSlots;
    // Program thread yüzeyini kullanıyor mu (thread {} / shared). false ise
    // backend'ler hiçbir threading altyapısını (ana thread kaydı, geri kenar
    // yoklaması, join) devreye sokmaz — tek thread davranışı birebir aynı.
    bool usesThreads = false;

    // Yeni fonksiyon ekle
    void addFunction(IRFunction fn) {
        // emplace çakışmada SESSİZCE hiçbir şey yapmaz. functionOrder'a yine
        // de isim eklenirse iki liste tutarsızlaşır (order'da iki kayıt,
        // map'te bir tane) ve VM çöker. Çift tanım semantic'te E002 ile
        // yakalanır; bu savunma o denetim atlanırsa bile tutarlılığı korur.
        if (functions.count(fn.name)) return;
        functionOrder.push_back(fn.name);
        functions.emplace(fn.name, std::move(fn));
    }

    // İsimle ara — bulunamazsa nullptr döner
    IRFunction* findFunction(const std::string& name) {
        auto it = functions.find(name);
        return (it != functions.end()) ? &it->second : nullptr;
    }
    // Salt okunur arama (ADR-045: IRProgram koşu sırasında paylaşılır, değişmez).
    const IRFunction* findFunction(const std::string& name) const {
        auto it = functions.find(name);
        return (it != functions.end()) ? &it->second : nullptr;
    }

    // Tüm fonksiyonları ekleme sırasıyla yazdır
    void dump() const;
};

#endif // SAQUT_IR_PROGRAM
