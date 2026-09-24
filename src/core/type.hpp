// ============================================================================
// saQut Compiler — Tip Sistemi (Type System)
// ============================================================================
//
// DİZİN:   src/core/type.hpp
// KATMAN:  Katman 0 — Tüm analiz katmanları tarafından kullanılır
// BAĞIMLI: Yok (sadece <string>, <vector>, <memory>)
// KULLANAN: Sembol tablosu (Faz 2), tip denetleyici (Faz 3), optimizasyon (Faz 4)
//
// AMAÇ:
//   Kaynak koddaki her ifadenin/sembolün veri tipini temsil eder. Derleyicinin
//   "bu değer ne?" sorusuna verdiği yapısal cevaptır. Tip, makine-okur (toJson)
//   ve insan-okur (toString) olarak dışa açıktır — "veri birincil, metin bir
//   görünümdür" ilkesine uyar (bkz. readme → Tasarım felsefesi).
//
// TİP TÜRLERİ (TypeKind):
//   Primitive : int, float, double, char, string, bool, void
//   Array     : eleman tipi taşır (örn. int[])
//   Struct    : struct adı taşır (örn. struct Point)
//   Function  : dönüş tipi + parametre tipleri taşır
//   Error     : hatalı/çözümlenememiş tip — ardışık sahte hataları bastırmak için
//               (tip denetleyici, operandı Error olan ifadede yeni hata üretmez)
//
// NOT (kasıtlı sadelik): Gizli tip dönüşümü YOKTUR (ADR-010). equals() yapısal
//   ve katıdır; "int, float'a uyar mı?" gibi kurallar tip denetleyicinin işidir,
//   bu dosyanın değil. Tamsayı literalinin bağlama-göre tiplenmesi de (ADR-010)
//   Faz 3'te ele alınır.
//
// ============================================================================

#ifndef SAQUT_CORE_TYPE
#define SAQUT_CORE_TYPE

#include <string>
#include <vector>
#include <memory>
#include "vendor/nlohmann/json.hpp"

// ============================================================================
// Enum'lar
// ============================================================================

enum class PrimitiveKind { Int, LongInt, Float, Double, Decimal, Byte, Char, String, Bool, Void, Date };

// ADR-045: Pool/List elementType alanını (Array gibi) kullanır; Thread bir
// ThreadTable id'sidir (heap nesnesi değil).
enum class TypeKind { Primitive, Array, Struct, Enum, Function, Error, Pool, List, Thread };

// ============================================================================
// Type — Bir veri tipi
// ============================================================================
//
// KULLANIM:
//   Type a = Type::Int();                       // int
//   Type b = Type::array(Type::Int());          // int[]
//   Type c = Type::function(Type::Int(), {Type::Int(), Type::Int()}); // fn(int,int)->int
//   Type d = Type::structType("Point");         // struct Point
//   Type e = Type::error();                      // <error>
//
//   a.equals(Type::Int());   // true
//   a.equals(b);             // false
//   a.toString();            // "int"
//   b.toString();            // "int[]"
//   c.toJson();              // {"kind":"function",...}
//
// İç içe tipler (array elemanı, fonksiyon dönüşü) shared_ptr ile tutulur:
// Type değer-semantiğiyle kopyalanabilir kalır ama özyinelemeli olabilir.
// ============================================================================

struct Type {
    TypeKind kind = TypeKind::Error;

    PrimitiveKind         prim = PrimitiveKind::Void; // kind == Primitive
    std::shared_ptr<Type> elementType;                // kind == Array
    std::shared_ptr<Type> returnType;                 // kind == Function
    std::vector<Type>     paramTypes;                 // kind == Function
    std::string           structName;                 // kind == Struct
    std::string           enumName;                   // kind == Enum
    bool                  nullable = false;           // ADR-021: Type? sözdizimi

