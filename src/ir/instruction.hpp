// ============================================================================
// saQut IR — Instruction (Tek Talimat)
//
// Sanal makine bu talimatlara bakarak ne yapacağını anlar.
// Her talimatın bir "opcode"u (ne iş yapacağı) ve birkaç operandı vardır.
// Operandlar ya slot numarasıdır (fonksiyonun yerel değişken/geçici depoları)
// ya da doğrudan bir sayı/isim değeridir.
//
// SLOT NEDİR?
//   Her fonksiyon çağrısı kendi "frame"ini açar.
//   Frame içinde numaralı kutucuklar vardır: slot[0], slot[1], ...
//   Parametreler slot 0'dan başlar. Sonrasında lokal değişkenler
//   ve hesaplama sırasında oluşan geçici değerler gelir.
//   "slots[5] = 42" demek "5 numaralı kutucuğa 42 değerini koy" demektir.
//
// HANGİ OPCODE HANGİ ALANI KULLANIR?
//   LOAD_CONST     : dest, intValue
//   LOAD_SLOT      : dest, src
//   ADD/SUB/...    : dest, left, right
//   LESS/LEQ/...   : dest, left, right       (sonuç: 1=doğru, 0=yanlış)
//   JMP            : jumpTarget
//   JIF_FALSE      : cond, jumpTarget
//   JIF_TRUE       : cond, jumpTarget
//   CALL           : dest, functionName, argSlots
//   RETURN         : src
//   CALLHOST       : functionName, argSlots
// ============================================================================

#ifndef SAQUT_IR_INSTRUCTION
#define SAQUT_IR_INSTRUCTION

#include <string>
#include <vector>
#include "core/array_elem_kind.hpp"
#include "core/decimal.hpp"

// ----------------------------------------------------------------------------
// SlotType — bir slot'un statik değer türü (ADR-020: slot çalışma zamanında tip
// değiştirmez). Amaçlar: (1) MIR JIT register tipi seçimi (Float→MIR_T_D, diğerleri
// →I64, MIRPLAN §3); (2) cam kutu `saqut ir --types`. VM bu alanı kullanmaz (Value
// zaten kind taşır). IR katmanında — ValueKind'a KASITLI bağımsız (ADR-021).
// instruction.hpp'de tanımlı çünkü Instruction::valueType (ADR-039) buna ihtiyaç duyar.
// ----------------------------------------------------------------------------
enum class SlotType : uint8_t { Int, LongInt, Float, Float32, Ref, Str, Decimal, Date, Unknown };

inline const char* slotTypeName(SlotType t) {
    switch (t) {
        case SlotType::Int:     return "int";
        case SlotType::LongInt: return "longint";
        case SlotType::Float:   return "float";
        case SlotType::Float32: return "float32";
        case SlotType::Ref:     return "ref";
        case SlotType::Str:     return "string";
        case SlotType::Decimal: return "decimal";
        case SlotType::Date:    return "date";
        case SlotType::Unknown: return "?";
    }
    return "?";
}

