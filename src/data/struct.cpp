// ============================================================================
// saQut — struct veri tipi
// ============================================================================
//
// DİZİN:   src/data/struct.cpp
// KATMAN:  data — struct'ın çalışma zamanındaki TEK sahibi
//
// SEMANTİK (ADR-020): struct REFERANS tiplidir; eşitlik KİMLİK karşılaştırması
//   (ADR-023). Alan adları StructObject::fieldNames'te paylaşılan bir tabloya
//   işaret eder (her örnekte kopyalanmaz).
//
// KAPSAM: burada yalnızca struct'ın METODLARI (toJson, dump) ve serileştirme
//   mantığı vardır. GC işaretleme (markChildren), alan okuma/yazma opcode'ları
//   (FIELD_GET/FIELD_SET) ve zero-init maskesi bu modülün DIŞINDADIR — onlar
//   tipe değil derleyici/VM aşamalarına aittir.
//
// ============================================================================

#include "data/struct.hpp"

#include <sstream>
#include <string>
#include <vector>

#include "data/array.hpp"
#include "ffi/host_bridge.hpp"
#include "gc/gc_object.hpp"
#include "gc/gc_heap.hpp"

namespace {

// Paylaşılan boş tablo. Eski kod burada
//   `obj->fieldNames ? *obj->fieldNames : std::vector<std::string>()`
// yazıyordu — fieldNames null iken GEÇİCİ bir vector'e referans bağlayan
// sarkan referanstı (sys_args'takiyle aynı hata sınıfı).
const std::vector<std::string>& fieldNamesOf(const StructObject* obj) {
    static const std::vector<std::string> kNone;
    return obj->fieldNames ? *obj->fieldNames : kNone;
}

std::string fieldKey(const std::vector<std::string>& names, size_t i) {
    return i < names.size() ? names[i] : ("field" + std::to_string(i));
}

// ── JSON serileştirme ────────────────────────────────────────────────────────

std::string valueToJson(const Value& v);

std::string structToJson(const StructObject* obj) {
    const auto& names = fieldNamesOf(obj);
    std::string s = "{";
    for (size_t i = 0; i < obj->fields.size(); ++i) {
        if (i) s += ",";
        s += "\"" + fieldKey(names, i) + "\":" + valueToJson(obj->fields[i]);
    }
    return s + "}";
}

std::string jsonEscape(const std::string& in) {
    std::string r = "\"";
    for (char c : in) {
        if      (c == '"')  r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\t') r += "\\t";
        else r += c;
    }
    return r + "\"";
}

std::string valueToJson(const Value& v) {
    switch (v.kind) {
        case ValueKind::Int:     return std::to_string(v.intValue());
        case ValueKind::LongInt: return std::to_string(v.int64Value());
        case ValueKind::Float:
        case ValueKind::Float32: {
            std::ostringstream os; os << v.floatValue(); return os.str();
        }
        case ValueKind::Decimal: return v.decimalValue().toString();
        case ValueKind::String:  return jsonEscape(v.stringValue());
        case ValueKind::Null:    return "null";
        case ValueKind::Date:    return std::to_string(v.int64Value());
        case ValueKind::Ref: {
            if (!v.ref()) return "null";
            if (v.ref()->type == ObjectType::Struct)
                return structToJson(static_cast<StructObject*>(v.ref()));
            // Array: packed eleman erişimi array modülünden gelir — bu dosya
            // eleman tiplerini bilmez (eski kod burada 7-dallı switch'i iki kez
            // daha tekrarlıyordu).
            const auto* arr = static_cast<const ArrayObject*>(v.ref());
            int n = dataArraySize(arr);
            std::string s = "[";
            for (int i = 0; i < n; ++i) {
                if (i) s += ",";
                s += valueToJson(dataArrayElemAt(arr, i));
            }
            return s + "]";
        }
    }
    return "null";
}

// ── Receiver çözümü ──────────────────────────────────────────────────────────
//
// struct olmayan receiver'da eski davranış hata DEĞİL, "null" string'i
// döndürmekti — birebir korunur.
const StructObject* asStruct(const HostSlot& s) {
    if (s.kind != HostKind::Ref || !s.p) return nullptr;
    const auto* o = static_cast<const Object*>(s.p);
    return o->type == ObjectType::Struct ? static_cast<const StructObject*>(o) : nullptr;
}

// ── Metod gövdeleri ──────────────────────────────────────────────────────────

int struct_toJson(HostCallFrame* f) {
    if (f->argc < 1 || !f->args) { hostSetRetString(*f, "null"); return 0; }
    const StructObject* obj = asStruct(f->args[0]);
    hostSetRetString(*f, obj ? structToJson(obj) : "null");
    return 0;
}

int struct_dump(HostCallFrame* f) {
    if (f->argc < 1 || !f->args) { hostSetRetString(*f, "null"); return 0; }
    const StructObject* obj = asStruct(f->args[0]);
    if (!obj) { hostSetRetString(*f, "null"); return 0; }

    const auto& names = fieldNamesOf(obj);
    std::string s = "struct{";
    for (size_t i = 0; i < obj->fields.size(); ++i) {
        if (i) s += ", ";
        s += fieldKey(names, i) + "=" + obj->fields[i].toString();
    }
    hostSetRetString(*f, s + "}");
    return 0;
}

}  // namespace

// ── Metod tablosu ────────────────────────────────────────────────────────────

const std::vector<DataMethod>& dataStructMethods() {
    static const std::vector<DataMethod> methods = {
        {"toJson", DataMethodCategory::StructVal, {dpElem()},
         drFixed(Type::String()), HostKind::Str, HOST_PURE, struct_toJson},
        {"dump",   DataMethodCategory::StructVal, {dpElem()},
         drFixed(Type::String()), HostKind::Str, HOST_PURE, struct_dump},
    };
    return methods;
}
