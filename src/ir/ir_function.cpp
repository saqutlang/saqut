// ============================================================================
// saQut IR — IRFunction Gerçeklemesi
// ============================================================================
//
// DİZİN:   src/ir/ir_function.cpp
// KATMAN:  IR — Fonksiyonun IR karşılığı (instruction listesi + slotlar)
//
// AMAÇ:
//   IRFunction::dump() ile debug çıktısı ve slot isim çözümlemesi.
//
// ============================================================================

#include <cstring>
#include "ir/ir_function.hpp"
#include "data/data_registry.hpp"
#include "ir/ir_color.hpp"
#include "ir/ir_dump.hpp"
#include "tools.hpp"
#include <iomanip>
#include <iostream>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Yardımcılar
// ─────────────────────────────────────────────────────────────────────────────

// Slot adını kısa göster: s0, s1, ...
static std::string slot(int s) {
    if (s == -1) return "?";
    return "s" + std::to_string(s);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRFunction::dump
// ─────────────────────────────────────────────────────────────────────────────

void IRFunction::dump() const {
    // Başlık: NAME=fibonacci PARAMS=1 SLOTS=10
    std::cout << IrColor::SoftGri() << "NAME=" << IrColor::Reset()
              << IrColor::SoftMor() << name << IrColor::Reset()
              << IrColor::SoftGri() << " PARAMS=" << IrColor::Reset()
              << IrColor::SoftTuruncu() << paramCount << IrColor::Reset()
              << IrColor::SoftGri() << " SLOTS=" << IrColor::Reset()
              << IrColor::SoftTuruncu() << slotCount << IrColor::Reset()
              << "\n";

    // Talimatlar
    for (int i = 0; i < (int)instructions.size(); i++) {
        const Instruction& ins = instructions[i];

        // Satır numarası
        std::cout << "  " << IrColor::SoftGri() << std::setw(3) << std::right << i << IrColor::Reset() << "  ";

        // Opcode sütunu
        std::cout << IrColor::SoftMor() << std::left << std::setw(16) << opcodeName(ins.opcode) << IrColor::Reset();
        // Sütunu dolduran uzun adlar (CAST_LONG_TO_STR) operandla bitişmesin.
        if (std::strlen(opcodeName(ins.opcode)) >= 16) std::cout << ' ';

        // Operandlar — ortak renderer (ir_dump.hpp, flat paleti); literal
        // değerler, slot'lar ve callee adları tek kaynaktan (#218).
        std::cout << IrDump::operands(ins, IrDump::kFlatPalette);

        std::cout << "\n";
    }
    std::cout << "\n";
}
