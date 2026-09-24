// ============================================================================
// saQut IR — Ortak Talimat Operand Renderer'ı (#218)
//
// Flat IR dump (IRFunction::dump) ve CFG dump (BasicBlock::dump) AYNI
// operand görünümünü kullanır — tek kaynak, iki görünüm. Literal değerler
// (int/long/float/string/decimal), slot'lar, callee adları ve nullable
// [null]/[throw] etiketleri burada üretilir.
//
// RENK PALETİ: iki dump farklı şema kullanır (kFlatPalette = mevcut flat
// renkleri, birebir korunur; kCfgPalette = CFG görüntüleyici şeması).
// Renkler TTY-aware'dir (IrColor) — redirect/pipe'ta sıfır ANSI.
//
// DİKKAT (spec): longint/float32 opcode'ları (LOAD_LONG, LADD, F32ADD,
// CAST_*_LONG vb.) flat dump'da BİLİNÇLİ boş render edilir — golden spec
// (tests/golden/ir/*.ir_plain.expected) bunu sabitler. Bu switch o davranışı
// birebir korur; eksik render ayrı bir iş olarak kayıtlıdır.
//
// -Wswitch koruması: switch default içermez — OPCODE_LIST'e yeni opcode
// eklenince ir_function.cpp TU'sundaki -Werror=switch bu dosyayı kırar,
// renderer'ın güncellenmesi zorunlu olur.
// ============================================================================

#ifndef SAQUT_IR_DUMP
#define SAQUT_IR_DUMP

#include <sstream>
#include <string>
#include "ir/instruction.hpp"
#include "ir/ir_color.hpp"
#include "data/data_registry.hpp"
#include "ffi/host_registry.hpp"

