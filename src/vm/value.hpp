// ============================================================================
// saQut VM — Value (Çalışma Zamanı Değeri)
// ============================================================================
//
// DİZİN:   src/vm/value.hpp
// KATMAN:  VM — Tüm çalışma zamanı değerlerinin ortak temsili
//
// AMAÇ:
//   Int, Float, Bool, String, Decimal, Null ve Ref (heap referansı) değerlerini
//   tek bir Value struct'ında birleştirir. VM'in tüm veri alışverişi Value
//   üzerinden yapılır.
//
// TEMSİL (altyapı denetimi, docs/gc-threading-altyapi-denetimi.md A1.1/A1.2):
//   Payload tek UNYONDA durur — öncesinde kind+int+double+Decimal+string+ref
//   yan yana yaşıyordu (~112 bayt; her kopya tüm alanları taşıyordu).
//
//   TEK STRING MODELİ: String artık Value içinde inline std::string DEĞİL,
//   heap'te yaşayan GC'li bir StringObject'e Object* referansıdır (aynı
//   model JIT tarafında zaten böyleydi — ADR-037; iki backend artık aynı
//   temsili paylaşır). Sonuçlar:
//     - Value 24 bayt ve TAMAMEN düz-kopyalanır (string kopya/yıkım kodu
//       yok; string paylaşımı güvenlidir çünkü string immutable — ADR-024)
//     - markValue String'i Ref gibi kökler; string'ler artık TOPLANABİLİR
//     - fromString tahsisi allocValueString kanca fonksiyonuyla yapar:
//       aktif Heap bağlıysa GC'li yola, değilse (test/izole kullanım)
//       süresiz-tutulan yedek havuza (object.cpp). Kanca thread_local'dır.
//
//   Yazmak yalnız fabrikalarla (fromInt/fromString/...) olur — kod tabanında
//   dışarıdan alan yazan yoktur, bu sözleşme korunur.
// ============================================================================

#ifndef SAQUT_VM_VALUE
#define SAQUT_VM_VALUE

#include <string>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <new>
#include "core/decimal.hpp"
#include "core/float_format.hpp"

// Forward — Object tam tanımı object.hpp'de; Value onu pointer olarak taşır.
struct Object;
// String'in heap temsili (tek-string-modeli) ve tahsis kancası —
// gerçeklemede object.cpp. Kanca, aktif Heap'e (yoksa yedek havuza) tahsis
// eder; Value bu başlıktan object.hpp'ye bağımlı olmadan çağırabilir.
struct StringObject;   // tanım object.hpp’te (yalnız ileri bildirim yeter)
Object* allocValueString(std::string s);  // kanca: GC’li heap / yedek havuz

// ADR-020: Primitive (int/bool) = değer; bileşik (array/struct/string) = referans.
// ADR-021: Null = nullable referansların null değeri (saQut'ta `null` anahtar sözcüğü).
// Bool ayrı kind değil — boolean sonuçlar int olarak saklanır (0=yanlış, sıfır-dışı=doğru).
enum class ValueKind {
    Int,
    LongInt,  // ADR-040: 64-bit işaretli tamsayı; int64Value() taşır
    Float,    // ADR-040: 64-bit IEEE double; floatValue() taşır
    Float32,  // ADR-040: 32-bit IEEE single; floatValue() tutar ama (float) truncate edilir
    Decimal,  // ADR-028: ondalık hassasiyet; decimalValue() taşır
    String,
    Ref,      // ADR-020: array/struct nesnesine Object* referansı
    Null,     // ADR-021: nullable referansın null değeri (saQut kaynağında `null`)
    Date,     // #88 (ADR-036): UTC epoch-ms; int64Value() taşır
};

struct Value {
private:
    // Unyon üyeleri kind'a göre yorumlanır:
    //   i   → Int (alt 32 bit), LongInt, Date
    //   d   → Float, Float32
    //   dec → Decimal
    //   r   → Ref (array/struct) VE String (StringObject) — tek string
    //         modeli: string de GC'li heap nesnesine işaret eder
    // Bütün üyeler düz-kopyalanabilir olduğundan Value'nun özel ömür
    // yönetimi YOKTUR (string artık unyonda değil).
    union Payload {
        long long    i;
        double       d;
        DecimalValue dec;
        Object*      r;
        Payload() : i(0) {}
    } p;

public:
    ValueKind kind = ValueKind::Int;

