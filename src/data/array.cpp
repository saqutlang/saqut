// ============================================================================
// saQut — array veri tipi
// ============================================================================
//
// DİZİN:   src/data/array.cpp
// KATMAN:  data — array'in çalışma zamanındaki TEK sahibi
//
// SEMANTİK (ADR-020): array REFERANS tiplidir. push/insert/remove/reverse/clear
//   receiver'ı YERİNDE değiştirir (HOST_MUTATING); slice/concat yeni array
//   üretir. Eşitlik KİMLİK karşılaştırmasıdır (ADR-023) — string'in aksine.
//
// PACKED ELEMANLAR (#206): bir array eleman tipine göre yedi ayrı buffer'dan
//   BİRİNİ kullanır (Ref/Byte/Int/LongInt/Float32/Float64/Decimal). Bu, küçük
//   sayısal dizilerde Value başına 80 bayt ödememek içindir.
//
//   Eski kod bu yedi dalı HER metodda elle tekrarlıyordu — push, pop, insert,
//   remove, slice, concat, reverse, clear: sekiz ayrı 7-dallı switch. Burada
//   dallanma üç yardımcıda toplandı (elemAt / pushElem / eraseAt); metod
//   gövdeleri eleman tipini artık hiç bilmiyor.
//
//   Yeni bir eleman tipi eklemek eskiden sekiz switch'e dokunmak demekti;
//   şimdi üç yardımcıya.
//
// ============================================================================

#include "data/array.hpp"

#include <algorithm>
#include <string>

#include "core/utf8.hpp"
#include "ffi/host_bridge.hpp"
#include "gc/gc_object.hpp"
#include "gc/gc_heap.hpp"

namespace {

// ── Receiver çözümü ──────────────────────────────────────────────────────────

ArrayObject* asArray(HostCallFrame* f, int idx, const char* method) {
    // Arite denetimi: çağıran doğru sayıda argüman geçmemişse args[idx] okumak
    // sınır dışıdır. Tip denetleyici bunu normalde engeller, ama ABI'nin
    // sözleşmesi backend'lere de açıktır — sessiz bellek hatası yerine açık hata.
    if (idx >= f->argc || !f->args) {
        f->err.set(std::string(method) + " — eksik argüman", "E_HOST");
        return nullptr;
    }
    const HostSlot& s = f->args[idx];
    if (s.kind != HostKind::Ref || !s.p ||
        static_cast<Object*>(s.p)->type != ObjectType::Array) {
        f->err.set(std::string(method) + " — expected array", "E_HOST");
        return nullptr;
    }
    return static_cast<ArrayObject*>(s.p);
}

int arr_toString(HostCallFrame* f) {
    auto* arr = asArray(f, 0, "toString");
    if (!arr) return 1;
    if (arr->elemKind != ArrayElemKind::Byte) {
        f->err.set("byte[]::toString — expected byte[]", "E_HOST");
        return 1;
    }
    hostSetRetString(*f, utf8::fromBytes(std::string_view(
        reinterpret_cast<const char*>(arr->bytes.data()), arr->bytes.size())));
    return 0;
}

// ── Packed eleman erişimi — dallanma YALNIZCA burada ─────────────────────────

}  // namespace

int dataArraySize(const ArrayObject* arr) {
    switch (arr->elemKind) {
        case ArrayElemKind::Ref:     return (int)arr->elements.size();
        case ArrayElemKind::Byte:    return (int)arr->bytes.size();
        case ArrayElemKind::Int:     return (int)arr->ints.size();
        case ArrayElemKind::LongInt: return (int)arr->longs.size();
        case ArrayElemKind::Float32: return (int)arr->f32s.size();
        case ArrayElemKind::Float64: return (int)arr->f64s.size();
        case ArrayElemKind::Decimal: return (int)arr->decimals.size();
    }
    return 0;
}

Value dataArrayElemAt(const ArrayObject* arr, int idx) {
    switch (arr->elemKind) {
        case ArrayElemKind::Ref:     return arr->elements[idx];
        case ArrayElemKind::Byte:    return Value::fromInt(arr->bytes[idx]);
        case ArrayElemKind::Int:     return Value::fromInt(arr->ints[idx]);
        case ArrayElemKind::LongInt: return Value::fromLongInt(arr->longs[idx]);
        case ArrayElemKind::Float32: return Value::fromFloat32(arr->f32s[idx]);
        case ArrayElemKind::Float64: return Value::fromFloat(arr->f64s[idx]);
        case ArrayElemKind::Decimal: return Value::fromDecimal(arr->decimals[idx]);
    }
    return Value::fromInt(0);
}