namespace IrDump {

// Renk paleti — rol bazlı; her rol TTY-aware renk fonksiyonu döndürür.
struct Palette {
    const char* (*slot)()  = IrColor::SoftTurkuaz;  // slot adları (s1, s2)
    const char* (*value)() = IrColor::SoftTuruncu;  // sayısal sabitler
    const char* (*op)()    = IrColor::SoftMor;      // işlem sembolleri (+, ~, -)
    const char* (*str)()   = IrColor::SoftPembe;    // string sabitleri
    const char* (*fn)()    = IrColor::SoftYesil;    // callee/builtin adları
    const char* (*label)() = IrColor::SoftGri;      // yapısal etiketler (=, (, [, →, return)
    const char* (*tag)()   = IrColor::SoftTurkuaz;  // null / [null] / [throw] etiketleri
    const char* (*reset)() = IrColor::Reset;
};

// Flat IR dump paleti — mevcut IRFunction::dump renkleri (birebir korunur).
inline const Palette kFlatPalette;

// CFG dump paleti — kullanıcı şeması: slot açık mavi, etiket koyu sarı.
inline const Palette kCfgPalette{
    IrColor::SoftMavi,      // slot
    IrColor::SoftTuruncu,   // value
    IrColor::SoftTuruncu,   // op
    IrColor::SoftPembe,     // str
    IrColor::SoftYesil,     // fn
    IrColor::KoyuSari,      // label
    IrColor::SoftTurkuaz,   // tag
    IrColor::Reset,
};

// Slot adı: s0, s1, ... (-1 → "?")
inline std::string renderSlot(int s, const Palette& p) {
    if (s == -1) return "?";
    return std::string(p.slot()) + "s" + std::to_string(s) + p.reset();
}

// Tam sayı değeri
inline std::string renderInt(int v, const Palette& p) {
    return std::string(p.value()) + std::to_string(v) + p.reset();
}

// Etiketli metin
inline std::string renderLabel(const char* t, const Palette& p) {
    return std::string(p.label()) + t + p.reset();
}

// İkili op sembolü: ADD → "+"
inline const char* opSymbol(Opcode op) {
    switch (op) {
        case Opcode::ADD:           return "+";
        case Opcode::SUB:           return "-";
        case Opcode::MUL:           return "*";
        case Opcode::DIV:           return "/";
        case Opcode::FADD:          return "+.";
        case Opcode::FSUB:          return "-.";
        case Opcode::FMUL:          return "*.";
        case Opcode::FDIV:          return "/.";
        case Opcode::MOD:           return "%";
        case Opcode::POW:           return "**";
        case Opcode::LPOW:          return "**L";
        case Opcode::FPOW:          return "**.";
        case Opcode::F32POW:        return "**f";
        case Opcode::FMOD:          return "%.";
        // #266: longint (L) ve float32 (f) aritmetiği — eskiden render edilmiyordu.
        case Opcode::LADD:          return "+L";
        case Opcode::LSUB:          return "-L";
        case Opcode::LMUL:          return "*L";
        case Opcode::LDIV:          return "/L";
        case Opcode::LMOD:          return "%L";
        case Opcode::LBAND:         return "&L";
        case Opcode::LBOR:          return "|L";
        case Opcode::LBXOR:         return "^L";
        case Opcode::LSHL:          return "<<L";
        case Opcode::LSHR:          return ">>L";
        case Opcode::F32ADD:        return "+f";
        case Opcode::F32SUB:        return "-f";
        case Opcode::F32MUL:        return "*f";
        case Opcode::F32DIV:        return "/f";
        case Opcode::F32MOD:        return "%f";
        case Opcode::BAND:          return "&";
        case Opcode::BOR:           return "|";
        case Opcode::BXOR:          return "^";
        case Opcode::SHL:           return "<<";
        case Opcode::SHR:           return ">>";
        case Opcode::LESS:          return "<";
        case Opcode::LESS_EQUAL:    return "<=";
        case Opcode::GREATER:       return ">";
        case Opcode::GREATER_EQUAL: return ">=";
        case Opcode::EQUAL_EQUAL:   return "==";
        case Opcode::NOT_EQUAL:     return "!=";
        case Opcode::STRING_CONCAT: return "++";
        case Opcode::DADD:          return "+d";
        case Opcode::DSUB:          return "-d";
        case Opcode::DMUL:          return "*d";
        case Opcode::DDIV:          return "/d";
        case Opcode::DMOD:          return "%d";
        default:                    return "?";
    }
}

// Flat dump'daki isBinaryOp kümesi — BİREBİR korunur (golden spec).
// #266: longint/float32 aritmetiği de ikili op olarak render edilir
// (eskiden bilinçli dışarıdaydı ve `saqut ir` operandsız satır basıyordu).
static bool isBinaryOp(Opcode op) {
    switch (op) {
        case Opcode::LADD: case Opcode::LSUB: case Opcode::LMUL:
        case Opcode::LDIV: case Opcode::LMOD:
        case Opcode::LBAND: case Opcode::LBOR: case Opcode::LBXOR:
        case Opcode::LSHL: case Opcode::LSHR:
        case Opcode::F32ADD: case Opcode::F32SUB: case Opcode::F32MUL: case Opcode::F32DIV:
        case Opcode::ADD: case Opcode::SUB: case Opcode::MUL:
        case Opcode::DIV: case Opcode::MOD:
        case Opcode::POW: case Opcode::LPOW: case Opcode::FPOW: case Opcode::F32POW:
        case Opcode::FMOD: case Opcode::F32MOD:
        case Opcode::FADD: case Opcode::FSUB: case Opcode::FMUL: case Opcode::FDIV:
        case Opcode::BAND: case Opcode::BOR: case Opcode::BXOR:
        case Opcode::SHL: case Opcode::SHR:
        case Opcode::LESS: case Opcode::LESS_EQUAL:
        case Opcode::GREATER: case Opcode::GREATER_EQUAL:
        case Opcode::EQUAL_EQUAL: case Opcode::NOT_EQUAL:
        case Opcode::STRING_CONCAT:
        case Opcode::DADD: case Opcode::DSUB: case Opcode::DMUL:
        case Opcode::DDIV: case Opcode::DMOD:
            return true;
        default: return false;
    }
}

// Nullable hedef etiketi: [null] / [throw]
inline std::string nullableTag(const Instruction& ins, const Palette& p) {
    return std::string(" ") + p.tag()
         + (ins.left ? "[null]" : "[throw]") + p.reset();
}

// String sabitini dump satırında TEK satırda tutar: satır sonu/sekme gibi
// denetim karakterleri kaçış dizisine çevrilir. IR'yi satır-satır işleyen
// araçlar (grep, fixture karşılaştırma) çok satırlı literal yüzünden
// şaşırtılmasın.
inline std::string escapeForDump(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            default:   out += c; break;
        }
    }
    return out;
}

