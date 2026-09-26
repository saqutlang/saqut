// ============================================================================
// saQut — Pool / List / Thread metot tablosu (ADR-045)
// ============================================================================
//
// DİZİN:   src/semantic/thread_intrinsics.hpp
// BAĞIMLI: core/type.hpp, parser/nodes/expressions.hpp (ThreadIntrinsic)
//
// Pool/List/Thread alıcılı metotlar src/data/ kaydında değildir: gövdeleri
// host thunk'ı değil, IR'de ayrı opcode'lardır (THREAD_*, POOL_*, LIST_*).
// İmzaları bu TEK tabloda durur; TypeChecker (checkThreadIntrinsic) ve LSP
// (tamamlama, dönüş tipi, imza yardımı) aynı tabloyu okur.
//
// YENİ METOT: ThreadIntrinsic'e bir değer (expressions.hpp), bu tabloya bir
// satır, IRGenerator'da op'un karşılığı olan opcode.
//
// ============================================================================

#ifndef SAQUT_SEMANTIC_THREAD_INTRINSICS
#define SAQUT_SEMANTIC_THREAD_INTRINSICS

#include <optional>
#include <string>
#include <vector>

#include "core/type.hpp"
#include "parser/nodes/expressions.hpp"

enum class ThreadReceiver { Pool, List, Thread };

// Argüman ve dönüş tipi kuralı. Elem = alıcının eleman tipi (Pool<int> → int).
enum class ThreadValue { None, Elem, Int, Bool };

struct ThreadMethod {
    ThreadReceiver  receiver;
    const char*     name;
    ThreadIntrinsic op;
    ThreadValue     arg;       // None: argümansız; diğerleri tek argüman
    const char*     argName;   // imzada görünen ad ("value", "index")
    ThreadValue     ret;       // None: void
    bool            blocking;  // kilit içinde çağrılırsa W009
    const char*     doc;
};

inline const std::vector<ThreadMethod>& threadMethods() {
    using R = ThreadReceiver;
    using V = ThreadValue;
    static const std::vector<ThreadMethod> table = {
        {R::Pool, "push", TI_PoolPush, V::Elem, "value", V::None, true,
         "Kuyruğa ekler; kuyruk doluysa (setMax) yer açılana kadar bekler."},
        {R::Pool, "pop", TI_PoolPop, V::None, "", V::Elem, true,
         "Kuyruktan alır; kuyruk boşsa eleman gelene kadar bekler."},
        {R::Pool, "setMax", TI_PoolSetMax, V::Int, "n", V::None, false,
         "Kuyruk kapasitesini sınırlar (push bu sınırda bekler)."},
        {R::Pool, "length", TI_PoolLength, V::None, "", V::Int, false,
         "Kuyruktaki eleman sayısı."},
        {R::List, "append", TI_ListAppend, V::Elem, "value", V::None, false,
         "Listenin sonuna ekler (ekle-yalnız, thread'ler arası)."},
        {R::List, "get", TI_ListGet, V::Int, "index", V::Elem, false,
         "index'teki elemanı döndürür."},
        {R::List, "length", TI_ListLength, V::None, "", V::Int, false,
         "Listedeki eleman sayısı."},
        {R::Thread, "join", TI_ThreadJoin, V::None, "", V::None, true,
         "Thread bitene kadar bekler."},
        {R::Thread, "stop", TI_ThreadStop, V::None, "", V::None, false,
         "Thread'in durmasını ister; beklemez."},
        {R::Thread, "running", TI_ThreadRunning, V::None, "", V::Bool, false,
         "Thread hâlâ çalışıyorsa true."},
    };
    return table;
}

inline std::optional<ThreadReceiver> threadReceiverOf(const Type& t) {
    if (t.isPool()) return ThreadReceiver::Pool;
    if (t.isList()) return ThreadReceiver::List;
    if (t.isThread()) return ThreadReceiver::Thread;
    return std::nullopt;
}

inline const char* threadReceiverName(ThreadReceiver r) {
    switch (r) {
        case ThreadReceiver::Pool:   return "Pool";
        case ThreadReceiver::List:   return "List";
        case ThreadReceiver::Thread: return "Thread";
    }
    return "";
}

inline const ThreadMethod* findThreadMethod(ThreadReceiver r, const std::string& name) {
    for (const ThreadMethod& m : threadMethods())
        if (m.receiver == r && name == m.name) return &m;
    return nullptr;
}

// Tanı ipuçları için: "push, pop, setMax, length".
inline std::string threadMethodNames(ThreadReceiver r) {
    std::string names;
    for (const ThreadMethod& m : threadMethods()) {
        if (m.receiver != r) continue;
        if (!names.empty()) names += ", ";
        names += m.name;
    }
    return names;
}

// Kuralın bu alıcıdaki somut tipi. Elem, eleman tipi bilinmiyorsa Error olur.
inline Type threadValueType(ThreadValue v, const Type& receiver) {
    switch (v) {
        case ThreadValue::None: return Type::Void();
        case ThreadValue::Elem: return receiver.elementType ? *receiver.elementType : Type::error();
        case ThreadValue::Int:  return Type::Int();
        case ThreadValue::Bool: return Type::Bool();
    }
    return Type::error();
}

#endif // SAQUT_SEMANTIC_THREAD_INTRINSICS