namespace {

// Sona ekler. Value, elemKind'a göre daraltılır (eski davranışla birebir).
void pushElem(ArrayObject* arr, const Value& v) {
    switch (arr->elemKind) {
        case ArrayElemKind::Ref:     arr->elements.push_back(v); break;
        case ArrayElemKind::Byte:    arr->bytes.push_back((uint8_t)v.intValue()); break;
        case ArrayElemKind::Int:     arr->ints.push_back(v.intValue()); break;
        case ArrayElemKind::LongInt: arr->longs.push_back(v.asI64()); break;
        case ArrayElemKind::Float32: arr->f32s.push_back((float)v.asDouble()); break;
        case ArrayElemKind::Float64: arr->f64s.push_back(v.asDouble()); break;
        case ArrayElemKind::Decimal: arr->decimals.push_back(v.decimalValue()); break;
    }
    // push_back reallocation yapabilir → JIT view pointer'ı eskimiş olabilir.
    arr->syncJitView();
}

void insertElem(ArrayObject* arr, int idx, const Value& v) {
    switch (arr->elemKind) {
        case ArrayElemKind::Ref:     arr->elements.insert(arr->elements.begin() + idx, v); break;
        case ArrayElemKind::Byte:    arr->bytes.insert(arr->bytes.begin() + idx, (uint8_t)v.intValue()); break;
        case ArrayElemKind::Int:     arr->ints.insert(arr->ints.begin() + idx, v.intValue()); break;
        case ArrayElemKind::LongInt: arr->longs.insert(arr->longs.begin() + idx, v.asI64()); break;
        case ArrayElemKind::Float32: arr->f32s.insert(arr->f32s.begin() + idx, (float)v.asDouble()); break;
        case ArrayElemKind::Float64: arr->f64s.insert(arr->f64s.begin() + idx, v.asDouble()); break;
        case ArrayElemKind::Decimal: arr->decimals.insert(arr->decimals.begin() + idx, v.decimalValue()); break;
    }
    arr->syncJitView();
}

void eraseAt(ArrayObject* arr, int idx) {
    switch (arr->elemKind) {
        case ArrayElemKind::Ref:     arr->elements.erase(arr->elements.begin() + idx); break;
        case ArrayElemKind::Byte:    arr->bytes.erase(arr->bytes.begin() + idx); break;
        case ArrayElemKind::Int:     arr->ints.erase(arr->ints.begin() + idx); break;
        case ArrayElemKind::LongInt: arr->longs.erase(arr->longs.begin() + idx); break;
        case ArrayElemKind::Float32: arr->f32s.erase(arr->f32s.begin() + idx); break;
        case ArrayElemKind::Float64: arr->f64s.erase(arr->f64s.begin() + idx); break;
        case ArrayElemKind::Decimal: arr->decimals.erase(arr->decimals.begin() + idx); break;
    }
    arr->syncJitView();
}

void reverseElems(ArrayObject* arr) {
    switch (arr->elemKind) {
        case ArrayElemKind::Ref:     std::reverse(arr->elements.begin(), arr->elements.end()); break;
        case ArrayElemKind::Byte:    std::reverse(arr->bytes.begin(), arr->bytes.end()); break;
        case ArrayElemKind::Int:     std::reverse(arr->ints.begin(), arr->ints.end()); break;
        case ArrayElemKind::LongInt: std::reverse(arr->longs.begin(), arr->longs.end()); break;
        case ArrayElemKind::Float32: std::reverse(arr->f32s.begin(), arr->f32s.end()); break;
        case ArrayElemKind::Float64: std::reverse(arr->f64s.begin(), arr->f64s.end()); break;
        case ArrayElemKind::Decimal: std::reverse(arr->decimals.begin(), arr->decimals.end()); break;
    }
}

void clearElems(ArrayObject* arr) {
    switch (arr->elemKind) {
        case ArrayElemKind::Ref:     arr->elements.clear(); break;
        case ArrayElemKind::Byte:    arr->bytes.clear();    break;
        case ArrayElemKind::Int:     arr->ints.clear();     break;
        case ArrayElemKind::LongInt: arr->longs.clear();    break;
        case ArrayElemKind::Float32: arr->f32s.clear();     break;
        case ArrayElemKind::Float64: arr->f64s.clear();     break;
        case ArrayElemKind::Decimal: arr->decimals.clear(); break;
    }
}

// saQut eşitliği (ADR-023): primitive değer, referans kimlik, string içerik.
bool valueEqual(const Value& a, const Value& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case ValueKind::Int:     return a.intValue()   == b.intValue();
        case ValueKind::LongInt: return a.int64Value() == b.int64Value();
        case ValueKind::Float:
        case ValueKind::Float32: return a.floatValue() == b.floatValue();
        case ValueKind::Decimal: return a.decimalValue().toString() == b.decimalValue().toString();
        case ValueKind::String:  return a.stringValue() == b.stringValue();
        case ValueKind::Ref:     return a.ref() == b.ref();   // kimlik
        case ValueKind::Null:    return true;
        case ValueKind::Date:    return a.int64Value() == b.int64Value();
    }
    return false;
}