// Bir talimatın opcode'undan SONRAKI operand kısmını üretir (renkli).
// switch default İÇERMEZ — yeni opcode eklenirse -Wswitch/-Werror bunu kırar.
inline std::string operands(const Instruction& ins, const Palette& p = kFlatPalette) {
    std::ostringstream os;
    auto s = [&](int n) { return renderSlot(n, p); };
    auto v = [&](int n) { return renderInt(n, p); };
    auto L = [&](const char* t) { return renderLabel(t, p); };
    const std::string reset = p.reset();

    if (isBinaryOp(ins.opcode)) {
        os << s(ins.dest) << " " << L("=") << " "
           << s(ins.left) << " " << p.op() << opSymbol(ins.opcode) << reset
           << " " << s(ins.right);
        return os.str();
    }

    switch (ins.opcode) {
        case Opcode::LOAD_CONST:
            os << s(ins.dest) << " " << L("=") << " " << v(ins.intValue);
            break;
        case Opcode::LOAD_STRING:
            os << s(ins.dest) << " " << L("=") << " \""
               << p.str() << escapeForDump(ins.stringValue) << reset << "\"";
            break;
        case Opcode::LOAD_NULL:
            os << s(ins.dest) << " " << L("=") << " " << p.tag() << "null" << reset;
            break;
        case Opcode::LOAD_SLOT:
            os << s(ins.dest) << " " << L("=") << " " << s(ins.src);
            break;

        case Opcode::JMP:
            os << L("→ ") << v(ins.jumpTarget);
            break;
        case Opcode::JIF_FALSE:
            os << L("!") << s(ins.cond) << " " << L("→") << " " << v(ins.jumpTarget);
            break;
        case Opcode::JIF_TRUE:
            os << s(ins.cond) << " " << L("→") << " " << v(ins.jumpTarget);
            break;

        case Opcode::CALL:
            os << s(ins.dest) << " " << L("=") << " "
               << p.fn() << ins.functionName << reset << L("(");
            for (size_t j = 0; j < ins.argSlots.size(); ++j) {
                if (j) os << L(", ");
                os << s(ins.argSlots[j]);
            }
            os << L(")");
            break;
        case Opcode::CALLHOST:
            if (ins.functionName == "__builtin_method__") {
                const auto* bm = dataMethodAt(ins.intValue - kBuiltinBase);  // #227: birleşik indeks
                std::string methodLabel = bm ? std::string(bm->name)
                                             : ("id" + std::to_string(ins.intValue));
                if (ins.dest >= 0)
                    os << s(ins.dest) << " " << L("=") << " ";
                os << L("builtin::") << p.fn() << methodLabel << reset << L("(");
            } else if (ins.functionName == "__ffi__") {
                // FFI çağrısının gerçeği intValue'daki birleşik registry
                // indeksidir (#227/#229); functionName yalnızca yer tutucu.
                // Dispatch indeksle yaptığı için dump da ismi indeksten
                // çözer — tablo dışı indeks hâlâ görünebilir kalır.
                const HostEntry* he = hostEntryAt(ins.intValue);
                if (ins.dest >= 0)
                    os << s(ins.dest) << " " << L("=") << " ";
                os << L("ffi::") << p.fn()
                   << (he ? he->symbolicId : std::string("id") + std::to_string(ins.intValue))
                   << reset << L("(");
            } else {
                os << p.fn() << ins.functionName << reset << L("(");
            }
            for (size_t j = 0; j < ins.argSlots.size(); ++j) {
                if (j) os << L(", ");
                os << s(ins.argSlots[j]);
            }
            os << L(")");
            break;
        case Opcode::RETURN:
            os << L("return") << " " << s(ins.src);
            break;
        case Opcode::THROW:
            os << s(ins.src);
            break;

        // ── ADR-045: izole thread modeli ─────────────────────────────────
        case Opcode::SHARED_LOAD:
        case Opcode::POOL_POP:
        case Opcode::POOL_LEN:
        case Opcode::LIST_LEN:
            os << s(ins.dest) << " " << L("=") << " " << L("shared#") << v(ins.intValue);
            break;
        case Opcode::SHARED_STORE:
        case Opcode::POOL_PUSH:
        case Opcode::POOL_SETMAX:
        case Opcode::LIST_APPEND:
            os << L("shared#") << v(ins.intValue) << " " << L("←") << " " << s(ins.src);
            break;
        case Opcode::SHARED_RMW:
            os << s(ins.dest) << " " << L("=") << " " << L("shared#") << v(ins.intValue) << " "
               << p.op() << (ins.int64Value ? "-=" : "+=") << reset << " " << s(ins.src);
            break;
        case Opcode::LOCK:
        case Opcode::UNLOCK:
            os << L("shared#") << v(ins.intValue);
            break;
        case Opcode::LIST_GET:
            os << s(ins.dest) << " " << L("=") << " " << L("shared#") << v(ins.intValue)
               << L("[") << s(ins.left) << L("]");
            break;
        case Opcode::SHARED_EPOCH:
            os << s(ins.dest);
            break;
        case Opcode::WAIT:
        case Opcode::THREAD_STOP:
        case Opcode::THREAD_JOIN:
            os << s(ins.src);
            break;
        case Opcode::THREAD_RUNNING:
            os << s(ins.dest) << " " << L("=") << " " << s(ins.src);
            break;
        case Opcode::THREAD_ARG:
            os << s(ins.dest) << " " << L("=") << " " << L("arg#") << v(ins.intValue);
            break;
        case Opcode::THREAD_SPAWN:
            os << s(ins.dest) << " " << L("=") << " "
               << p.fn() << ins.functionName << reset << L("(");
            for (size_t j = 0; j < ins.argSlots.size(); ++j) {
                if (j) os << L(", ");
                os << s(ins.argSlots[j]);
            }
            os << L(")");
            break;

        case Opcode::BNOT:
            os << s(ins.dest) << " " << L("=") << " " << p.op() << "~" << reset << s(ins.src);
            break;
        case Opcode::FNEG:
            os << s(ins.dest) << " " << L("=") << " " << p.op() << "-" << reset << s(ins.src);
            break;

        case Opcode::LOAD_FLOAT:
            os << s(ins.dest) << " " << L("=") << " "
               << p.value() << ins.floatValue << reset;
            break;
        case Opcode::INT_TO_FLOAT:
            os << s(ins.dest) << " " << L("=") << " " << L("(float)") << s(ins.src);
            break;
        case Opcode::FLOAT_TO_INT:
            os << s(ins.dest) << " " << L("=") << " " << L("(int)") << s(ins.src);
            break;

        case Opcode::CAST_INT_TO_STR:
        case Opcode::CAST_FLOAT_TO_STR:
        case Opcode::CAST_BOOL_TO_STR:
            os << s(ins.dest) << " " << L("=") << " " << L("str(")
               << s(ins.src) << L(")");
            break;
        case Opcode::CAST_STR_TO_INT:
            os << s(ins.dest) << " " << L("=") << " " << L("int?(")
               << s(ins.src) << L(")") << nullableTag(ins, p);
            break;
        case Opcode::CAST_STR_TO_FLOAT:
            os << s(ins.dest) << " " << L("=") << " " << L("float?(")
               << s(ins.src) << L(")") << nullableTag(ins, p);
            break;
        case Opcode::CAST_FLOAT_TO_INT_CHECKED:
            os << s(ins.dest) << " " << L("=") << " " << L("int(")
               << s(ins.src) << L(")") << nullableTag(ins, p);
            break;

        case Opcode::STRUCT_NEW:
            os << s(ins.dest) << " " << L("=") << " " << L("struct<")
               << p.fn() << ins.functionName << reset << L(">[") << v(ins.intValue)
               << L(" alan]");
            break;
        case Opcode::FIELD_GET:
            os << s(ins.dest) << " " << L("=") << " " << s(ins.src)
               << L(".") << v(ins.intValue);
            break;
        case Opcode::FIELD_SET:
            os << s(ins.dest) << L(".") << v(ins.intValue)
               << " " << L("=") << " " << s(ins.right);
            break;

        case Opcode::ARRAY_NEW:
            os << s(ins.dest) << " " << L("=") << " " << L("array<");
            switch (ins.arrayElemKind) {
                case ArrayElemKind::Ref:     os << "ref";     break;
                case ArrayElemKind::Byte:    os << "byte";    break;
                case ArrayElemKind::Int:     os << "int";     break;
                case ArrayElemKind::LongInt: os << "long";    break;
                case ArrayElemKind::Float32: os << "f32";     break;
                case ArrayElemKind::Float64: os << "f64";     break;
                case ArrayElemKind::Decimal: os << "dec";     break;
            }
            os << L(">[") << v(ins.intValue) << L("]");
            break;
        case Opcode::ARRAY_GET:
            os << s(ins.dest) << " " << L("=") << " " << s(ins.left)
               << L("[") << s(ins.right) << L("]");
            break;
        case Opcode::ARRAY_SET:
            os << s(ins.dest) << L("[") << s(ins.left)
               << L("] =") << " " << s(ins.right);
            break;
        case Opcode::ARRAY_LEN:
            os << s(ins.dest) << " " << L("=") << " " << L("len(")
               << s(ins.src) << L(")");
            break;

        case Opcode::LOAD_GLOBAL:
            os << s(ins.dest) << " " << L("=") << " " << L("global[")
               << v(ins.intValue) << L("]");
            break;
        case Opcode::STORE_GLOBAL:
            os << L("global[") << v(ins.intValue) << L("] =") << " " << s(ins.src);
            break;

        case Opcode::LOAD_DECIMAL:
            os << s(ins.dest) << " " << L("=") << " "
               << p.value() << ins.decimalValue.toString() << reset << L("d");
            break;
        case Opcode::INT_TO_DECIMAL:
        case Opcode::FLOAT_TO_DECIMAL:
            os << s(ins.dest) << " " << L("=") << " " << L("(decimal)") << s(ins.src);
            break;
        case Opcode::DNEG:
            os << s(ins.dest) << " " << L("=") << " " << L("-d") << " " << s(ins.src);
            break;
        case Opcode::CAST_DECIMAL_TO_STR:
            os << s(ins.dest) << " " << L("=") << " " << L("str(")
               << s(ins.src) << L(")");
            break;
        case Opcode::CAST_DECIMAL_TO_FLOAT:
            os << s(ins.dest) << " " << L("=") << " " << L("float(")
               << s(ins.src) << L(")");
            break;
        case Opcode::CAST_DECIMAL_TO_INT:
            os << s(ins.dest) << " " << L("=") << " " << L("int(")
               << s(ins.src) << L(")") << nullableTag(ins, p);
            break;
        case Opcode::CAST_STR_TO_DECIMAL:
            os << s(ins.dest) << " " << L("=") << " " << L("decimal?(")
               << s(ins.src) << L(")") << nullableTag(ins, p);
            break;

        case Opcode::ENTER_TRY:
            os << L("err→") << s(ins.dest)
               << "  " << L("catch→") << v(ins.jumpTarget);
            break;

        // İkili opcode'lar yukarıda isBinaryOp erken-return'üyle render edilir;
        // -Wswitch için case'ler yine de gerekli (boş).
        case Opcode::ADD: case Opcode::SUB: case Opcode::MUL:
        case Opcode::DIV: case Opcode::MOD:
        case Opcode::POW: case Opcode::LPOW: case Opcode::FPOW: case Opcode::F32POW:
        case Opcode::FMOD: case Opcode::F32MOD:
        case Opcode::FADD: case Opcode::FSUB: case Opcode::FMUL: case Opcode::FDIV:
        case Opcode::BAND: case Opcode::BOR: case Opcode::BXOR:
        case Opcode::SHL: case Opcode::SHR:
        case Opcode::LESS: case Opcode::LESS_EQUAL:
        case Opcode::GREATER: case Opcode::GREATER_EQUAL:
        case Opcode::EQUAL_EQUAL: case Opcode::NOT_EQUAL:
        case Opcode::STRING_CONCAT:
        case Opcode::DADD: case Opcode::DSUB: case Opcode::DMUL:
        case Opcode::DDIV: case Opcode::DMOD:
        case Opcode::LADD: case Opcode::LSUB: case Opcode::LMUL:
        case Opcode::LDIV: case Opcode::LMOD:
        case Opcode::LBAND: case Opcode::LBOR: case Opcode::LBXOR:
        case Opcode::LSHL: case Opcode::LSHR:
        case Opcode::F32ADD: case Opcode::F32SUB: case Opcode::F32MUL: case Opcode::F32DIV:
            break;

        // #266: longint/float32 yükleme, dönüşüm ve tekli işlemler.
        case Opcode::LOAD_LONG:
            os << s(ins.dest) << " " << L("=") << " " << p.value() << ins.int64Value << "L" << reset;
            break;
        case Opcode::LOAD_FLOAT32:
            os << s(ins.dest) << " " << L("=") << " " << p.value() << ins.floatValue << "f" << reset;
            break;
        case Opcode::LNEG: case Opcode::F32NEG:
            os << s(ins.dest) << " " << L("=") << " " << p.op() << "-" << reset << s(ins.src);
            break;
        case Opcode::LBNOT:
            os << s(ins.dest) << " " << L("=") << " " << p.op() << "~" << reset << s(ins.src);
            break;
        case Opcode::INT_TO_LONG:
            os << s(ins.dest) << " " << L("=") << " " << L("(longint)") << s(ins.src);
            break;
        case Opcode::INT_TO_FLOAT32: case Opcode::FLOAT_TO_FLOAT32:
            os << s(ins.dest) << " " << L("=") << " " << L("(float)") << s(ins.src);
            break;
        case Opcode::FLOAT32_TO_FLOAT:
            os << s(ins.dest) << " " << L("=") << " " << L("(double)") << s(ins.src);
            break;
        case Opcode::FLOAT32_TO_INT:
            os << s(ins.dest) << " " << L("=") << " " << L("(int)") << s(ins.src);
            break;
        case Opcode::LONG_TO_INT_CHECKED:
            os << s(ins.dest) << " " << L("=") << " " << L("int?(") << s(ins.src) << L(")")
               << nullableTag(ins, p);
            break;
        case Opcode::CAST_INT_TO_BYTE_CHECKED:
            os << s(ins.dest) << " " << L("=") << " " << L("byte?(") << s(ins.src) << L(")")
               << nullableTag(ins, p);
            break;
        case Opcode::CAST_FLOAT_TO_LONG_CHECKED:
            os << s(ins.dest) << " " << L("=") << " " << L("longint?(") << s(ins.src) << L(")")
               << nullableTag(ins, p);
            break;
        case Opcode::CAST_STR_TO_LONG:
            os << s(ins.dest) << " " << L("=") << " " << L("longint?(") << s(ins.src) << L(")")
               << nullableTag(ins, p);
            break;
        case Opcode::CAST_STR_TO_FLOAT32:
            os << s(ins.dest) << " " << L("=") << " " << L("float?(") << s(ins.src) << L(")")
               << nullableTag(ins, p);
            break;
        case Opcode::CAST_LONG_TO_STR: case Opcode::CAST_FLOAT32_TO_STR:
            os << s(ins.dest) << " " << L("=") << " " << L("(string)") << s(ins.src);
            break;
        case Opcode::LEAVE_TRY:
            break;
    }
    return os.str();
}

}  // namespace IrDump

#endif  // SAQUT_IR_DUMP