// ----------------------------------------------------------------------------
// OPCODE_LIST — Opcode spec tablosu (TEK KAYNAK, #132)
//
// Her opcode tam olarak tek satırda tanımlanır:
//   X(ISIM, ARITE, BACKENDS, SONUC)
//     ISIM     : kanonik opcode adı (enum değeri; sıralama pozisyoneldir,
//                eklerken listenin SONUNA değil doğru gruba ekle)
//     ARITE    : talimatın anlamlı operand alanı sayısı. Kural:
//                  - slot operandları (dest/src/left/right/cond) ve sabitler
//                    (intValue/int64Value/floatValue/decimalValue/stringValue)
//                    teker teker sayılır
//                  - callee (functionName + argSlots) tek operand sayılır
//                  - fallible cast'lerde nullable-modu bayrağı (left) bir
//                    operand sayılır
//                  - ARRAY_NEW'de arrayElemKind (packed eleman tipi) bir
//                    operand sayılır
//                  - IR-metadata (valueType, fieldNames, source*)
//                    SAYILMAZ
//     BACKENDS : destekleyen backend bayrakları (OP_VM | OP_JIT). VM
//                normatif backend'dir ve TÜM opcode'ları çalıştırır; JIT
//                [EXPERIMENTAL] — tablodaki OP_JIT yalnızca "temel" desteği
//                gösterir, talimata bağlı ek koşullar
//                (mir_backend.cpp::opcodeSupported'da) ayrıca uygulanır.
//     SONUC    : talimatın dest slotuna yazdığı değerin türü (OpResult, aşağıda).
//                IRGenerator::finalizeSlotTypes slot tiplerini buradan
//                çıkarır; JIT register türünü bu tiplere göre seçer. Yanlış
//                sütun derleme hatası değil, --jit'te yanlış değerdir: yeni
//                opcode'un sonucunu dikkatle seç (#297).
//
// Yeni opcode eklemek = bu listeye bir satır eklemek. enum, opcodeName(),
// opcodeArity(), opcodeBackends(), opcodeResult() ve JIT temel destek
// filtresi buradan türetilir. Opcode'un GÖVDESİ ayrıca yazılır: VM
// (interpreter.cpp), IR yazdırma (ir_function.cpp — default'suz switch,
// -Werror=switch: eksik dal derleme hatası) ve JIT desteklenecekse
// mir_backend.cpp.
// ----------------------------------------------------------------------------

// Backend bayrakları (OPCODE_LIST üçüncü alanı)
enum : uint8_t {
    OP_VM  = 1u << 0,  // VM (normatif) — tüm opcode'lar
    OP_JIT = 1u << 1,  // MIR JIT [EXPERIMENTAL] — ek koşullar mir_backend.cpp'de
};

// Sonuç türü (OPCODE_LIST dördüncü alanı): talimatın dest slotuna ne yazdığı.
enum class OpResult : uint8_t {
    None,       // dest'e yazmaz (FIELD_SET/ARRAY_SET'te dest okunan nesnedir)
    Int,        // int / bool / byte / enum değeri
    Long,       // longint
    Float,      // double
    Float32,    // float
    Decimal,
    Str,
    Ref,        // struct / array / Error referansı
    ValueType,  // tip Instruction::valueType'ta (eleman / alan / global / shared)
    Copy,       // LOAD_SLOT: kaynak slotun tipi
    Call,       // CALL: çağrılan saQut fonksiyonunun dönüş tipi
    Host,       // CALLHOST: valueType, yoksa host kaydının retKind'ı
    Null,       // LOAD_NULL: tip taşımaz (null her tipte olabilir)
};

