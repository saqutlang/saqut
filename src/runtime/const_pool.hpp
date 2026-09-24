// ============================================================================
// saQut — ConstPool (immutable string sabitleri, ADR-045)
//
// DİZİN:   src/runtime/const_pool.hpp
// KATMAN:  runtime — süreç ömrü boyunca yaşayan sabit havuzu
//
// JIT derlemesi LOAD_STRING sabitlerini burada kutular; StringObject* pointer
// native koda int sabiti olarak gömülür. Nesneler `immortal` işaretlenir:
// hiçbir Heap'in nesne zincirinde değildirler, GC onları ne işaretler ne
// süpürür (markObject immortal bitini görünce döner).
//
// PROGRAM-ÖMÜRLÜDÜR (c3, S2): CompiledProgram üyesidir; süreç-global değil,
// Isolate üyesi de değil. Derleme sırasında doldurulur, sonra salt okunur;
// tüm isolate'ler aynı nesneleri görür. CompiledProgram yıkılınca (tüm
// isolate'ler bittikten sonra) serbest kalır.
//
// NOT: intern* senkronizasyonsuzdur; yalnız compileProgram (tek thread)
// çağırır. Koşu sırasında mutasyon yoktur.
// ============================================================================

#ifndef SAQUT_RUNTIME_CONST_POOL
#define SAQUT_RUNTIME_CONST_POOL

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gc/gc_object.hpp"

class ConstPool {
public:
    ConstPool() = default;
    ConstPool(const ConstPool&)            = delete;
    ConstPool& operator=(const ConstPool&) = delete;

    // Aynı içerik tek StringObject'e indirgenir; nesne immortal işaretlenir.
    StringObject* internString(const std::string& s) {
        auto it = index_.find(s);
        if (it != index_.end()) return it->second;
        strings_.push_back(std::make_unique<StringObject>(s));
        StringObject* obj = strings_.back().get();
        obj->immortal   = true;
        index_.emplace(s, obj);
        return obj;
    }

    // Decimal literalleri: değere göre tekilleştirilmiş, immortal (ADR-045).
    // LOAD_DECIMAL codegen'i artık heap'e dokunmaz; gömülen adres program-
    // ömürlü ve immutable olur.
    DecimalObject* internDecimal(const DecimalValue& v) {
        const std::string key = v.toString();
        auto it = decIndex_.find(key);
        if (it != decIndex_.end()) return it->second;
        decimals_.push_back(std::make_unique<DecimalObject>(v));
        DecimalObject* obj = decimals_.back().get();
        obj->immortal = true;
        decIndex_.emplace(key, obj);
        return obj;
    }

    // p, bu havuzun sahip olduğu bir nesne mi? (doğrusal; test/teşhis için —
    // embedProgramPtr assert'i CompiledProgram::ownsEmbeddable kullanır)
    bool owns(const void* p) const {
        for (const auto& s : strings_)  if (s.get() == p) return true;
        for (const auto& d : decimals_) if (d.get() == p) return true;
        return false;
    }

private:
    std::vector<std::unique_ptr<StringObject>>      strings_;
    std::unordered_map<std::string, StringObject*>  index_;
    std::vector<std::unique_ptr<DecimalObject>>      decimals_;
    std::unordered_map<std::string, DecimalObject*>  decIndex_;
};

#endif // SAQUT_RUNTIME_CONST_POOL
