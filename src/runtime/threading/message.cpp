// ============================================================================
// saQut — Mesaj serileştirme gerçeklemesi (ADR-045 Faz 2-c). Sözleşme:
// message.hpp.
// ============================================================================

#include "runtime/threading/message.hpp"

#include <cstring>
#include <stdexcept>
#include <unordered_map>

#include "gc/gc_heap.hpp"
#include "gc/gc_object.hpp"

namespace saqut::threading {

namespace {

constexpr uint8_t  kObjBackref = 0;
constexpr uint8_t  kObjNew     = 1;
constexpr uint32_t kNoMeta     = 0xFFFFFFFFu;

// ── Yazıcı ──────────────────────────────────────────────────────────────────
struct Writer {
    MessageBuffer&                                buf;
    std::unordered_map<const Object*, uint32_t>   seen;
    std::unordered_map<const void*, uint32_t>     metaIndex;
    uint32_t                                      nextIndex = 0;

    template <typename T>
    void put(const T& x) {
        const size_t at = buf.bytes.size();
        buf.bytes.resize(at + sizeof(T));
        std::memcpy(buf.bytes.data() + at, &x, sizeof(T));
    }
    void putBytes(const void* p, size_t n) {
        const size_t at = buf.bytes.size();
        buf.bytes.resize(at + n);
        if (n) std::memcpy(buf.bytes.data() + at, p, n);
    }

    void value(const Value& v) {
        put<uint8_t>(static_cast<uint8_t>(v.kind));
        switch (v.kind) {
            case ValueKind::Int:
            case ValueKind::LongInt:
            case ValueKind::Date:
                put<int64_t>(v.asI64());
                break;
            case ValueKind::Float:
            case ValueKind::Float32:
                put<double>(v.floatValue());
                break;
            case ValueKind::Decimal:
                put<int64_t>(v.decimalValue().coeff);
                put<int32_t>(v.decimalValue().exp);
                break;
            case ValueKind::Null:
                break;
            case ValueKind::String:
            case ValueKind::Ref:
                object(v.ref());
                break;
        }
    }

    void object(const Object* o) {
        if (!o) throw std::runtime_error("message: null object reference");
        if (auto it = seen.find(o); it != seen.end()) {
            put<uint8_t>(kObjBackref);
            put<uint32_t>(it->second);
            return;
        }
        seen.emplace(o, nextIndex++);
        put<uint8_t>(kObjNew);
        put<uint8_t>(static_cast<uint8_t>(o->type));
        switch (o->type) {
            case ObjectType::String: {
                const auto* s = static_cast<const StringObject*>(o);
                put<uint64_t>(s->data.size());
                putBytes(s->data.data(), s->data.size());
                break;
            }
            case ObjectType::Decimal: {
                const auto* d = static_cast<const DecimalObject*>(o);
                put<int64_t>(d->val.coeff);
                put<int32_t>(d->val.exp);
                break;
            }
            case ObjectType::Array: {
                const auto* a = static_cast<const ArrayObject*>(o);
                put<uint8_t>(static_cast<uint8_t>(a->elemKind));
                switch (a->elemKind) {
                    case ArrayElemKind::Ref:
                        put<uint64_t>(a->elements.size());
                        for (const Value& e : a->elements) value(e);
                        break;
                    case ArrayElemKind::Byte:
                        put<uint64_t>(a->bytes.size());
                        putBytes(a->bytes.data(), a->bytes.size());
                        break;
                    case ArrayElemKind::Int:
                        put<uint64_t>(a->ints.size());
                        putBytes(a->ints.data(), a->ints.size() * sizeof(int32_t));
                        break;
                    case ArrayElemKind::LongInt:
                        put<uint64_t>(a->longs.size());
                        putBytes(a->longs.data(), a->longs.size() * sizeof(int64_t));
                        break;
                    case ArrayElemKind::Float32:
                        put<uint64_t>(a->f32s.size());
                        putBytes(a->f32s.data(), a->f32s.size() * sizeof(float));
                        break;
                    case ArrayElemKind::Float64:
                        put<uint64_t>(a->f64s.size());
                        putBytes(a->f64s.data(), a->f64s.size() * sizeof(double));
                        break;
                    case ArrayElemKind::Decimal:
                        put<uint64_t>(a->decimals.size());
                        for (const DecimalValue& d : a->decimals) {
                            put<int64_t>(d.coeff);
                            put<int32_t>(d.exp);
                        }
                        break;
                }
                break;
            }
            case ObjectType::Struct: {
                const auto* s = static_cast<const StructObject*>(o);
                put<uint32_t>(static_cast<uint32_t>(s->fields.size()));
                uint32_t meta = kNoMeta;
                if (s->fieldNames) {
                    auto [it, inserted] = metaIndex.emplace(s->fieldNames.get(),
                                                            (uint32_t)buf.metas.size());
                    if (inserted) buf.metas.push_back(s->fieldNames);
                    meta = it->second;
                }
                put<uint32_t>(meta);
                for (const Value& f : s->fields) value(f);
                break;
            }
        }
    }
};

// ── Okuyucu ─────────────────────────────────────────────────────────────────
struct Reader {
    MessageBuffer&        buf;
    Heap&                 heap;
    std::vector<Object*>  byIndex;