#define OPCODE_LIST(X) \
    /* --- Değer yükleme --- */ \
    X(LOAD_CONST,  2, OP_VM | OP_JIT, Int)  /* slots[dest] = intValue (tam sayı sabiti) */ \
    X(LOAD_STRING, 2, OP_VM | OP_JIT, Str)  /* slots[dest] = stringValue */ \
    X(LOAD_NULL,   1, OP_VM | OP_JIT, Null) /* slots[dest] = null (ADR-021); JIT'te yandaş isNull bayrağı (#221) */ \
    X(LOAD_SLOT,   2, OP_VM | OP_JIT, Copy) /* slots[dest] = slots[src] */ \
    /* --- Aritmetik (dest = left OP right) --- */ \
    X(ADD, 3, OP_VM | OP_JIT, Int) \
    X(SUB, 3, OP_VM | OP_JIT, Int) \
    X(MUL, 3, OP_VM | OP_JIT, Int) \
    X(DIV, 3, OP_VM | OP_JIT, Int) /* UYARI: sıfıra bölme → runtime_error */ \
    X(MOD, 3, OP_VM | OP_JIT, Int) \
    /* #237: ** üs alma. Tamsayı tabanı tamsayı üsle yükseltir (tekrarlı
       çarpma — libm pow() değil, çünkü pow() büyük değerlerde yuvarlama
       hatası verir ve VM≡JIT bit-birebirliği bozulur). Negatif üs E_POWNEG
       ile hata: tamsayı sonucu kesirli olurdu. */ \
    X(POW,  3, OP_VM | OP_JIT, Int) \
    X(LPOW, 3, OP_VM | OP_JIT, Long) /* longint taban/üs */ \
    /* --- Bitsel (dest = left OP right) --- */ \
    X(BAND, 3, OP_VM | OP_JIT, Int) /* slots[left] & slots[right] */ \
    X(BOR,  3, OP_VM | OP_JIT, Int) /* slots[left] | slots[right] */ \
    X(BXOR, 3, OP_VM | OP_JIT, Int) /* slots[left] ^ slots[right] */ \
    X(SHL,  3, OP_VM | OP_JIT, Int) /* slots[left] << slots[right] */ \
    X(SHR,  3, OP_VM | OP_JIT, Int) /* slots[left] >> slots[right] */ \
    X(BNOT, 2, OP_VM | OP_JIT, Int) /* slots[dest] = ~slots[src] (tekli) */ \
    /* --- Karşılaştırma (sonuç: 1 = doğru, 0 = yanlış) --- */ \
    X(LESS,          3, OP_VM | OP_JIT, Int) \
    X(LESS_EQUAL,    3, OP_VM | OP_JIT, Int) \
    X(GREATER,       3, OP_VM | OP_JIT, Int) \
    X(GREATER_EQUAL, 3, OP_VM | OP_JIT, Int) \
    X(EQUAL_EQUAL,   3, OP_VM | OP_JIT, Int) \
    X(NOT_EQUAL,     3, OP_VM | OP_JIT, Int) \
    /* --- Kontrol akışı --- */ \
    X(JMP,       1, OP_VM | OP_JIT, None) /* ip = jumpTarget */ \
    X(JIF_FALSE, 2, OP_VM | OP_JIT, None) /* cond falsy ise ip = jumpTarget */ \
    X(JIF_TRUE,  2, OP_VM | OP_JIT, None) /* cond truthy ise ip = jumpTarget */ \
    /* --- Fonksiyon çağrısı --- */ \
    X(CALL,   3, OP_VM | OP_JIT, Call) /* dest, functionName, argSlots */ \
    X(RETURN, 1, OP_VM | OP_JIT, None) /* slots[src]'yi caller'a ilet */ \
    /* --- Float aritmetik (#44) --- */ \
    X(LOAD_FLOAT,   2, OP_VM | OP_JIT, Float) /* slots[dest] = floatValue */ \
    X(FADD, 3, OP_VM | OP_JIT, Float) \
    X(FSUB, 3, OP_VM | OP_JIT, Float) \
    X(FMUL, 3, OP_VM | OP_JIT, Float) \
    X(FDIV, 3, OP_VM | OP_JIT, Float) /* sıfır → runtime_error */ \
    X(FPOW, 3, OP_VM | OP_JIT, Float) /* #237: double üs — libm pow() */ \
    X(FMOD, 3, OP_VM | OP_JIT, Float) /* #241: double % — fmod(); sıfır → Error */ \
    X(FNEG, 2, OP_VM | OP_JIT, Float) /* -slots[src] */ \
    X(INT_TO_FLOAT, 2, OP_VM | OP_JIT, Float) /* gizli int→float */ \
    X(FLOAT_TO_INT, 2, OP_VM | OP_JIT, Int)   /* açık cast */ \
    /* --- Float32 aritmetik (ADR-040: tek sonuç (float) truncate) --- */ \
    X(LOAD_FLOAT32,     2, OP_VM | OP_JIT, Float32) /* single sabit yükle */ \
    X(F32ADD, 3, OP_VM | OP_JIT, Float32) \
    X(F32SUB, 3, OP_VM | OP_JIT, Float32) \
    X(F32MUL, 3, OP_VM | OP_JIT, Float32) \
    X(F32DIV, 3, OP_VM | OP_JIT, Float32) /* sıfır → runtime_error */ \
    X(F32POW, 3, OP_VM | OP_JIT, Float32) /* #237: float32 üs — powf() */ \
    X(F32MOD, 3, OP_VM | OP_JIT, Float32) /* #241: float32 % — fmodf(); sıfır → Error */ \
    X(F32NEG, 2, OP_VM | OP_JIT, Float32) \
    X(INT_TO_FLOAT32,   2, OP_VM | OP_JIT, Float32) /* int → float32 */ \
    X(FLOAT32_TO_INT,   2, OP_VM | OP_JIT, Int)     /* float32 → int (checked) */ \
    X(FLOAT_TO_FLOAT32, 2, OP_VM | OP_JIT, Float32) /* double → float (E003 veri kaybı) */ \
    X(FLOAT32_TO_FLOAT, 2, OP_VM | OP_JIT, Float)   /* float → double (kayıpsız) */ \
    /* --- LongInt aritmetik (ADR-040: 64-bit signed, wrap tanımlı) --- */ \
    X(LOAD_LONG, 2, OP_VM | OP_JIT, Long) /* 64-bit sabit yükle */ \
    X(LADD, 3, OP_VM | OP_JIT, Long) \
    X(LSUB, 3, OP_VM | OP_JIT, Long) \
    X(LMUL, 3, OP_VM | OP_JIT, Long) \
    X(LDIV, 3, OP_VM | OP_JIT, Long) /* sıfır → Error; INT64_MIN/-1 → INT64_MIN */ \
    X(LMOD, 3, OP_VM | OP_JIT, Long) /* sıfır → Error; INT64_MIN/-1 → 0 */ \
    X(LNEG, 2, OP_VM | OP_JIT, Long) \
    X(LBAND, 3, OP_VM | OP_JIT, Long) \
    X(LBOR,  3, OP_VM | OP_JIT, Long) \
    X(LBXOR, 3, OP_VM | OP_JIT, Long) \
    X(LSHL,  3, OP_VM | OP_JIT, Long) \
    X(LSHR,  3, OP_VM | OP_JIT, Long) /* aritmetik */ \
    X(LBNOT, 2, OP_VM | OP_JIT, Long) \
    X(INT_TO_LONG,         2, OP_VM | OP_JIT, Long) /* int → longint (kayıpsız) */ \
    X(LONG_TO_INT_CHECKED, 3, OP_VM | OP_JIT, Int)  /* int32 aralığı dışı → fallible */ \
    /* --- Struct (ADR-020: referans semantiği) --- */ \
    X(STRUCT_NEW, 3, OP_VM | OP_JIT, Ref)       /* dest, intValue alan sayısı, functionName tip adı */ \
    X(FIELD_GET,  3, OP_VM | OP_JIT, ValueType) /* slots[dest] = slots[src].fields[intValue] */ \
    X(FIELD_SET,  3, OP_VM | OP_JIT, None)      /* slots[dest].fields[intValue] = slots[right] */ \
    /* --- Array (ADR-020: referans semantiği; #206 packed elemanlar) --- */ \
    X(ARRAY_NEW, 3, OP_VM | OP_JIT, Ref)       /* dest, intValue kapasite, arrayElemKind packed tip */ \
    X(ARRAY_GET, 3, OP_VM | OP_JIT, ValueType) /* slots[dest] = slots[left][slots[right]] — sınır kontrolü */ \
    X(ARRAY_SET, 3, OP_VM | OP_JIT, None)      /* slots[dest][slots[left]] = slots[right] — sınır kontrolü */ \
    X(ARRAY_LEN, 2, OP_VM | OP_JIT, Int)       /* slots[dest] = slots[src].uzunluk() */ \
    /* --- Modül-düzeyi değişken erişimi --- */ \
    X(LOAD_GLOBAL,  2, OP_VM | OP_JIT, ValueType) /* slots[dest] = moduleSlots[intValue] */ \
    X(STORE_GLOBAL, 2, OP_VM | OP_JIT, None)      /* moduleSlots[intValue] = slots[src] */ \
    /* --- String işlemleri (ADR-024: immutable değer-tipi) --- */ \
    X(STRING_CONCAT, 3, OP_VM | OP_JIT, Str) /* slots[dest] = slots[left] + slots[right] */ \
    /* --- Hata yönetimi (ADR-025: UNCHECKED try/catch/throw) --- */ \
    X(ENTER_TRY, 2, OP_VM | OP_JIT, Ref)  /* dest (catch'teki Error referansı), jumpTarget (#260) */ \
    X(LEAVE_TRY, 0, OP_VM | OP_JIT, None) /* TryFrame'i çıkar (operand yok) */ \
    X(THROW,     1, OP_VM | OP_JIT, None) /* slots[src] değerini fırlat */ \
    /* --- Tip dönüşümleri (ADR-026: as operatörü) — hatasız --- */ \
    X(CAST_INT_TO_STR,   2, OP_VM | OP_JIT, Str) \
    X(CAST_FLOAT_TO_STR, 2, OP_VM | OP_JIT, Str) \
    X(CAST_BOOL_TO_STR,  2, OP_VM | OP_JIT, Str) \
    /* --- Tip dönüşümleri — fallible (left=0 → Error; left=1 → null) --- */ \
    X(CAST_STR_TO_INT,            3, OP_VM | OP_JIT, Int) \
    X(CAST_STR_TO_FLOAT,          3, OP_VM | OP_JIT, Float) \
    X(CAST_FLOAT_TO_INT_CHECKED,  3, OP_VM | OP_JIT, Int)     /* NaN/Inf/taşma → fallible */ \
    X(CAST_INT_TO_BYTE_CHECKED,   3, OP_VM | OP_JIT, Int)     /* 0-255 dışı → fallible (#86) */ \
    X(CAST_LONG_TO_STR,           2, OP_VM | OP_JIT, Str)     /* longint → string (hatasız) */ \
    X(CAST_STR_TO_LONG,           3, OP_VM | OP_JIT, Long)    /* string → longint (fallible) */ \
    X(CAST_FLOAT32_TO_STR,        2, OP_VM | OP_JIT, Str)     /* float32 → string (hatasız) */ \
    X(CAST_STR_TO_FLOAT32,        3, OP_VM | OP_JIT, Float32) /* string → float32 (fallible) */ \
    X(CAST_FLOAT_TO_LONG_CHECKED, 3, OP_VM | OP_JIT, Long)    /* NaN/Inf/int64 taşma → fallible */ \
    /* --- Decimal aritmetik (ADR-028) --- */ \
    X(LOAD_DECIMAL,   2, OP_VM | OP_JIT, Decimal) /* decimal sabit yükle */ \
    X(DADD, 3, OP_VM | OP_JIT, Decimal) \
    X(DSUB, 3, OP_VM | OP_JIT, Decimal) \
    X(DMUL, 3, OP_VM | OP_JIT, Decimal) \
    X(DDIV, 3, OP_VM | OP_JIT, Decimal) /* sıfır → Error */ \
    X(DMOD, 3, OP_VM | OP_JIT, Decimal) /* sıfır → Error */ \
    X(DNEG, 2, OP_VM | OP_JIT, Decimal) \
    X(INT_TO_DECIMAL,   2, OP_VM | OP_JIT, Decimal) /* gizli int→decimal terfi */ \
    X(FLOAT_TO_DECIMAL, 2, OP_VM | OP_JIT, Decimal) /* gizli float→decimal terfi */ \
    X(CAST_DECIMAL_TO_STR,   2, OP_VM | OP_JIT, Str)     /* hatasız */ \
    X(CAST_DECIMAL_TO_FLOAT, 2, OP_VM | OP_JIT, Float)   /* hatasız */ \
    X(CAST_DECIMAL_TO_INT,   3, OP_VM | OP_JIT, Int)     /* trunc; taşma → fallible */ \
    X(CAST_STR_TO_DECIMAL,   3, OP_VM | OP_JIT, Decimal) /* fallible */ \
    /* --- Dış dünya: print, curated FFI ve built-in metotlar (intValue = host kaydı) --- */ \
    X(CALLHOST, 2, OP_VM | OP_JIT, Host) /* functionName, argSlots */ \
    /* --- İzole thread modeli (ADR-045). intValue = shared slot indeksi \
       (SharedSlots); valueType = sonuç/eleman SlotType. Bloklayanlar \
       (POOL_PUSH/POP, WAIT, THREAD_JOIN) iptal noktasıdır. --- */ \
    X(SHARED_LOAD,    2, OP_VM | OP_JIT, ValueType) /* slots[dest] = shared[intValue] (atomik) */ \
    X(SHARED_STORE,   2, OP_VM | OP_JIT, None)      /* shared[intValue] = slots[src] (atomik) */ \
    X(SHARED_RMW,     3, OP_VM | OP_JIT, ValueType) /* slots[dest] = (shared[intValue] += / -= slots[src]); int64Value: 0 ekle, 1 çıkar */ \
    X(LOCK,           1, OP_VM | OP_JIT, None)      /* shared[intValue] kilidini al */ \
    X(UNLOCK,         1, OP_VM | OP_JIT, None)      /* shared[intValue] kilidini bırak */ \
    X(POOL_PUSH,      2, OP_VM | OP_JIT, None)      /* Pool shared[intValue].push(deep copy slots[src]) — bloklar */ \
    X(POOL_POP,       2, OP_VM | OP_JIT, ValueType) /* slots[dest] = Pool shared[intValue].pop() — bloklar */ \
    X(POOL_LEN,       2, OP_VM | OP_JIT, Int)       /* slots[dest] = Pool shared[intValue].length() */ \
    X(POOL_SETMAX,    2, OP_VM | OP_JIT, None)      /* Pool shared[intValue].setMax(slots[src]) */ \
    X(LIST_APPEND,    2, OP_VM | OP_JIT, None)      /* List shared[intValue].append(deep copy slots[src]) */ \
    X(LIST_GET,       3, OP_VM | OP_JIT, ValueType) /* slots[dest] = List shared[intValue].get(slots[left]) */ \
    X(LIST_LEN,       2, OP_VM | OP_JIT, Int)       /* slots[dest] = List shared[intValue].length() */ \
    X(SHARED_EPOCH,   1, OP_VM | OP_JIT, Long)      /* slots[dest] = park katmanı epoch'u (wait döngüsü) */ \
    X(WAIT,           1, OP_VM | OP_JIT, None)      /* epoch slots[src]'den farklı olana dek park — bloklar */ \
    X(THREAD_SPAWN,   3, OP_VM | OP_JIT, Int)       /* slots[dest] = spawn(functionName, argSlots kopyası) — tamsayı handle */ \
    X(THREAD_ARG,     2, OP_VM | OP_JIT, ValueType) /* slots[dest] = başlangıç argümanı[intValue] */ \
    X(THREAD_STOP,    1, OP_VM | OP_JIT, None)      /* Thread slots[src].stop() — bloklamaz */ \
    X(THREAD_JOIN,    1, OP_VM | OP_JIT, None)      /* Thread slots[src].join() — bloklar */ \
    X(THREAD_RUNNING, 2, OP_VM | OP_JIT, Int)       /* slots[dest] = Thread slots[src].running() */