// ── Metod gövdeleri ──────────────────────────────────────────────────────────

int arr_length(HostCallFrame* f) {
    auto* arr = asArray(f, 0, "length");
    if (!arr) return 1;
    f->ret = HostSlot::fromInt(dataArraySize(arr));
    return 0;
}

int arr_push(HostCallFrame* f) {
    auto* arr = asArray(f, 0, "push");
    if (!arr) return 1;
    pushElem(arr, fromHostSlot(f->args[1]));
    f->ret = HostSlot::fromInt(dataArraySize(arr) - 1);  // eklenen indeks
    return 0;
}

int arr_pop(HostCallFrame* f) {
    auto* arr = asArray(f, 0, "pop");
    if (!arr) return 1;
    int n = dataArraySize(arr);
    if (n == 0) {
        f->err.set("pop on empty array", "E_HOST");
        return 1;
    }
    Value v = dataArrayElemAt(arr, n - 1);
    eraseAt(arr, n - 1);
    hostSetRetValue(*f, v);
    return 0;
}

int arr_insert(HostCallFrame* f) {
    auto* arr = asArray(f, 0, "insert");
    if (!arr) return 1;
    if (f->args[1].kind != HostKind::Int) {
        f->err.set("insert — index must be int", "E_HOST");
        return 1;
    }
    int idx = (int)hostAsI64(f->args[1]);
    if (idx < 0 || idx > dataArraySize(arr)) {
        f->err.set("insert — index out of bounds", "E_HOST");
        return 1;
    }
    insertElem(arr, idx, fromHostSlot(f->args[2]));
    f->ret = HostSlot::fromInt(idx);
    return 0;
}

int arr_remove(HostCallFrame* f) {
    auto* arr = asArray(f, 0, "remove");
    if (!arr) return 1;
    if (f->args[1].kind != HostKind::Int) {
        f->err.set("remove — index must be int", "E_HOST");
        return 1;
    }
    int idx = (int)hostAsI64(f->args[1]);
    if (idx < 0 || idx >= dataArraySize(arr)) {
        f->err.set("remove — index out of bounds", "E_HOST");
        return 1;
    }
    Value v = dataArrayElemAt(arr, idx);
    eraseAt(arr, idx);
    hostSetRetValue(*f, v);
    return 0;
}

int arr_slice(HostCallFrame* f) {
    auto* arr = asArray(f, 0, "slice");
    if (!arr) return 1;
    if (!f->env || !f->env->heap) { f->err.set("slice — heap yok", "E_HOST"); return 1; }

    int n    = dataArraySize(arr);
    int from = (f->args[1].kind == HostKind::Int) ? (int)hostAsI64(f->args[1]) : 0;
    int to   = (f->args[2].kind == HostKind::Int) ? (int)hostAsI64(f->args[2]) : n;
    if (from < 0) from = 0;
    if (to > n)   to = n;

    auto* dst = f->env->heap->allocArray(to - from, arr->elemKind);
    for (int i = from; i < to; ++i) pushElem(dst, dataArrayElemAt(arr, i));
    f->ret = HostSlot::fromRef(dst);
    return 0;
}

int arr_reverse(HostCallFrame* f) {
    auto* arr = asArray(f, 0, "reverse");
    if (!arr) return 1;
    reverseElems(arr);
    f->ret = f->args[0];  // yerinde çevirir, AYNI referansı döner
    return 0;
}

int arr_concat(HostCallFrame* f) {
    auto* a = asArray(f, 0, "concat");
    if (!a) return 1;
    auto* b = asArray(f, 1, "concat");
    if (!b) return 1;
    if (b->elemKind != a->elemKind) {
        f->err.set("concat: element kind mismatch", "E_HOST");
        return 1;
    }
    if (!f->env || !f->env->heap) { f->err.set("concat — heap yok", "E_HOST"); return 1; }

    int na = dataArraySize(a), nb = dataArraySize(b);
    auto* dst = f->env->heap->allocArray(na + nb, a->elemKind);
    for (int i = 0; i < na; ++i) pushElem(dst, dataArrayElemAt(a, i));
    for (int i = 0; i < nb; ++i) pushElem(dst, dataArrayElemAt(b, i));
    f->ret = HostSlot::fromRef(dst);
    return 0;
}

