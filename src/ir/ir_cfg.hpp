// ============================================================================
// saQut IR — Control Flow Graph (CFG) + BasicBlock
// ============================================================================
//
// DİZİN:   src/ir/ir_cfg.hpp
// KATMAN:  IR — CFG, optimizasyon pass'leri ve backend linearizasyonu
//
// AMAÇ:
//   Flat instruction list → CFG → optimizasyon → flat (linearize).
//   VM ve backend'ler CFG'yi GÖRMEZ — yalnızca linearize edilmiş flat
//   instruction list'ini alır. (#218, ADR-039)
//
// ANALİZ YÜZEYİ (GC/threading altyapı denetimi, docs/gc-threading-altyapi-
// denetimi.md): CFG optimizasyon sırasında kanonik temsildir; flat liste
// depolama/yürütme sözleşmesidir. Bu katman sağlar: exception kenarları
// (ENTER_TRY → catch), erişilemez blok temizliği, dominance tree (CHK
// algoritması) ve natural loop tespiti. Pass'ler blok yapısına bağlı yazılır,
// talimat indeksine değil — böylece ileride SSA'ya geçiş flat sözleşmeyi
// bozmaz.
//
// ============================================================================

#ifndef SAQUT_IR_CFG
#define SAQUT_IR_CFG

#include <cstring>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include "ir/instruction.hpp"
#include "ir/ir_color.hpp"   // TTY-aware renk — redirect'te ANSI yok (#141 deseni)
#include "ir/ir_dump.hpp"    // ortak operand renderer'ı (literal değerler, #218)

struct BasicBlock {
    int id = -1;
    int startIndex = 0;       // orijinal flat listedeki başlangıç
    int endIndex = 0;          // orijinal flat listedeki bitiş (exclusive)
    std::vector<Instruction> instructions;
    Opcode terminator = Opcode::RETURN;
    int jumpTarget = -1;       // block hedefi (block ID)
    std::vector<int> predecessors;
    std::vector<int> successors;
    // Exception kenarları (block ID): blok içindeki ENTER_TRY'lerin catch
    // hedefleri, talimat sırasına göre. Normal successors'tan AYRI tutulur
    // çünkü kenar blok sonundan değil, talimatın kendisinden çıkar; kenar
    // listesine (predecessors/successors) DAHİL EDİLİR — erişilebilirlik
    // ve dataflow catch bloğunu görmek zorundadır. linearize() sıradaki
    // ENTER_TRY'nin jumpTarget'ını bu listeden yeniden yazar (blok silinse
    // bile flat indeks doğru kalır).
    std::vector<int> exceptionTargets;

    std::string dump() const {
        // TTY-aware renk: gerçek terminalde renkli, redirect/pipe'ta düz.
        // Semantik metin (blok aralığı, kenar listesi, terminator) renkten
        // bağımsız aynıdır — yalnız renk kod noktaları eklenir/çıkarılır.
        std::ostringstream os;
        // Renk şeması (kullanıcı): blok adları gri, metadata koyu sarı,
        // opcode turuncu, CALLHOST/FFI kırmızı, slot değerleri açık mavi.
        os << IrColor::SoftGri() << "BB_" << id << IrColor::Reset()
           << IrColor::KoyuSari() << " [" << startIndex << ".." << endIndex << "]"
           << " preds:{" << IrColor::Reset();
        for (size_t i = 0; i < predecessors.size(); ++i) {
            if (i) os << ",";
            os << IrColor::SoftGri() << "BB_" << predecessors[i] << IrColor::Reset();
        }
        os << IrColor::KoyuSari() << "} succs:{" << IrColor::Reset();
        for (size_t i = 0; i < successors.size(); ++i) {
            if (i) os << ",";
            os << IrColor::SoftGri() << "BB_" << successors[i] << IrColor::Reset();
        }
        if (!exceptionTargets.empty()) {
            os << IrColor::KoyuSari() << "} exc:{" << IrColor::Reset();
            for (size_t i = 0; i < exceptionTargets.size(); ++i) {
                if (i) os << ",";
                os << IrColor::SoftGri() << "BB_" << exceptionTargets[i]
                   << IrColor::Reset();
            }
        }
        os << IrColor::KoyuSari() << "} term=" << IrColor::Reset()
           << IrColor::KoyuSari() << opcodeName(terminator) << IrColor::Reset();
        if (jumpTarget >= 0)
            os << IrColor::KoyuSari() << " ->" << IrColor::Reset()
               << IrColor::SoftGri() << "BB_" << jumpTarget << IrColor::Reset();
        os << "\n";
        for (const auto& ins : instructions) {
            os << "    ";
            if (&ins == &instructions.back()) os << IrColor::SoftGri() << "* " << IrColor::Reset();
            else os << "  ";
            // CALLHOST (builtin metod + __ffi__) dış dünya çağrısı → kırmızı;
            // diğer opcode'lar turuncu. Operandlar (literal değerler dahil)
            // ortak renderer'dan — CFG paleti (ir_dump.hpp, #218).
            const char* opColor = (ins.opcode == Opcode::CALLHOST)
                ? IrColor::Kirmizi() : IrColor::SoftTuruncu();
            os << opColor << std::left << std::setw(16) << opcodeName(ins.opcode) << IrColor::Reset();
            // Sütunu dolduran uzun adlar (CAST_LONG_TO_STR) operandla bitişmesin.
            if (std::strlen(opcodeName(ins.opcode)) >= 16) os << ' ';
            os << IrDump::operands(ins, IrDump::kCfgPalette);
            os << "\n";
        }
        return os.str();
    }
};