// Spec tablosundan türetilen enum — OPCODE_LIST'e satır eklemek yeterlidir.
enum class Opcode {
#define X(name, arity, backends, result) name,
    OPCODE_LIST(X)
#undef X
};

// Hata ayıklama ve IR dump için okunabilir isim (spec tablosundan türetilir)
inline const char* opcodeName(Opcode op) {
    switch (op) {
#define X(name, arity, backends, result) case Opcode::name: return #name;
        OPCODE_LIST(X)
#undef X
    }
    return "UNKNOWN";
}

// Operand arite bilgisi (spec tablosundan; tanım üstteki kurala göre)
constexpr int opcodeArity(Opcode op) {
    switch (op) {
#define X(name, arity, backends, result) case Opcode::name: return arity;
        OPCODE_LIST(X)
#undef X
    }
    return 0;
}

// Destekleyen backend bayrakları (spec tablosundan; VM her zaman normatiftir)
constexpr uint8_t opcodeBackends(Opcode op) {
    switch (op) {
#define X(name, arity, backends, result) case Opcode::name: return backends;
        OPCODE_LIST(X)
#undef X
    }
    return 0;
}

// Dest slotuna yazılan değerin türü (spec tablosundan; OpResult)
constexpr OpResult opcodeResult(Opcode op) {
    switch (op) {
#define X(name, arity, backends, result) case Opcode::name: return OpResult::result;
        OPCODE_LIST(X)
#undef X
    }
    return OpResult::None;
}

