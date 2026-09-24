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
// SÜREÇ-GLOBALDIR (Isolate üyesi değil): ADR-045 "kod bir kez derlenir, tüm
// thread'ler aynı makine kodunu çalıştırır; string sabitleri süreç ömrü
// boyunca yaşayan, immutable bir ConstPool'da tutulur" der. Tek iş
// parçacıklı çalışışta davranış birebir aynıdır.
//
// NOT: internString bugün senkronizasyonsuzdur; derleme tek iş parçacıklıdır.
// Eşzamanlı derleme gerekirse burası kilitlenmelidir.
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
    static ConstPool& instance() {
        static ConstPool pool;
        return pool;
    }

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

private:
    std::vector<std::unique_ptr<StringObject>>      strings_;
    std::unordered_map<std::string, StringObject*>  index_;
};

#endif // SAQUT_RUNTIME_CONST_POOL