    // ------------------------------------------------------------------ //
    // Factory'ler
    // ------------------------------------------------------------------ //
    static Type primitive(PrimitiveKind p) {
        Type t;
        t.kind = TypeKind::Primitive;
        t.prim = p;
        return t;
    }
    static Type Int()     { return primitive(PrimitiveKind::Int); }
    static Type LongInt() { return primitive(PrimitiveKind::LongInt); }
    static Type Float()   { return primitive(PrimitiveKind::Float); }
    static Type Double()  { return primitive(PrimitiveKind::Double); }
    static Type Decimal() { return primitive(PrimitiveKind::Decimal); }
    static Type Byte()    { return primitive(PrimitiveKind::Byte); }
    static Type Date()    { return primitive(PrimitiveKind::Date); }
    static Type Char()    { return primitive(PrimitiveKind::Char); }
    static Type String() { return primitive(PrimitiveKind::String); }
    static Type Bool()   { return primitive(PrimitiveKind::Bool); }
    static Type Void()   { return primitive(PrimitiveKind::Void); }

    static Type array(Type elem) {
        Type t;
        t.kind = TypeKind::Array;
        t.elementType = std::make_shared<Type>(std::move(elem));
        return t;
    }
    static Type function(Type ret, std::vector<Type> params) {
        Type t;
        t.kind = TypeKind::Function;
        t.returnType = std::make_shared<Type>(std::move(ret));
        t.paramTypes = std::move(params);
        return t;
    }
    static Type structType(std::string name) {
        Type t;
        t.kind = TypeKind::Struct;
        t.structName = std::move(name);
        return t;
    }
    static Type enumType(std::string name) {
        Type t;
        t.kind = TypeKind::Enum;
        t.enumName = std::move(name);
        return t;
    }
    static Type error() {
        return Type{}; // varsayılan = Error
    }
    // ADR-045: Pool(T) / List(T) — eleman tipi elementType'ta. Eleman tipi
    // bilinmiyorsa (yalnız "Pool" adı) elementType boş kalır; bildirimde
    // başlatıcıdan tamamlanır.
    static Type pool(Type elem) {
        Type t;
        t.kind = TypeKind::Pool;
        t.elementType = std::make_shared<Type>(std::move(elem));
        return t;
    }
    static Type list(Type elem) {
        Type t;
        t.kind = TypeKind::List;
        t.elementType = std::make_shared<Type>(std::move(elem));
        return t;
    }
    static Type thread() {
        Type t;
        t.kind = TypeKind::Thread;
        return t;
    }

    // ------------------------------------------------------------------ //
    // Yüklemler (predicates)
    // ------------------------------------------------------------------ //
    bool isError()     const { return kind == TypeKind::Error; }
    bool isPrimitive() const { return kind == TypeKind::Primitive; }
    bool isArray()     const { return kind == TypeKind::Array; }
    bool isStruct()    const { return kind == TypeKind::Struct; }
    bool isEnum()      const { return kind == TypeKind::Enum; }
    bool isFunction()  const { return kind == TypeKind::Function; }
    bool isVoid()      const { return kind == TypeKind::Primitive && prim == PrimitiveKind::Void; }
    bool isPool()      const { return kind == TypeKind::Pool; }
    bool isList()      const { return kind == TypeKind::List; }
    bool isThread()    const { return kind == TypeKind::Thread; }

    // Aritmetik/karşılaştırma operatörlerine uygun sayısal tip mi?
    // byte de sayısaldır (#86) ama aritmetikte int'e terfi eder (C modeli) —
    // sonuç asla byte olmaz; bu ayrım TypeChecker'da yapılır (numericRank byte
    // içermez, promotion aritmetik dalında elle yapılır).
    bool isNumeric() const {
        return kind == TypeKind::Primitive &&
               (prim == PrimitiveKind::Int     ||
                prim == PrimitiveKind::LongInt ||
                prim == PrimitiveKind::Float   ||
                prim == PrimitiveKind::Double  ||
                prim == PrimitiveKind::Decimal ||
                prim == PrimitiveKind::Byte);
    }

    bool isDecimal() const {
        return kind == TypeKind::Primitive && prim == PrimitiveKind::Decimal;
    }

    bool isByte() const {
        return kind == TypeKind::Primitive && prim == PrimitiveKind::Byte;
    }