// MIR JIT temel destek filtresi — talimata bağlı ek koşullar
// mir_backend.cpp::opcodeSupported()'da switch ile uygulanır.
constexpr bool opcodeJitBaseSupported(Opcode op) {
    return (opcodeBackends(op) & OP_JIT) != 0;
}

// Spec tablosundaki toplam opcode sayısı (testler ve iterasyon için)
#define X(name, arity, backends, result) +1
constexpr int kOpcodeCount = OPCODE_LIST(X);
#undef X

// ----------------------------------------------------------------------------
// Instruction — Tek bir IR talimatı
//
// Okunabilirlik öncelikli bir tasarım: her talimat TÜM alanları içerir,
// kullanılmayanlar varsayılan değerde (-1 veya boş) kalır.
// Bu yaklaşım bellek israfeder ama her talimatın hangi veriyle çalıştığı
// açıkça görünür — karmaşık union/variant yapısı gerekmez.
// ----------------------------------------------------------------------------
struct Instruction {
    Opcode opcode;

    // Hedef slot — sonucun yazılacağı yer (LOAD_CONST, ADD, CALL vb.)
    int dest       = -1;

    // Kaynak slot — kopyalama veya döndürme için (LOAD_SLOT, RETURN)
    int src        = -1;

    // Aritmetik/karşılaştırma operandları
    int left       = -1;
    int right      = -1;