int arr_contains(HostCallFrame* f) {
    auto* arr = asArray(f, 0, "contains");
    if (!arr) return 1;
    Value needle = fromHostSlot(f->args[1]);
    int n = dataArraySize(arr);
    for (int i = 0; i < n; ++i)
        if (valueEqual(dataArrayElemAt(arr, i), needle)) {
            f->ret = HostSlot::fromInt(1);
            return 0;
        }
    f->ret = HostSlot::fromInt(0);
    return 0;
}

int arr_indexOf(HostCallFrame* f) {
    auto* arr = asArray(f, 0, "indexOf");
    if (!arr) return 1;
    Value needle = fromHostSlot(f->args[1]);
    int n = dataArraySize(arr);
    for (int i = 0; i < n; ++i)
        if (valueEqual(dataArrayElemAt(arr, i), needle)) {
            f->ret = HostSlot::fromInt(i);
            return 0;
        }
    // Bulunamazsa null (dönüş tipi `int?`) — hata DEĞİL, ADR-021.
    f->ret = HostSlot::null();
    return 0;
}

int arr_clear(HostCallFrame* f) {
    auto* arr = asArray(f, 0, "clear");
    if (!arr) return 1;
    clearElems(arr);
    f->ret = HostSlot::voidVal();
    return 0;
}

}  // namespace

// ── Metod tablosu ────────────────────────────────────────────────────────────
//
// Tablo içindeki sıra runtime id'yi belirler (dataAllMethods indeksi). id'ler
// yalnız aynı süreçte üretilen IR'de yaşar; yine de ADR-044 gereği kararlı
// tutulur: yeni metot tablonun SONUNA eklenir.
const std::vector<DataMethod>& dataArrayMethods() {
    static const std::vector<DataMethod> methods = {
        {"length",   DataMethodCategory::Array, {dpElemArray()},
         drFixed(Type::Int()), HostKind::Int, HOST_PURE, arr_length},
        {"push",     DataMethodCategory::Array, {dpElemArray(), dpElem()},
         drFixed(Type::Int()), HostKind::Int, HOST_MUTATING, arr_push},
        {"pop",      DataMethodCategory::Array, {dpElemArray()},
         drElem(), HostKind::Int, HOST_MUTATING | HOST_CAN_FAIL, arr_pop},
        {"insert",   DataMethodCategory::Array, {dpElemArray(), dpFixed(Type::Int()), dpElem()},
         drFixed(Type::Int()), HostKind::Int, HOST_MUTATING | HOST_CAN_FAIL, arr_insert},
        {"remove",   DataMethodCategory::Array, {dpElemArray(), dpFixed(Type::Int())},
         drElem(), HostKind::Int, HOST_MUTATING | HOST_CAN_FAIL, arr_remove},
        // slice/concat yeni array üretir → heap
        {"slice",    DataMethodCategory::Array,
         {dpElemArray(), dpFixed(Type::Int()), dpFixed(Type::Int())},
         drElemArray(), HostKind::Ref, HOST_NEEDS_HEAP, arr_slice},
        {"reverse",  DataMethodCategory::Array, {dpElemArray()},
         drElemArray(), HostKind::Ref, HOST_MUTATING, arr_reverse},
        {"concat",   DataMethodCategory::Array, {dpElemArray(), dpElemArray()},
         drElemArray(), HostKind::Ref, HOST_NEEDS_HEAP | HOST_CAN_FAIL, arr_concat},
        {"contains", DataMethodCategory::Array, {dpElemArray(), dpElem()},
         drFixed(Type::Bool()), HostKind::Int, HOST_PURE, arr_contains},
        {"indexOf",  DataMethodCategory::Array, {dpElemArray(), dpElem()},
         drFixed(Type::Int().asNullable()), HostKind::Int, HOST_PURE, arr_indexOf},
        {"clear",    DataMethodCategory::Array, {dpElemArray()},
         drFixed(Type::Void()), HostKind::Void, HOST_MUTATING, arr_clear},
        // UTF-8 çözme: yalnız byte[] alıcıda geçerli — alıcı sabit tipte.
        {"toString", DataMethodCategory::Array, {dpFixed(Type::array(Type::Byte()))},
         drFixed(Type::String()), HostKind::Str, HOST_PURE, arr_toString},
    };
    return methods;
}