    // ADR-040: 64-bit genel amaçlı tamsayı; rank kulesine katılmaz (byte gibi
    // izole) — int→longint serbest, longint↔float/double/decimal yalnızca `as`.
    bool isLongInt() const {
        return kind == TypeKind::Primitive && prim == PrimitiveKind::LongInt;
    }

    bool isInt() const {
        return kind == TypeKind::Primitive && prim == PrimitiveKind::Int;
    }

    // int veya longint (her ikisi de tamsayı; aritmetik/cast dallarında ortak)
    bool isIntegral() const {
        return kind == TypeKind::Primitive &&
               (prim == PrimitiveKind::Int || prim == PrimitiveKind::LongInt);
    }

    // #88 (ADR-036): date yalnızca karşılaştırılabilir — aritmetik YOK
    // (birim belirsizliği önlenir; yalnızca açık addX fonksiyonları).
    bool isDate() const {
        return kind == TypeKind::Primitive && prim == PrimitiveKind::Date;
    }

    bool isString() const {
        return kind == TypeKind::Primitive && prim == PrimitiveKind::String;
    }

    // ADR-021: "null" literal tipi — yalnızca nullable değişkene atanabilir
    bool isNullLiteral() const {
        return kind == TypeKind::Primitive && prim == PrimitiveKind::Void && nullable;
    }

    // Nullable kopyası döndür
    Type asNullable() const { Type t = *this; t.nullable = true; return t; }
    Type asNonNull()  const { Type t = *this; t.nullable = false; return t; }

    // ------------------------------------------------------------------ //
    // equals — Yapısal eşitlik (katı; gizli dönüşüm yok, ADR-010)
    // ------------------------------------------------------------------ //
    // Yapısal eşitlik — nullable dahil (ADR-021: int ≠ int?)
    bool equals(const Type& o) const {
        if (kind != o.kind) return false;
        if (nullable != o.nullable) return false;
        switch (kind) {
            case TypeKind::Primitive:
                return prim == o.prim;
            case TypeKind::Array:
            case TypeKind::Pool:
            case TypeKind::List:
                return elementType && o.elementType &&
                       elementType->equals(*o.elementType);
            case TypeKind::Thread:
                return true;
            case TypeKind::Struct:
                return structName == o.structName;
            case TypeKind::Enum:
                return enumName == o.enumName;
            case TypeKind::Function: {
                if (!returnType || !o.returnType) return false;
                if (!returnType->equals(*o.returnType)) return false;
                if (paramTypes.size() != o.paramTypes.size()) return false;
                for (size_t i = 0; i < paramTypes.size(); ++i)
                    if (!paramTypes[i].equals(o.paramTypes[i])) return false;
                return true;
            }
            case TypeKind::Error:
                return true;
        }
        return false;
    }

    // Temel yapısal eşitlik — nullable farkını yok say (T == T? üstün çakışma için)
    bool equalsBase(const Type& o) const { return asNonNull().equals(o.asNonNull()); }

    // ------------------------------------------------------------------ //
    // İsim yardımcıları
    // ------------------------------------------------------------------ //
    static const char* primName(PrimitiveKind p) {
        switch (p) {
            case PrimitiveKind::Int:     return "int";
            case PrimitiveKind::LongInt: return "longint";
            case PrimitiveKind::Float:   return "float";
            case PrimitiveKind::Double:  return "double";
            case PrimitiveKind::Decimal: return "decimal";
            case PrimitiveKind::Byte:    return "byte";
            case PrimitiveKind::Date:    return "date";
            case PrimitiveKind::Char:    return "char";
            case PrimitiveKind::String:  return "string";
            case PrimitiveKind::Bool:    return "bool";
            case PrimitiveKind::Void:    return "void";
        }
        return "?";
    }