    // LOAD_CONST için yüklenecek tam sayı sabiti
    int         intValue    =  0;

    // LOAD_LONG için yüklenecek 64-bit tam sayı sabiti (ADR-040)
    long long   int64Value  =  0;

    // LOAD_FLOAT / LOAD_FLOAT32 için yüklenecek double sabiti (#44; float32'de (float) truncate)
    double       floatValue   = 0.0;

    // LOAD_DECIMAL için yüklenecek decimal sabiti (ADR-028)
    DecimalValue decimalValue;

    // LOAD_STRING için yüklenecek metin sabiti (tırnak işaretleri olmadan)
    std::string stringValue;

    // JMP / JIF_FALSE için hedef instruction indeksi
    // Üretim sırasında bilinmiyorsa -1 bırakılır, sonradan doldurulur (backpatch).
    int jumpTarget = -1;

    // JIF_FALSE için kontrol edilecek koşul slotu
    int cond       = -1;

    // CALL / CALLHOST için çağrılacak fonksiyonun adı
    std::string functionName;

    // CALL / CALLHOST için argüman slot indeksleri (sırayla)
    std::vector<int> argSlots;

    // STRUCT_NEW için alan adları (sırasıyla) — toJson/dump'ta kullanılır
    std::vector<std::string> fieldNames;