// Natural loop: geri kenar (u→h, h u'yu dominate eder) başına bir kayıt.
// body artan blok ID sırasındadır; header = girişte döngünün başı.
struct NaturalLoop {
    int header = -1;
    std::vector<int> body;
};

struct CFG {
    std::vector<BasicBlock> blocks;

    // ── Analiz sonuçları (compute* çağrılana kadar boş) ─────────────────────
    std::vector<int> idom;  // blok → ani-dominatör (girişin ki -1); CHK
    std::vector<int> rpo;   // reverse postorder (dominance yürüyüş sırası)
    std::vector<NaturalLoop> loops;

    bool isValid() const {
        if (blocks.empty()) return false;
        for (const auto& b : blocks) {
            if (b.id < 0 || b.id >= (int)blocks.size()) return false;
        }
        return true;
    }

    // Erişilemez blokları (blok 0'dan successors üzerinden ulaşılamayanlar)
    // siler, kalanları yeniden numaralar, kenarları/analiz alanlarını tazeler.
    // Exception kenarları dahil olduğundan catch blokları asla silinmez.
    // Dönüş: silinen blok sayısı. Blok ID'leri değiştiğinden idom/rpo/loops
    // temizlenir — yeniden compute* çağrılmalı.
    int removeUnreachableBlocks();

    // Dominance tree (Cooper-Harvey-Kennedy). Tüm bloklar erişilebilir
    // olmalı (önce removeUnreachableBlocks). idom/rpo doldurur.
    void computeDominance();

    // Natural loop'lar; computeDominance sonrası çağrılmalı. loops doldurur.
    void computeLoops();

    // CFG → flat instruction list
    // VM bu listeyi alır, CFG'yi görmez.
    //
    // Jump hedefleri blok silinse/dizi değişse bile doğru kalır: blockID →
    // flat indeks çevirimi burada yapılır. İki talimat sınıfı yeniden yazılır:
    //   1. Blok terminator'ü JMP/JIF_* → block.jumpTarget (blok ID) üzerinden
    //   2. ENTER_TRY → block.exceptionTargets (blok ID), talimat sırasıyla
    std::vector<Instruction> linearize() const {
        std::vector<Instruction> result;
        std::vector<int> blockFlatStart(blocks.size() + 1, 0);
        for (size_t b = 0; b < blocks.size(); ++b)
            blockFlatStart[b + 1] = blockFlatStart[b] + (int)blocks[b].instructions.size();

        auto flatIndexOf = [&](int blockId) -> int {
            if (blockId >= 0 && blockId < (int)blocks.size())
                return blockFlatStart[(size_t)blockId];
            return -1;
        };

        for (const auto& block : blocks) {
            size_t enterTrySeen = 0;
            for (const auto& ins : block.instructions) {
                result.push_back(ins);
                Instruction& out = result.back();
                if (out.opcode == Opcode::ENTER_TRY &&
                    enterTrySeen < block.exceptionTargets.size()) {
                    out.jumpTarget = flatIndexOf(block.exceptionTargets[enterTrySeen++]);
                }
            }

            if (!result.empty()) {
                Instruction& last = result.back();
                if ((last.opcode == Opcode::JMP ||
                     last.opcode == Opcode::JIF_FALSE ||
                     last.opcode == Opcode::JIF_TRUE) && block.jumpTarget >= 0) {
                    last.jumpTarget = flatIndexOf(block.jumpTarget);
                }
            }
        }
        return result;
    }

    std::string dump() const {
        std::ostringstream os;
        os << IrColor::SoftGri() << "CFG: " << IrColor::Reset()
           << IrColor::SoftTuruncu() << blocks.size() << IrColor::Reset()
           << " blocks\n";
        // Doğal döngüler (computeLoops çağrılmışsa): her döngü başlığı ve
        // gövdesi tek satırda. Gövdedeki bloklar aynı zamanda dominatör
        // ağacında başlığın altındadır — geri kenarın tanımı gereği.
        for (const auto& loop : loops) {
            os << IrColor::SoftGri() << "loop: header=BB_" << loop.header
               << " body={" << IrColor::Reset();
            for (size_t i = 0; i < loop.body.size(); ++i) {
                if (i) os << ",";
                os << IrColor::SoftGri() << "BB_" << loop.body[i]
                   << IrColor::Reset();
            }
            os << IrColor::SoftGri() << "}" << IrColor::Reset() << "\n";
        }
        for (const auto& b : blocks)
            os << b.dump();
        return os.str();
    }
};

// Forward: CFG builder
// buildCFG(instructions) → CFG
// implementasyon ir_cfg.cpp'de
CFG buildCFG(const std::vector<Instruction>& instructions);

// a, b'yi dominate ediyor mu? (computeDominance çağrılmış olmalı)
bool cfgDominates(const CFG& cfg, int a, int b);

#endif