    // Bir tip adından (parser tipleri string olarak tutar) Type üretir.
    // "int?" → nullable int; "int[]" → int array; bilinen değilse Error.
    static Type fromName(const std::string& n) {
        // Nullable soneki: "int?", "string?" vb. (ADR-021)
        if (!n.empty() && n.back() == '?') {
            Type base = fromName(n.substr(0, n.size() - 1));
            if (!base.isError()) return base.asNullable();
            return error();
        }
        if (n == "int")     return Int();
        if (n == "longint") return LongInt();
        if (n == "float")   return Float();
        if (n == "double")  return Double();
        if (n == "decimal") return Decimal();
        if (n == "byte")    return Byte();
        if (n == "date")    return Date();
        if (n == "char")    return Char();
        if (n == "string") return String();
        if (n == "bool")   return Bool();
        if (n == "void")   return Void();
        // ADR-045: "Thread" tam tiptir; "Pool"/"List" eleman tipsiz yer tutucudur
        // (bildirimde Pool(T)/List(T) başlatıcısından tamamlanır).
        if (n == "Thread") return thread();
        if (n == "Pool")   { Type t; t.kind = TypeKind::Pool; return t; }
        if (n == "List")   { Type t; t.kind = TypeKind::List; return t; }
        // "int[]", "float[]" vb. — suffix [] ile dizi tipi
        if (n.size() > 2 && n.substr(n.size() - 2) == "[]") {
            Type elem = fromName(n.substr(0, n.size() - 2));
            if (!elem.isError()) return array(elem);
        }
        return error();
    }

    // ------------------------------------------------------------------ //
    // toString — İnsan-okur ("int", "int[]", "fn(int,int)->int")
    // ------------------------------------------------------------------ //
    std::string toString() const {
        std::string base;
        switch (kind) {
            case TypeKind::Primitive:
                base = primName(prim); break;
            case TypeKind::Array:
                base = (elementType ? elementType->toString() : "<?>") + "[]"; break;
            case TypeKind::Pool:
                base = "Pool(" + (elementType ? elementType->toString() : std::string("?")) + ")"; break;
            case TypeKind::List:
                base = "List(" + (elementType ? elementType->toString() : std::string("?")) + ")"; break;
            case TypeKind::Thread:
                base = "Thread"; break;
            case TypeKind::Struct:
                base = "struct " + structName; break;
            case TypeKind::Enum:
                base = enumName; break;
            case TypeKind::Function: {
                base = "fn(";
                for (size_t i = 0; i < paramTypes.size(); ++i) {
                    if (i) base += ",";
                    base += paramTypes[i].toString();
                }
                base += ")->";
                base += returnType ? returnType->toString() : "<?>";
                break;
            }
            case TypeKind::Error:
                return "<error>";
        }
        return nullable ? base + "?" : base;
    }

    // ------------------------------------------------------------------ //
    // toJson — Makine-okur (cam ilkesi: her tip dışarıdan sorgulanabilir)
    // ------------------------------------------------------------------ //
    nlohmann::json toJsonObj() const {
        nlohmann::json j;
        switch (kind) {
            case TypeKind::Primitive:
                j["kind"] = "primitive";
                j["name"] = primName(prim);
                break;
            case TypeKind::Array:
                j["kind"]    = "array";
                j["element"] = elementType ? elementType->toJsonObj() : nullptr;
                break;
            case TypeKind::Struct:
                j["kind"] = "struct";
                j["name"] = structName;
                break;
            case TypeKind::Enum:
                j["kind"] = "enum";
                j["name"] = enumName;
                break;
            case TypeKind::Function: {
                j["kind"]    = "function";
                j["returns"] = returnType ? returnType->toJsonObj() : nullptr;
                nlohmann::json params = nlohmann::json::array();
                for (const auto& p : paramTypes) params.push_back(p.toJsonObj());
                j["params"] = params;
                break;
            }
            case TypeKind::Error:
                j["kind"] = "error";
                break;
            case TypeKind::Pool:
            case TypeKind::List:
                j["kind"]    = kind == TypeKind::Pool ? "pool" : "list";
                j["element"] = elementType ? elementType->toJsonObj() : nullptr;
                break;
            case TypeKind::Thread:
                j["kind"] = "thread";
                break;
        }
        return j;
    }

    std::string toJson() const { return toJsonObj().dump(); }
};

#endif // SAQUT_CORE_TYPE
