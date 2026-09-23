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
//   X(ISIM, ARITE, BACKENDS)
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
//
// Yeni opcode eklemek = bu listeye bir satır eklemek. enum, opcodeName(),
// opcodeArity(), opcodeBackends() ve JIT temel destek filtresi buradan
// türetilir — başka yerde elle senkron switch kalmadı.
// ----------------------------------------------------------------------------

// Backend bayrakları (OPCODE_LIST üçüncü alanı)
enum : uint8_t {
    OP_VM  = 1u << 0,  // VM (normatif) — tüm opcode'lar
    OP_JIT = 1u << 1,  // MIR JIT [EXPERIMENTAL] — ek koşullar mir_backend.cpp'de
};

#define OPCODE_LIST(X) \
    /* --- Değer yükleme --- */ \
    X(LOAD_CONST,  2, OP_VM | OP_JIT) /* slots[dest] = intValue (tam sayı sabiti) */ \
    X(LOAD_STRING, 2, OP_VM | OP_JIT) /* slots[dest] = stringValue */ \
    X(LOAD_NULL,   1, OP_VM | OP_JIT) /* slots[dest] = null (ADR-021); JIT'te yandaş isNull bayrağı (#221) */ \
    X(LOAD_SLOT,   2, OP_VM | OP_JIT) /* slots[dest] = slots[src] */ \
    /* --- Aritmetik (dest = left OP right) --- */ \
    X(ADD, 3, OP_VM | OP_JIT) \
    X(SUB, 3, OP_VM | OP_JIT) \
    X(MUL, 3, OP_VM | OP_JIT) \
    X(DIV, 3, OP_VM | OP_JIT) /* UYARI: sıfıra bölme → runtime_error */ \
    X(MOD, 3, OP_VM | OP_JIT) \
    /* #237: ** üs alma. Tamsayı tabanı tamsayı üsle yükseltir (tekrarlı
       çarpma — libm pow() değil, çünkü pow() büyük değerlerde yuvarlama
       hatası verir ve VM≡JIT bit-birebirliği bozulur). Negatif üs E_POWNEG
       ile hata: tamsayı sonucu kesirli olurdu. */ \
    X(POW,  3, OP_VM | OP_JIT) \
    X(LPOW, 3, OP_VM | OP_JIT) /* longint taban/üs */ \
    /* --- Bitsel (dest = left OP right) --- */ \
    X(BAND, 3, OP_VM | OP_JIT) /* slots[left] & slots[right] */ \
    X(BOR,  3, OP_VM | OP_JIT) /* slots[left] | slots[right] */ \
    X(BXOR, 3, OP_VM | OP_JIT) /* slots[left] ^ slots[right] */ \
    X(SHL,  3, OP_VM | OP_JIT) /* slots[left] << slots[right] */ \
    X(SHR,  3, OP_VM | OP_JIT) /* slots[left] >> slots[right] */ \
    X(BNOT, 2, OP_VM | OP_JIT) /* slots[dest] = ~slots[src] (tekli) */ \
    /* --- Karşılaştırma (sonuç: 1 = doğru, 0 = yanlış) --- */ \
    X(LESS,          3, OP_VM | OP_JIT) \
    X(LESS_EQUAL,    3, OP_VM | OP_JIT) \
    X(GREATER,       3, OP_VM | OP_JIT) \
    X(GREATER_EQUAL, 3, OP_VM | OP_JIT) \
    X(EQUAL_EQUAL,   3, OP_VM | OP_JIT) \
    X(NOT_EQUAL,     3, OP_VM | OP_JIT) \
    /* --- Kontrol akışı --- */ \
    X(JMP,       1, OP_VM | OP_JIT) /* ip = jumpTarget */ \
    X(JIF_FALSE, 2, OP_VM | OP_JIT) /* cond falsy ise ip = jumpTarget */ \
    X(JIF_TRUE,  2, OP_VM | OP_JIT) /* cond truthy ise ip = jumpTarget */ \
    /* --- Fonksiyon çağrısı --- */ \
    X(CALL,   3, OP_VM | OP_JIT) /* dest, functionName, argSlots */ \
    X(RETURN, 1, OP_VM | OP_JIT) /* slots[src]'yi caller'a ilet */ \
    /* --- Float aritmetik (#44) --- */ \
    X(LOAD_FLOAT,   2, OP_VM | OP_JIT) /* slots[dest] = floatValue */ \
    X(FADD, 3, OP_VM | OP_JIT) \
    X(FSUB, 3, OP_VM | OP_JIT) \
    X(FMUL, 3, OP_VM | OP_JIT) \
    X(FDIV, 3, OP_VM | OP_JIT) /* sıfır → runtime_error */ \
    X(FPOW, 3, OP_VM | OP_JIT) /* #237: double üs — libm pow() */ \
    X(FMOD, 3, OP_VM | OP_JIT) /* #241: double % — fmod(); sıfır → Error */ \
    X(FNEG, 2, OP_VM | OP_JIT) /* -slots[src] */ \
    X(INT_TO_FLOAT, 2, OP_VM | OP_JIT) /* gizli int→float */ \
    X(FLOAT_TO_INT, 2, OP_VM | OP_JIT) /* açık cast */ \
    /* --- Float32 aritmetik (ADR-040: tek sonuç (float) truncate) --- */ \
    X(LOAD_FLOAT32,     2, OP_VM | OP_JIT) /* single sabit yükle */ \
    X(F32ADD, 3, OP_VM | OP_JIT) \
    X(F32SUB, 3, OP_VM | OP_JIT) \
    X(F32MUL, 3, OP_VM | OP_JIT) \
    X(F32DIV, 3, OP_VM | OP_JIT) /* sıfır → runtime_error */ \
    X(F32POW, 3, OP_VM | OP_JIT) /* #237: float32 üs — powf() */ \
    X(F32MOD, 3, OP_VM | OP_JIT) /* #241: float32 % — fmodf(); sıfır → Error */ \
    X(F32NEG, 2, OP_VM | OP_JIT) \
    X(INT_TO_FLOAT32,   2, OP_VM | OP_JIT) /* int → float32 */ \
    X(FLOAT32_TO_INT,   2, OP_VM | OP_JIT) /* float32 → int (checked) */ \
    X(FLOAT_TO_FLOAT32, 2, OP_VM | OP_JIT) /* double → float (E003 veri kaybı) */ \
    X(FLOAT32_TO_FLOAT, 2, OP_VM | OP_JIT) /* float → double (kayıpsız) */ \
    /* --- LongInt aritmetik (ADR-040: 64-bit signed, wrap tanımlı) --- */ \
    X(LOAD_LONG, 2, OP_VM | OP_JIT) /* 64-bit sabit yükle */ \
    X(LADD, 3, OP_VM | OP_JIT) \
    X(LSUB, 3, OP_VM | OP_JIT) \
    X(LMUL, 3, OP_VM | OP_JIT) \
    X(LDIV, 3, OP_VM | OP_JIT) /* sıfır → Error; INT64_MIN/-1 → INT64_MIN */ \
    X(LMOD, 3, OP_VM | OP_JIT) /* sıfır → Error; INT64_MIN/-1 → 0 */ \
    X(LNEG, 2, OP_VM | OP_JIT) \
    X(LBAND, 3, OP_VM | OP_JIT) \
    X(LBOR,  3, OP_VM | OP_JIT) \
    X(LBXOR, 3, OP_VM | OP_JIT) \
    X(LSHL,  3, OP_VM | OP_JIT) \
    X(LSHR,  3, OP_VM | OP_JIT) /* aritmetik */ \
    X(LBNOT, 2, OP_VM | OP_JIT) \
    X(INT_TO_LONG,         2, OP_VM | OP_JIT) /* int → longint (kayıpsız) */ \
    X(LONG_TO_INT_CHECKED, 3, OP_VM | OP_JIT) /* int32 aralığı dışı → fallible */ \
    /* --- Struct (ADR-020: referans semantiği) --- */ \
    X(STRUCT_NEW, 3, OP_VM | OP_JIT) /* dest, intValue alan sayısı, functionName tip adı */ \
    X(FIELD_GET,  3, OP_VM | OP_JIT) /* slots[dest] = slots[src].fields[intValue] */ \
    X(FIELD_SET,  3, OP_VM | OP_JIT) /* slots[dest].fields[intValue] = slots[right] */ \
    /* --- Array (ADR-020: referans semantiği; #206 packed elemanlar) --- */ \
    X(ARRAY_NEW, 3, OP_VM | OP_JIT) /* dest, intValue kapasite, arrayElemKind packed tip */ \
    X(ARRAY_GET, 3, OP_VM | OP_JIT) /* slots[dest] = slots[left][slots[right]] — sınır kontrolü */ \
    X(ARRAY_SET, 3, OP_VM | OP_JIT) /* slots[dest][slots[left]] = slots[right] — sınır kontrolü */ \
    X(ARRAY_LEN, 2, OP_VM | OP_JIT) /* slots[dest] = slots[src].uzunluk() */ \
    /* --- Modül-düzeyi değişken erişimi --- */ \
    X(LOAD_GLOBAL,  2, OP_VM | OP_JIT) /* slots[dest] = moduleSlots[intValue] */ \
    X(STORE_GLOBAL, 2, OP_VM | OP_JIT) /* moduleSlots[intValue] = slots[src] */ \
    /* --- String işlemleri (ADR-024: immutable değer-tipi) --- */ \
    X(STRING_CONCAT, 3, OP_VM | OP_JIT) /* slots[dest] = slots[left] + slots[right] */ \
    /* --- Hata yönetimi (ADR-025: UNCHECKED try/catch/throw) --- */ \
    X(ENTER_TRY, 2, OP_VM | OP_JIT) /* dest, jumpTarget; callDepth'i VM kaydeder */ \
    X(LEAVE_TRY, 0, OP_VM | OP_JIT) /* TryFrame'i çıkar (operand yok) */ \
    X(THROW,     1, OP_VM | OP_JIT) /* slots[src] değerini fırlat */ \
    /* --- Tip dönüşümleri (ADR-026: as operatörü) — hatasız --- */ \
    X(CAST_INT_TO_STR,   2, OP_VM | OP_JIT) \
    X(CAST_FLOAT_TO_STR, 2, OP_VM | OP_JIT) \
    X(CAST_BOOL_TO_STR,  2, OP_VM | OP_JIT) \
    /* --- Tip dönüşümleri — fallible (left=0 → Error; left=1 → null) --- */ \
    X(CAST_STR_TO_INT,            3, OP_VM | OP_JIT) \
    X(CAST_STR_TO_FLOAT,          3, OP_VM | OP_JIT) \
    X(CAST_FLOAT_TO_INT_CHECKED,  3, OP_VM | OP_JIT) /* NaN/Inf/taşma → fallible */ \
    X(CAST_INT_TO_BYTE_CHECKED,   3, OP_VM | OP_JIT) /* 0-255 dışı → fallible (#86) */ \
    X(CAST_LONG_TO_STR,           2, OP_VM | OP_JIT) /* longint → string (hatasız) */ \
    X(CAST_STR_TO_LONG,           3, OP_VM | OP_JIT) /* string → longint (fallible) */ \
    X(CAST_FLOAT32_TO_STR,        2, OP_VM | OP_JIT) /* float32 → string (hatasız) */ \
    X(CAST_STR_TO_FLOAT32,        3, OP_VM | OP_JIT) /* string → float32 (fallible) */ \
    X(CAST_FLOAT_TO_LONG_CHECKED, 3, OP_VM | OP_JIT) /* NaN/Inf/int64 taşma → fallible */ \
    /* --- Decimal aritmetik (ADR-028) --- */ \
    X(LOAD_DECIMAL,   2, OP_VM | OP_JIT) /* decimal sabit yükle */ \
    X(DADD, 3, OP_VM | OP_JIT) \
    X(DSUB, 3, OP_VM | OP_JIT) \
    X(DMUL, 3, OP_VM | OP_JIT) \
    X(DDIV, 3, OP_VM | OP_JIT) /* sıfır → Error */ \
    X(DMOD, 3, OP_VM | OP_JIT) /* sıfır → Error */ \
    X(DNEG, 2, OP_VM | OP_JIT) \
    X(INT_TO_DECIMAL,   2, OP_VM | OP_JIT) /* gizli int→decimal terfi */ \
    X(FLOAT_TO_DECIMAL, 2, OP_VM | OP_JIT) /* gizli float→decimal terfi */ \
    X(CAST_DECIMAL_TO_STR,   2, OP_VM | OP_JIT) /* hatasız */ \
    X(CAST_DECIMAL_TO_FLOAT, 2, OP_VM | OP_JIT) /* hatasız */ \
    X(CAST_DECIMAL_TO_INT,   3, OP_VM | OP_JIT) /* trunc; taşma → fallible */ \
    X(CAST_STR_TO_DECIMAL,   3, OP_VM | OP_JIT) /* fallible */ \
    /* --- Dış dünya (FFI) --- */ \
    X(CALLHOST, 2, OP_VM | OP_JIT) /* functionName, argSlots; şu an yalnız print */

