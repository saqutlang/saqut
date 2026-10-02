// #132 — Opcode spec tablosu birim testleri (çerçevesiz; assert + çıktı).
// Koşmak için: tests/run.sh
//
// Spec tablosu (OPCODE_LIST) tek kaynaktır: enum, opcodeName(), opcodeArity(),
// opcodeBackends() ve opcodeJitBaseSupported() buradan türetilir. Bu test
// türetilmiş yüzeyin tutarlılığını doğrular — yeni opcode eklemek = OPCODE_LIST'e
// tek satır eklemek, bu test türetilmiş tüm yüzeyleri otomatik kapsar.
#include "ir/instruction.hpp"
#include <cassert>
#include <iostream>
#include <string>

int main() {
    // 1) Her opcode: kanonik isim döner, "UNKNOWN" değil
    for (int i = 0; i < kOpcodeCount; ++i) {
        Opcode op = static_cast<Opcode>(i);
        std::string name = opcodeName(op);
        assert(!name.empty());
        assert(name != "UNKNOWN");
    }

    // 2) VM normatif backend'dir — TÜM opcode'lar OP_VM bayrağı taşır
    for (int i = 0; i < kOpcodeCount; ++i) {
        Opcode op = static_cast<Opcode>(i);
        assert((opcodeBackends(op) & OP_VM) != 0);
        assert(opcodeArity(op) >= 0 && opcodeArity(op) <= 4);
    }

    // 3) JIT temel destek — VM-only dilimler (struct/array/global/try)
    //    OP_JIT bayrağı taşımaz; skaler dilimler taşır.
    //
    // #221: LOAD_NULL artık JIT'te destekleniyor — nullable slot başına gizli
    // "isNull" yandaş register'ı ile. Bir Int register'ı 0 ile null'u ayıramaz,
    // bu yüzden null'luk ayrı bitte taşınır. Nullable slot'u null-farkında
    // OLMAYAN bir opcode tüketirse wholeProgramSupported reddeder (sessiz
    // yanlış cevap yerine eksik kapsam — ADR-037: VM normatif).
    assert(opcodeJitBaseSupported(Opcode::LOAD_NULL));
    // #228: Ref ailesi shadow stack ile açıldı — JIT'in ürettiği nesneler artık
    // GC'ye görünür. Talimata bağlı ek koşullar (STRUCT_NEW metadata'sı,
    // GET'lerin valueType'ı) mir_backend.cpp::opcodeSupported'da.
    assert(opcodeJitBaseSupported(Opcode::STRUCT_NEW));
    assert(opcodeJitBaseSupported(Opcode::FIELD_GET));
    assert(opcodeJitBaseSupported(Opcode::FIELD_SET));
    assert(opcodeJitBaseSupported(Opcode::ARRAY_NEW));
    assert(opcodeJitBaseSupported(Opcode::ARRAY_GET));
    assert(opcodeJitBaseSupported(Opcode::ARRAY_SET));
    assert(opcodeJitBaseSupported(Opcode::ARRAY_LEN));
    assert(opcodeJitBaseSupported(Opcode::LOAD_GLOBAL));
    assert(opcodeJitBaseSupported(Opcode::STORE_GLOBAL));
    assert(opcodeJitBaseSupported(Opcode::ENTER_TRY));
    assert(opcodeJitBaseSupported(Opcode::LEAVE_TRY));
    assert(opcodeJitBaseSupported(Opcode::THROW));
    assert(opcodeJitBaseSupported(Opcode::LOAD_CONST));
    assert(opcodeJitBaseSupported(Opcode::ADD));
    assert(opcodeJitBaseSupported(Opcode::FADD));
    assert(opcodeJitBaseSupported(Opcode::LADD));
    assert(opcodeJitBaseSupported(Opcode::F32ADD));
    assert(opcodeJitBaseSupported(Opcode::DADD));
    assert(opcodeJitBaseSupported(Opcode::CALL));

    // 4) Arite — örnek sınıflar (spec tablosundaki kurala göre)
    assert(opcodeArity(Opcode::LOAD_CONST) == 2);   // dest, intValue
    assert(opcodeArity(Opcode::LOAD_NULL) == 1);    // dest
    assert(opcodeArity(Opcode::ADD) == 3);          // dest, left, right
    assert(opcodeArity(Opcode::BNOT) == 2);         // dest, src
    assert(opcodeArity(Opcode::JMP) == 1);          // jumpTarget
    assert(opcodeArity(Opcode::JIF_FALSE) == 2);    // cond, jumpTarget
    assert(opcodeArity(Opcode::RETURN) == 1);       // src
    assert(opcodeArity(Opcode::CALL) == 3);         // dest, callee
    assert(opcodeArity(Opcode::CALLHOST) == 2);     // callee
    assert(opcodeArity(Opcode::LEAVE_TRY) == 0);    // operandsız
    assert(opcodeArity(Opcode::THROW) == 1);        // src
    assert(opcodeArity(Opcode::ENTER_TRY) == 2);    // dest, jumpTarget
    assert(opcodeArity(Opcode::ARRAY_NEW) == 3);    // dest, intValue, elemKind
    assert(opcodeArity(Opcode::CAST_STR_TO_INT) == 3); // dest, src, nullable bayrağı

    // 5) Sonuç türü sütunu (#297) — JIT register türü buradan çıkar; yanlış
    //    sütun derlemede değil --jit'te yanlış değer olarak görünür. Dönüşüm
    //    opcode'larının adı hedef türü söyler: ad ile sütun tutarlı olmalı.
    auto endsWith = [](const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() &&
               s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    for (int i = 0; i < kOpcodeCount; ++i) {
        Opcode op = static_cast<Opcode>(i);
        std::string name = opcodeName(op);
        std::string target = endsWith(name, "_CHECKED") ? name.substr(0, name.size() - 8) : name;
        if (target.find("_TO_") == std::string::npos) continue;
        OpResult r = opcodeResult(op);
        if (endsWith(target, "_TO_STR"))          assert(r == OpResult::Str);
        else if (endsWith(target, "_TO_FLOAT32")) assert(r == OpResult::Float32);
        else if (endsWith(target, "_TO_FLOAT"))   assert(r == OpResult::Float);
        else if (endsWith(target, "_TO_LONG"))    assert(r == OpResult::Long);
        else if (endsWith(target, "_TO_DECIMAL")) assert(r == OpResult::Decimal);
        else if (endsWith(target, "_TO_INT") || endsWith(target, "_TO_BYTE"))
            assert(r == OpResult::Int);
        else
            assert(!"dönüşüm opcode'u tanınmayan hedef türüne sahip — bu testi genişlet");
    }
    // Ailenin öneki türü söyler: F32* → float, L* aritmetik → longint, D* → decimal.
    assert(opcodeResult(Opcode::F32ADD) == OpResult::Float32);
    assert(opcodeResult(Opcode::LADD) == OpResult::Long);
    assert(opcodeResult(Opcode::DADD) == OpResult::Decimal);
    assert(opcodeResult(Opcode::FADD) == OpResult::Float);
    // Özel kurallı opcode'lar
    assert(opcodeResult(Opcode::LOAD_NULL) == OpResult::Null);
    assert(opcodeResult(Opcode::LOAD_SLOT) == OpResult::Copy);
    assert(opcodeResult(Opcode::CALL) == OpResult::Call);
    assert(opcodeResult(Opcode::CALLHOST) == OpResult::Host);
    assert(opcodeResult(Opcode::ARRAY_GET) == OpResult::ValueType);
    // dest'i okunan (yazılmayan) opcode'lar
    assert(opcodeResult(Opcode::FIELD_SET) == OpResult::None);
    assert(opcodeResult(Opcode::ARRAY_SET) == OpResult::None);

    // 6) Geçersiz opcode değerleri güvenli fallback döndürür
    Opcode bogus = static_cast<Opcode>(kOpcodeCount);
    assert(std::string(opcodeName(bogus)) == "UNKNOWN");
    assert(opcodeArity(bogus) == 0);
    assert(opcodeBackends(bogus) == 0);
    assert(opcodeResult(bogus) == OpResult::None);
    assert(!opcodeJitBaseSupported(bogus));

    std::cout << "test_opcode: TUM TESTLER GECTI (" << kOpcodeCount << " opcode)\n";
    return 0;
}