    Value() = default;
    // ── Fabrikalar: Value'ya alan yazmanın TEK yolu ─────────────────────────
    static Value fromInt(int n) {
        Value v; v.kind = ValueKind::Int; v.p.i = n; return v;
    }
    static Value fromLongInt(long long n) {
        Value v; v.kind = ValueKind::LongInt; v.p.i = n; return v;
    }
    static Value fromDate(long long epochMs) {
        Value v; v.kind = ValueKind::Date; v.p.i = epochMs; return v;
    }
    static Value fromFloat(double d) {
        Value v; v.kind = ValueKind::Float; v.p.d = d; return v;
    }
    // ADR-040: 32-bit single — saklarken (float) truncate ederek gerçek precision
    static Value fromFloat32(double d) {
        Value v; v.kind = ValueKind::Float32; v.p.d = (double)(float)d; return v;
    }
    static Value fromDecimal(const DecimalValue& d) {
        Value v; v.kind = ValueKind::Decimal; v.p.dec = d; return v;
    }
    static Value fromString(std::string s) {
        Value v; v.kind = ValueKind::String;
        v.p.r = allocValueString(std::move(s));
        return v;
    }
    static Value fromRef(Object* obj) {
        Value v; v.kind = ValueKind::Ref; v.p.r = obj; return v;
    }
    static Value null() {
        Value v; v.kind = ValueKind::Null; v.p.i = 0; return v;
    }

    // ── Okuyucular: eski alan adları, artık metot — okuma sözdizimi değişmez ─
    int               intValue()     const { return (int)p.i; }
    long long         int64Value()   const { return p.i; }
    double            floatValue()   const { return p.d; }
    const DecimalValue& decimalValue() const { return p.dec; }
    // Tek string modeli: String kind'ı r üyesindeki StringObject'e işaret
    // eder. Yalnız kind==String iken çağrılmalı (tüm çağıranlar böyle).
    const std::string&  stringValue()  const;  // gövde object.cpp'te (StringObject tam tipi)
    Object*           ref()          const { return p.r; }

    // ADR-040: sayısal karşılaştırma için ortak erişim. Int/LongInt tamsayı
    // alanından, Float/Float32 double alanından okur; karışık int↔float
    // karşılaştırmaları double üzerinden yapılır (typechecker izin verdiği ölçüde).
    bool isFloaty()  const { return kind == ValueKind::Float || kind == ValueKind::Float32; }
    bool isIntegral() const { return kind == ValueKind::Int || kind == ValueKind::LongInt; }
    // Date bir 64-bit epoch-ms'tir: int32'ye kırpılmamalı (#242).
    long long asI64() const {
        return (kind == ValueKind::LongInt || kind == ValueKind::Date) ? p.i : (long long)(int)p.i;
    }
    double asDouble() const {
        if (isFloaty()) return p.d;
        if (kind == ValueKind::LongInt || kind == ValueKind::Date) return (double)p.i;
        return (double)(int)p.i;
    }

    bool isTruthy() const {
        switch (kind) {
            case ValueKind::Int:     return p.i != 0;
            case ValueKind::LongInt: return p.i != 0;
            case ValueKind::Float:   return p.d != 0.0;
            case ValueKind::Float32: return p.d != 0.0;
            case ValueKind::Decimal: return p.dec.isTruthy();
            case ValueKind::String:  return !stringValue().empty();
            case ValueKind::Ref:     return p.r != nullptr;
            case ValueKind::Null:    return false;
            case ValueKind::Date:    return true; // her zaman geçerli bir andı temsil eder
        }
        return false;
    }

    std::string toString() const {
        switch (kind) {
            case ValueKind::Int:     return std::to_string((int)p.i);
            case ValueKind::LongInt: return std::to_string(p.i);
            case ValueKind::Decimal: return p.dec.toString();
            case ValueKind::Float:
            case ValueKind::Float32: {
                // #114: biçim tek kaynaktan (core/float_format.hpp) — VM/JIT senkronu
                return (kind == ValueKind::Float32)
                    ? formatFloat32Print(p.d)
                    : formatDoublePrint(p.d);
            }
            case ValueKind::String: return stringValue();
            case ValueKind::Ref:   return "<ref>";
            case ValueKind::Null:  return "null";
            case ValueKind::Date:  return std::to_string(p.i);
        }
        return "?";
    }

    std::string typeName() const {
        switch (kind) {
            case ValueKind::Int:     return "int";
            case ValueKind::LongInt: return "longint";
            case ValueKind::Float:   return "float";
            case ValueKind::Float32: return "float";
            case ValueKind::Decimal: return "decimal";
            case ValueKind::String:  return "string";
            case ValueKind::Ref:     return "ref";
            case ValueKind::Null:    return "null";
            case ValueKind::Date:    return "date";
        }
        return "?";
    }
};

// Temsil sözleşmesi: Value kompakt kalmalıdır. Bu eşik aşılırsa (unyona büyük
// üye eklendiyse) kopyaların maliyeti sessizce büyür — derleme zamanı uyarır.
static_assert(sizeof(Value) <= 24, "Value unyon temsili 24 baytı aşmamalı");

#endif // SAQUT_VM_VALUE
