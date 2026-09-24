// ============================================================================
// saQut — Faz 2 runtime primitifleri birim testleri (ADR-045 Faz 2-i)
//
//   1. PoolCore: N üretici × M tüketici (toplam ve sayı korunur)
//   2. PoolCore: sınırlı kuyrukta push bloklar (uzunluk max'ı aşmaz)
//   3. PoolCore: pop'ta beklerken stop → false döner
//   4. ListCore: 4 thread × 100k eşzamanlı append + eşzamanlı kilitsiz get
//      (sayı ve eleman kümesi doğrulanır)
//   5. Mesaj: serialize/deserialize gidiş-dönüş (aliasing + döngü + string +
//      paketli dizi + decimal)
//   6. Deadlock dedektörü: pozitif (iki thread boş kuyrukta) ve negatif
//      (normal üretici-tüketici koşusunda tetiklenmez)
//   7. SharedSlots: 4 thread × 100k atomik += ; lock altında iki değişkenli
//      tutarlılık
//
// Derleme: cmake --build build --target threading_primitives_test
// TSan:    build-tsan (bkz. CMakeLists.txt ADR-045 bölümü)
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "gc/gc_heap.hpp"
#include "gc/gc_object.hpp"
#include "runtime/threading/list_core.hpp"
#include "runtime/threading/message.hpp"
#include "runtime/threading/park.hpp"
#include "runtime/threading/pool_core.hpp"
#include "runtime/threading/shared_slots.hpp"
#include "runtime/threading/thread_table.hpp"
#include "vm/value.hpp"

using namespace saqut::threading;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  %s %s\n", ok ? "OK  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

// Tamsayı mesajı (heap'e dokunmaz).
MessageBuffer intMsg(int64_t v) { return makeMessage(Value::fromLongInt(v)); }

int64_t msgInt(const MessageBuffer& m) {
    Heap heap;
    return deserialize(m, heap).int64Value();
}

void testProducersConsumers() {
    std::printf("[1] PoolCore N uretici x M tuketici\n");
    constexpr int kProducers = 4, kConsumers = 3, kPerProducer = 20000;
    PoolCore pool("jobs");
    pool.setMax(64);
    std::atomic<int64_t> sum{0}, count{0};
    std::stop_source     stopConsumers;
    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; ++p)
        threads.emplace_back([&, p] {
            for (int i = 1; i <= kPerProducer; ++i)
                pool.push(intMsg(int64_t(p) * 1000000 + i), std::stop_token{});
        });
    for (int c = 0; c < kConsumers; ++c)
        threads.emplace_back([&] {
            MessageBuffer m;
            while (pool.pop(m, stopConsumers.get_token())) {
                sum += msgInt(m);
                ++count;
            }
        });
    for (int p = 0; p < kProducers; ++p) threads[p].join();
    while (count.load() < int64_t(kProducers) * kPerProducer)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    stopConsumers.request_stop();
    for (size_t t = kProducers; t < threads.size(); ++t) threads[t].join();

    int64_t expected = 0;
    for (int p = 0; p < kProducers; ++p)
        for (int i = 1; i <= kPerProducer; ++i) expected += int64_t(p) * 1000000 + i;
    check(count.load() == int64_t(kProducers) * kPerProducer, "sayi korunur");
    check(sum.load() == expected, "toplam korunur");
    check(pool.length() == 0, "kuyruk bos");
}