// Spec tablosundan türetilen enum — OPCODE_LIST'e satır eklemek yeterlidir.
enum class Opcode {
#define X(name, arity, backends) name,
    OPCODE_LIST(X)
#undef X
};

// Hata ayıklama ve IR dump için okunabilir isim (spec tablosundan türetilir)
inline const char* opcodeName(Opcode op) {
    switch (op) {
#define X(name, arity, backends) case Opcode::name: return #name;
        OPCODE_LIST(X)
#undef X
    }
    return "UNKNOWN";
}

// Operand arite bilgisi (spec tablosundan; tanım üstteki kurala göre)
constexpr int opcodeArity(Opcode op) {
    switch (op) {
#define X(name, arity, backends) case Opcode::name: return arity;
        OPCODE_LIST(X)
#undef X
    }
    return 0;
}

// Destekleyen backend bayrakları (spec tablosundan; VM her zaman normatiftir)
constexpr uint8_t opcodeBackends(Opcode op) {
    switch (op) {
#define X(name, arity, backends) case Opcode::name: return backends;
        OPCODE_LIST(X)
#undef X
    }
    return 0;
}

// MIR JIT temel destek filtresi — talimata bağlı ek koşullar
// mir_backend.cpp::opcodeSupported()'da switch ile uygulanır.
constexpr bool opcodeJitBaseSupported(Opcode op) {
    return (opcodeBackends(op) & OP_JIT) != 0;
}

// Spec tablosundaki toplam opcode sayısı (testler ve iterasyon için)
#define X(name, arity, backends) +1
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