    template <typename T>
    T get() {
        if (buf.readPos + sizeof(T) > buf.bytes.size())
            throw std::runtime_error("message: truncated buffer");
        T x;
        std::memcpy(&x, buf.bytes.data() + buf.readPos, sizeof(T));
        buf.readPos += sizeof(T);
        return x;
    }
    void getBytes(void* p, size_t n) {
        if (buf.readPos + n > buf.bytes.size())
            throw std::runtime_error("message: truncated buffer");
        if (n) std::memcpy(p, buf.bytes.data() + buf.readPos, n);
        buf.readPos += n;
    }
    DecimalValue decimal() {
        DecimalValue d;
        d.coeff = get<int64_t>();
        d.exp   = get<int32_t>();
        return d;
    }

    Value value() {
        const auto kind = static_cast<ValueKind>(get<uint8_t>());
        switch (kind) {
            case ValueKind::Int:     return Value::fromInt((int)get<int64_t>());
            case ValueKind::LongInt: return Value::fromLongInt(get<int64_t>());
            case ValueKind::Date:    return Value::fromDate(get<int64_t>());
            case ValueKind::Float:   return Value::fromFloat(get<double>());
            case ValueKind::Float32: return Value::fromFloat32(get<double>());
            case ValueKind::Decimal: return Value::fromDecimal(decimal());
            case ValueKind::Null:    return Value::null();
            case ValueKind::String:  return Value::fromStringObject(object());
            case ValueKind::Ref:     return Value::fromRef(object());
        }
        throw std::runtime_error("message: bad value kind");
    }

    Object* object() {
        const uint8_t tag = get<uint8_t>();
        if (tag == kObjBackref) {
            const uint32_t idx = get<uint32_t>();
            if (idx >= byIndex.size()) throw std::runtime_error("message: bad backref");
            return byIndex[idx];
        }
        const auto type = static_cast<ObjectType>(get<uint8_t>());
        switch (type) {
            case ObjectType::String: {
                const auto n = get<uint64_t>();
                std::string s(n, '\0');
                getBytes(s.data(), n);
                Object* o = heap.allocString(std::move(s));
                byIndex.push_back(o);
                return o;
            }
            case ObjectType::Decimal: {
                Object* o = heap.allocDecimal(decimal());
                byIndex.push_back(o);
                return o;
            }
            case ObjectType::Array: {
                const auto k = static_cast<ArrayElemKind>(get<uint8_t>());
                const auto n = get<uint64_t>();
                ArrayObject* a = heap.allocArray((int)n, k);
                // Döngü desteği: nesne, elemanları okunmadan ÖNCE indekslenir.
                byIndex.push_back(a);
                switch (k) {
                    case ArrayElemKind::Ref:
                        for (uint64_t i = 0; i < n; ++i) a->elements.push_back(value());
                        break;
                    case ArrayElemKind::Byte:
                        a->bytes.resize(n);   getBytes(a->bytes.data(), n);
                        break;
                    case ArrayElemKind::Int:
                        a->ints.resize(n);    getBytes(a->ints.data(), n * sizeof(int32_t));
                        break;
                    case ArrayElemKind::LongInt:
                        a->longs.resize(n);   getBytes(a->longs.data(), n * sizeof(int64_t));
                        break;
                    case ArrayElemKind::Float32:
                        a->f32s.resize(n);    getBytes(a->f32s.data(), n * sizeof(float));
                        break;
                    case ArrayElemKind::Float64:
                        a->f64s.resize(n);    getBytes(a->f64s.data(), n * sizeof(double));
                        break;
                    case ArrayElemKind::Decimal:
                        for (uint64_t i = 0; i < n; ++i) a->decimals.push_back(decimal());
                        break;
                }
                a->syncJitView();
                return a;
            }
            case ObjectType::Struct: {
                const auto n    = get<uint32_t>();
                const auto meta = get<uint32_t>();
                StructObject* s = heap.allocStruct((int)n);
                byIndex.push_back(s);
                if (meta != kNoMeta) {
                    if (meta >= buf.metas.size()) throw std::runtime_error("message: bad meta");
                    s->fieldNames = buf.metas[meta];
                }
                for (uint32_t i = 0; i < n; ++i) s->fields[i] = value();
                return s;
            }
        }
        throw std::runtime_error("message: bad object type");
    }
};

}  // namespace

void serialize(const Value& v, MessageBuffer& buf) {
    Writer w{buf, {}, {}, 0};
    w.value(v);
}

Value deserialize(MessageBuffer& buf, Heap& heap) {
    Reader r{buf, heap, {}};
    return r.value();
}

}  // namespace saqut::threading