    // #206: ARRAY_NEW için packed eleman tipi. Sadece ARRAY_NEW'de anlamlı;
    // diğer opcode'larda ArrayElemKind::Ref kullanılır (ignored).
    ArrayElemKind arrayElemKind = ArrayElemKind::Ref;

    // ADR-039: GET-tarafı opcode'ların sonuç/eleman türü — FIELD_GET / ARRAY_GET /
    // LOAD_GLOBAL dest tipi, ARRAY_NEW eleman tipi. IR'de kaybolan tip bilgisini
    // taşır (kaynak heap/global olduğu için opcode'dan türetilemez). finalizeSlotTypes
    // GET dest'ini buradan çözer; JIT register/köprü tipi buradan seçer. SET-tarafı
    // (FIELD_SET/ARRAY_SET/STORE_GLOBAL) gerektirmez — değer slot'undan bilinir.
    SlotType valueType = SlotType::Unknown;
    bool valueNullable = false;

    // Kaynak konum — yalnızca hata-odaklı opcode'larda (CALL, RETURN, THROW,
    // ARRAY_GET/SET, FIELD_SET) set edilir. filePath IRFunction::moduleId'den
    // türetilir; burada sadece satır/sütun tutulur.
    int         sourceLine = 0;
    int         sourceCol  = 0;
    std::string sourceFile;
    // Hata ayıklayıcı bu komutta durmaz ve adımlamada onu satır sınırı
    // saymaz: `main`'in başına enjekte edilen global başlatıcılar. Satır
    // bilgisi hata mesajları için korunur (D-1: eskiden stopOnEntry `main`
    // yerine global başlatıcının satırında duruyordu).
    bool        debugHidden = false;

    explicit Instruction(Opcode op) : opcode(op) {}
};

#endif // SAQUT_IR_INSTRUCTION