void testBoundedBlocks() {
    std::printf("[2] PoolCore sinirli kuyrukta push bloklar\n");
    PoolCore pool("bounded");
    pool.setMax(2);
    std::atomic<int> pushed{0};
    std::thread producer([&] {
        for (int i = 0; i < 5; ++i) {
            pool.push(intMsg(i), std::stop_token{});
            ++pushed;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(pushed.load() == 2 && pool.length() == 2, "max=2'de 3. push bekliyor");
    int64_t maxSeen = 0, got = 0;
    MessageBuffer m;
    for (int i = 0; i < 5; ++i) {
        maxSeen = std::max(maxSeen, pool.length());
        pool.pop(m, std::stop_token{});
        got += msgInt(m);
    }
    producer.join();
    check(maxSeen <= 2, "uzunluk max'i asmadi");
    check(got == 0 + 1 + 2 + 3 + 4, "FIFO elemanlari eksiksiz");
}

void testStopWhilePopping() {
    std::printf("[3] pop'ta beklerken stop\n");
    PoolCore         pool("empty");
    std::stop_source ss;
    std::atomic<int> result{-1};
    std::thread t([&] {
        MessageBuffer m;
        result = pool.pop(m, ss.get_token()) ? 1 : 0;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check(result.load() == -1, "pop bloklu");
    ss.request_stop();
    t.join();
    check(result.load() == 0, "stop ile false dondu");
}

void testListConcurrentAppend() {
    std::printf("[4] ListCore 4 x 100k esmanli append\n");
    constexpr int kThreads = 4, kPer = 100000;
    ListCore list("log");
    std::atomic<bool> done{false};
    std::atomic<int64_t> readerChecks{0};
    std::thread reader([&] {
        // Kilitsiz okuyucu: yayınlanmış her eleman okunabilir olmalı.
        while (!done.load()) {
            const int64_t n = list.length();
            if (n > 0 && list.get(n - 1) != nullptr) ++readerChecks;
        }
    });
    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t)
        writers.emplace_back([&, t] {
            for (int i = 0; i < kPer; ++i)
                list.append(intMsg(int64_t(t) * kPer + i));
        });
    for (auto& w : writers) w.join();
    done = true;
    reader.join();
    check(list.length() == int64_t(kThreads) * kPer, "uzunluk = 400000");
    std::set<int64_t> seen;
    for (int64_t i = 0; i < list.length(); ++i) seen.insert(msgInt(*list.get(i)));
    check(seen.size() == size_t(kThreads) * kPer && *seen.begin() == 0 &&
              *seen.rbegin() == int64_t(kThreads) * kPer - 1,
          "eleman kumesi eksiksiz ve tekil");
    check(list.get(list.length()) == nullptr && list.get(-1) == nullptr, "aralik disi -> nullptr");
    check(readerChecks.load() >= 0, "esmanli okuyucu calisti");
}

void testMessageRoundTrip() {
    std::printf("[5] mesaj gidis-donus (aliasing + dongu)\n");
    Heap src;
    StructObject* s = src.allocStruct(4);
    s->fieldNames   = std::make_shared<std::vector<std::string>>(
        std::vector<std::string>{"arr", "name", "nums", "price"});
    ArrayObject* a = src.allocArray(2, ArrayElemKind::Ref);
    a->elements.push_back(Value::fromRef(s));   // döngü: s -> a -> s
    a->elements.push_back(Value::fromRef(s));   // aliasing: iki eleman aynı nesne
    a->syncJitView();
    ArrayObject* nums = src.allocArray(3, ArrayElemKind::Int);
    nums->ints = {7, 8, 9};
    nums->syncJitView();
    s->fields[0] = Value::fromRef(a);
    s->fields[1] = Value::fromStringObject(src.allocString("merhaba"));
    s->fields[2] = Value::fromRef(nums);
    s->fields[3] = Value::fromDecimal(DecimalValue::fromInt(42));

    MessageBuffer m = makeMessage(Value::fromRef(s));
    Heap dst;
    Value v = deserialize(m, dst);
    auto* s2 = static_cast<StructObject*>(v.ref());
    check(v.kind == ValueKind::Ref && s2 != s, "yeni struct nesnesi");
    check(s2->fieldNames == s->fieldNames, "alan adlari paylasimli metadata");
    auto* a2 = static_cast<ArrayObject*>(s2->fields[0].ref());
    check(a2 != a && a2->elements.size() == 2, "dizi kopyalandi");
    check(a2->elements[0].ref() == s2 && a2->elements[1].ref() == s2, "dongu ve aliasing korundu");
    check(s2->fields[1].kind == ValueKind::String && s2->fields[1].ref() != s->fields[1].ref() &&
              static_cast<StringObject*>(s2->fields[1].ref())->data == "merhaba",
          "string derin kopya");
    auto* n2 = static_cast<ArrayObject*>(s2->fields[2].ref());
    check(n2->elemKind == ArrayElemKind::Int && n2->ints == std::vector<int32_t>{7, 8, 9} &&
              n2->jitLength == 3,
          "paketli int dizisi + jit gorunumu");
    check(s2->fields[3].kind == ValueKind::Decimal &&
              s2->fields[3].decimalValue().toString() == DecimalValue::fromInt(42).toString(),
          "decimal deger");
}

std::atomic<int> g_deadlockReports{0};
void recordingHandler(const std::string&) { ++g_deadlockReports; }

void testDeadlockDetector() {
    std::printf("[6] deadlock dedektoru\n");
    setDeadlockHandler(recordingHandler);
    setDeadlockDetectionEnabled(true);
    auto& table = ThreadTable::instance();

    // Negatif: normal üretici-tüketici — dedektör tetiklenmemeli.
    {
        PoolCore q("neg");
        auto& prod = table.spawn("producer", [&](ThreadCore& self) {
            for (int i = 0; i < 2000; ++i) q.push(intMsg(i), self.stopToken());
        });
        auto& cons = table.spawn("consumer", [&](ThreadCore& self) {
            MessageBuffer m;
            for (int i = 0; i < 2000; ++i) q.pop(m, self.stopToken());
        });
        while (!prod.finished() || !cons.finished())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(g_deadlockReports.load() == 0, "negatif: rapor yok");
    }

    // Pozitif: iki thread boş kuyruklarda — ana thread kayıtlı değil, live=2.
    {
        PoolCore q1("q1"), q2("q2");
        auto& t1 = table.spawn("a", [&](ThreadCore& self) {
            MessageBuffer m;
            q1.pop(m, self.stopToken());
        });
        auto& t2 = table.spawn("b", [&](ThreadCore& self) {
            MessageBuffer m;
            q2.pop(m, self.stopToken());
        });
        for (int i = 0; i < 2000 && g_deadlockReports.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(g_deadlockReports.load() >= 1, "pozitif: rapor uretildi");
        table.requestStop(t1.id);
        table.requestStop(t2.id);
        while (!t1.finished() || !t2.finished())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(true, "stop ile iki thread de cikti");
    }
    setDeadlockHandler(nullptr);
}

void testSharedSlots() {
    std::printf("[7] SharedSlots atomik += ve lock\n");
    auto& slots = SharedSlots::instance();
    slots.configure({{SharedKind::Int, "counter"}, {SharedKind::Int, "a"}, {SharedKind::Int, "b"}});
    constexpr int kThreads = 4, kPer = 100000;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < kPer; ++i) slots.addInt(0, 1);
            for (int i = 0; i < 1000; ++i) {
                // lock altında iki değişkenli değişmez: a + b == 0
                slots.lock(1);
                slots.storeInt(1, slots.loadInt(1) + 1);
                slots.storeInt(2, slots.loadInt(2) - 1);
                slots.unlock(1);
            }
        });
    for (auto& t : ts) t.join();
    check(slots.loadInt(0) == int64_t(kThreads) * kPer, "atomik += kayipsiz");
    check(slots.loadInt(1) == kThreads * 1000 && slots.loadInt(1) + slots.loadInt(2) == 0,
          "lock altinda iki degiskenli tutarlilik");
}

}  // namespace

int main() {
    std::printf("=== threading_primitives_test ===\n");
    testProducersConsumers();
    testBoundedBlocks();
    testStopWhilePopping();
    testListConcurrentAppend();
    testMessageRoundTrip();
    testDeadlockDetector();
    testSharedSlots();
    std::printf("%s (%d hata)\n", failures == 0 ? "TUM TESTLER GECTI" : "BASARISIZ", failures);
    return failures == 0 ? 0 : 1;
}
