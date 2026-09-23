// ============================================================================
// saQut Benchmark — Aşama Profiler
//
// Yalnızca `saqut bench` komutu tarafından kullanılır.
// Normal derleme/çalışma pipeline'ına sıfır etkisi vardır.
//
// TASARIM KURALI (kullanıcı isteği):
//   VM çalışırken hesap/karşılaştırma YOK — yalnız kaydet, sonra türet.
//
//   Eski gerçekleme bunu "her talimatta rdtsc + iki push_back" ile yapıyordu.
//   Ölçüm (aşağıda, BenchVMTrace) bunun talimat başına ~8.1 ns'ye ve
//   cpu_heavy.sqt'te %13 VM yavaşlamasına mal olduğunu gösterdi — yani
//   profiler ölçtüğü şeyi kayda değer biçimde değiştiriyordu.
//
//   Bugünkü tasarım sayımı süreden ayırır: dağılım her talimatta sabit
//   histogramla (kayıpsız), süre ise seyrek örneklemeyle (istatistiksel)
//   toplanır. Ayrıntı ve ölçüm tablosu BenchVMTrace başlığında.
//
// DONANIM ZAMANLAMA:
//   x86/x86_64: __rdtsc() — serializing değil ama ucuz da değil; ölçümde
//     çağrı + iki push_back birlikte ~8.1 ns tuttu (bkz. BenchVMTrace).
//   Diğerleri: std::chrono::steady_clock.
//   analyzeVMTrace() içinde tek seferlik TSC kalibrasyon yapılır.
// ============================================================================

#pragma once

#include <algorithm>
#include <climits>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ── Donanım zaman damgası ────────────────────────────────────────────────────
#if defined(__x86_64__) || defined(__i386__)
#  include <x86intrin.h>
static inline uint64_t benchTick() { return __rdtsc(); }
static constexpr bool kUseTSC = true;
#else
#  include <chrono>
static inline uint64_t benchTick() {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}
static constexpr bool kUseTSC = false;
#endif

// ── VM Execution Trace ───────────────────────────────────────────────────────
// VM çalışırken: sadece sayaç artışı + seyrek örnekleme — hesap yok.
// VM bittikten sonra: analyzeVMTrace() ile istatistik türet.
//
// ÖLÇÜLMÜŞ MALİYET (50M iterasyon, -O2, bu makine):
//   rdtsc + 2×push_back (eski tasarım) : 407 ms  (~8.1 ns/talimat)
//   yalnız opcode push_back            :  35 ms  (~0.7 ns/talimat)
//   histogram ++hist[op]               :  18 ms  (~0.36 ns/talimat)
// Yani maliyetin ~%91'i RDTSC'ydi. cpu_heavy.sqt üzerinde bu, VM süresini
// 6.27 s → 7.09 s'ye çıkarıyordu (%13 overhead) ve profil raporundaki VM
// süresini gerçek koşudan sistematik olarak saptırıyordu.
//
// YENİ TASARIM — sayım kayıpsız, süre örneklenmiş:
//   - HER talimatta: ++counts[op]  → opcode DAĞILIMI tam doğru kalır.
//   - Her kSampleStride talimatta bir: ardışık iki tick alınıp o talimatın
//     süresi örneklenir. analyzeVMTrace süre istatistiğini (ort/min/max) bu
//     örneklemden türetir.
// Süre artık istatistiksel bir tahmindir; rapor bunu örneklem sayısıyla
// birlikte basar (kanıtsız kesinlik iddiası yok — AGENTS.md §5).
//
// Bellek: eski tasarım 8M talimatta ~72MB tutuyordu; yeni tasarım sabit
// 256×8B histogram + örneklem başına 9B. Stride 1024'te 8M talimat → ~70KB.

struct BenchVMTrace {
    // Örnekleme iki fazlıdır; sabit tek stride her iki ucu da idare edemez:
    //   - Kısa koşu (birkaç yüz talimat): stride 1024 olsaydı HİÇ örneklem
    //     toplanmaz, bütün süre sütunları "—" olurdu.
    //   - Uzun koşu (milyonlarca talimat): her talimatı örneklemek eski
    //     tasarımın %13 overhead'ini geri getirirdi.
    // Çözüm: ilk kWarmSamples talimat tam örneklenir (küçük programlar için
    // yeterli veri), sonrasında kSampleStride'a seyreltilir (uzun koşularda
    // maliyet talimat başına ~8.1/1024 ≈ 0.008 ns'ye iner).
    static constexpr uint32_t kWarmSamples  = 4096;
    static constexpr uint32_t kSampleStride = 1024;

    // Opcode DAĞILIMI — her talimatta artar, kayıpsız.
    uint64_t counts[256] = {};

    // Süre ÖRNEKLEMİ — yalnız her kSampleStride talimatta bir doldurulur.
    std::vector<uint8_t>  sampleOpcodes;
    std::vector<uint64_t> sampleDurations;

    // Lightweight sayaçlar — VM döngüsündeki null-check aynı yerde artar
    uint64_t vmLoopIter     = 0;  // toplam dispatch iterasyonu
    uint64_t vmSaqutCalls   = 0;  // CALL opcodu (saQut→saQut)
    uint64_t vmFfiCalls     = 0;  // CALLHOST (host FFI veya builtin)
    uint64_t vmBuiltinCalls = 0;  // CALLHOST + __builtin_method__

    void reserve(size_t hint) {
        size_t samples = std::min<size_t>(hint, kWarmSamples)
                       + hint / kSampleStride + 16;
        sampleOpcodes.reserve(samples);
        sampleDurations.reserve(samples);
    }

    // VM döngüsünden çağrılır — hesap yok, sadece kaydet.
    //
    // Örnekleme iki adımlıdır: pendingSample_ set edildiğinde BİR SONRAKİ
    // çağrı farkı kapatır. Böylece ölçülen süre "örneklenen talimatın dispatch
    // başlangıcından bir sonrakine kadar geçen zaman" olur — eski tasarımdaki
    // tick[i+1]-tick[i] semantiğiyle birebir aynı.
    inline void pushDispatch(uint8_t op) {
        ++counts[op];
        ++vmLoopIter;

        if (pendingSample_) [[unlikely]] {
            pendingSample_ = false;
            sampleOpcodes.push_back(pendingOp_);
            sampleDurations.push_back(benchTick() - pendingTick_);
        }

        // Isınma fazında her talimat, sonrasında her kSampleStride'da bir.
        const bool warm = vmLoopIter <= kWarmSamples;
        if (warm || ++sinceSample_ >= kSampleStride) [[unlikely]] {
            sinceSample_   = 0;
            pendingOp_     = op;
            pendingSample_ = true;
            pendingTick_   = benchTick();
        }
    }

private:
    uint64_t pendingTick_   = 0;
    uint32_t sinceSample_   = 0;
    uint8_t  pendingOp_     = 0;
    bool     pendingSample_ = false;
};

// ── Per-Opcode Analiz Sonucu ─────────────────────────────────────────────────
struct OpcodeStats {
    uint64_t count       = 0;  // kayıpsız — her çalışmada artan histogram
    uint64_t sampleCount = 0;  // süre örneklemi sayısı (0 ise süre bilinmiyor)
    uint64_t totalTick   = 0;  // yalnız örneklenen çalışmaların toplamı
    uint64_t minTick     = UINT64_MAX;
    uint64_t maxTick     = 0;

    // Örneklenen çalışmaların ortalama tick'i. Örneklem yoksa 0 döner —
    // çağıran taraf sampleCount'a bakıp "—" basmalıdır.
    uint64_t avgTick() const {
        return sampleCount ? totalTick / sampleCount : 0;
    }
    // count × ortalama: bu opcode'un toplam VM süresine tahmini katkısı.
    // Örneklem yoksa 0 (bilinmiyor).
    uint64_t estimatedTotalTick() const { return avgTick() * count; }
};

// ── Token İstatistikleri ─────────────────────────────────────────────────────
struct TokenStats {
    uint64_t total       = 0;
    uint64_t keywords    = 0;
    uint64_t identifiers = 0;
    uint64_t numbers     = 0;
    uint64_t strings     = 0;
    uint64_t operators_  = 0;
    uint64_t delimiters  = 0;
    uint64_t other       = 0;
    uint64_t fileCount   = 0;
};

// ── AST İstatistikleri ───────────────────────────────────────────────────────
struct ASTStats {
    uint64_t totalNodes   = 0;
    uint64_t declarations = 0;
    uint64_t statements   = 0;
    uint64_t expressions  = 0;
    uint64_t funcDecls    = 0;
    uint64_t varDecls     = 0;
    uint64_t importDecls  = 0;
};

// ── Sembol İstatistikleri ────────────────────────────────────────────────────
struct SymbolStats {
    uint64_t total      = 0;
    uint64_t functions  = 0;
    uint64_t variables  = 0;
    uint64_t parameters = 0;
    uint64_t structs    = 0;
    uint64_t enums      = 0;
    uint64_t passes     = 3;  // SymbolCollector her zaman 3 geçiş yapar
};

// ── IR İstatistikleri ────────────────────────────────────────────────────────
struct IRStats {
    uint64_t totalInstr  = 0;
    uint64_t funcCount   = 0;
    uint64_t ffiSites    = 0;  // CALLHOST talimatı sayısı (statik)
    uint64_t callSites   = 0;  // CALL talimatı sayısı (statik)
    std::unordered_map<std::string, uint64_t> staticOpcodes;  // opcode dağılımı (statik)
};

// ── Tam Profil ───────────────────────────────────────────────────────────────
struct BenchProfile {
    TokenStats  tok;
    ASTStats    ast;
    SymbolStats sym;
    IRStats     ir;
    BenchVMTrace vmTrace;

    uint64_t vmHeapAllocCount = 0;  // Heap::allocCount (toplam tahsis)

    // JIT çalıştırma istatistikleri
    // JIT kullanılmadığında varsayılan (0/false) kalır — zararsız.
    bool     jitUsed          = false;  // profil raporu yazdırırken VM/JIT ayırmak için
    uint64_t jitWarmupUs      = 0;     // IR→MIR çeviri + native derleme (jit-warmup)
    uint64_t jitExecUs        = 0;     // derlenmiş native main() çalıştırma (jit-exec)
    uint64_t jitCallhostCount = 0;    // CALLHOST trampoline çağrıları (FFI/builtin)
    uint64_t jitFfiCount      = 0;     // yalnızca FFI çağrıları
    uint64_t jitBuiltinCount  = 0;     // yalnızca builtin çağrıları

    // Analiz sonuçları — analyzeVMTrace() ile doldurulur
    std::unordered_map<std::string, OpcodeStats> opcodeResult;
    uint64_t tscHz = 0;  // kalibrasyon: tsc/saniye

    // ── TSC kalibrasyon ───────────────────────────────────────────────────────
    // Kısa bir chrono interval ile tsc/s ölçer.
    void calibrateTSC() {
        if (!kUseTSC) { tscHz = 1'000'000'000ULL; return; }
        // 10ms ölçüm — bench bağlamında ihmal edilebilir ek süre
        using Clk = std::chrono::steady_clock;
        auto  ta = Clk::now();
        uint64_t ra = benchTick();
        // busy-wait (sleep gerekmez — kalibrasyon için yeterli)
        uint64_t wait_ns = 10'000'000; // 10ms
        while (true) {
            auto tb = Clk::now();
            if ((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    tb - ta).count() >= wait_ns) {
                tscHz = (benchTick() - ra) * 1'000'000'000ULL /
                        (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                            tb - ta).count();
                break;
            }
        }
    }

    // ticks → nanosaniye
    uint64_t ticksToNs(uint64_t ticks) const {
        if (!tscHz) return ticks;
        return ticks * 1'000'000'000ULL / tscHz;
    }

    // ── VM trace analizi ──────────────────────────────────────────────────────
    // VM bittikten sonra çağrılır.
    // Parametre: opcode int → isim fonksiyonu (instruction.hpp'den opcodeName())
    // Sayım histogramdan (kayıpsız), süre örneklemden (istatistiksel) türetilir.
    // İki kaynak ayrıdır: bir opcode çalışmış ama hiç örneklenmemiş olabilir —
    // o durumda count > 0, sampleCount == 0 ve süre alanları raporda "—" basılır.
    // Örneklenmemiş süreyi sıfır veya tahmin olarak göstermek kanıtsız sayı
    // üretmek olurdu (AGENTS.md §5).
    void analyzeVMTrace(const char* (*nameFunc)(int)) {
        OpcodeStats tmp[256];

        for (int op = 0; op < 256; op++)
            tmp[op].count = vmTrace.counts[op];

        const auto& so = vmTrace.sampleOpcodes;
        const auto& sd = vmTrace.sampleDurations;
        for (size_t i = 0; i < so.size() && i < sd.size(); i++) {
            auto& s = tmp[so[i]];
            uint64_t dur = sd[i];
            s.totalTick += dur;
            s.sampleCount++;
            if (dur < s.minTick) s.minTick = dur;
            if (dur > s.maxTick) s.maxTick = dur;
        }

        opcodeResult.clear();
        for (int op = 0; op < 256; op++) {
            if (tmp[op].count == 0) continue;
            std::string name = nameFunc ? nameFunc(op) : std::to_string(op);
            opcodeResult[name] = tmp[op];
        }
    }
};

// ── Token istatistiklerini topla ─────────────────────────────────────────────
// Token::gettype() → "keyword" / "identifier" / "number" / "string" /
//                     "operator" / "delimiter"
#include "tokenizer/token.hpp"

inline void collectTokenStats(TokenStats& out,
                               const std::vector<std::vector<Token*>>& allTokens) {
    out.fileCount = allTokens.size();
    for (auto& toks : allTokens) {
        out.total += toks.size();
        for (auto* t : toks) {
            const std::string& ty = const_cast<Token*>(t)->gettype();
            if      (ty == "keyword")    ++out.keywords;
            else if (ty == "identifier") ++out.identifiers;
            else if (ty == "number")     ++out.numbers;
            else if (ty == "string")     ++out.strings;
            else if (ty == "operator")   ++out.operators_;
            else if (ty == "delimiter")  ++out.delimiters;
            else                         ++out.other;
        }
    }
}

// ── AST istatistiklerini topla ───────────────────────────────────────────────
// AST düğümleri hem getChildren() hem tipli pointer'lar kullanır.
// Doğru sayım için her düğüm tipine özgü pointer'lar da gezilir.
#include "parser/ast_node.hpp"
#include "parser/nodes/statements.hpp"
#include "parser/nodes/declarations.hpp"
#include "parser/nodes/binary_expr.hpp"
#include "parser/nodes/expressions.hpp"
#include "module/module_graph.hpp"

static void walkAST(ASTNode* node, ASTStats& out);

static void walkList(const std::vector<ASTNode*>& list, ASTStats& out) {
    for (auto* n : list) walkAST(n, out);
}

static void walkAST(ASTNode* node, ASTStats& out) {
    if (!node) return;
    ++out.totalNodes;

    switch (node->kind) {
        // ── Declarations ──────────────────────────────────────────────────────
        case ASTKind::FunctionDecl: {
            ++out.declarations; ++out.funcDecls;
            auto* fn = static_cast<FunctionDeclNode*>(node);
            for (auto* p : fn->params) walkAST(p, out);
            // body is in getChildren()
            break;
        }
        case ASTKind::VariableDecl: {
            ++out.declarations; ++out.varDecls;
            auto* vd = static_cast<VariableDeclNode*>(node);
            walkAST(vd->initExpr, out);
            break;
        }
        case ASTKind::ImportDecl:   ++out.declarations; ++out.importDecls; break;
        case ASTKind::StructDecl:
        case ASTKind::EnumDecl:
        case ASTKind::FfiDecl:      ++out.declarations; break;

        // ── Statements ────────────────────────────────────────────────────────
        case ASTKind::Block:
        case ASTKind::BreakStatement:
        case ASTKind::ContinueStatement:
            ++out.statements;
            break;

        case ASTKind::IfStatement: {
            ++out.statements;
            auto* n2 = static_cast<IfStatementNode*>(node);
            walkAST(n2->condition, out);
            walkAST(n2->thenBranch, out);
            walkAST(n2->elseBranch, out);
            break;
        }
        case ASTKind::WhileStatement: {
            ++out.statements;
            auto* n2 = static_cast<WhileStatementNode*>(node);
            walkAST(n2->condition, out);
            walkAST(n2->body, out);
            break;
        }
        case ASTKind::ForStatement: {
            ++out.statements;
            auto* n2 = static_cast<ForStatementNode*>(node);
            walkAST(n2->init, out);
            walkAST(n2->condition, out);
            walkAST(n2->update, out);
            walkAST(n2->body, out);
            break;
        }
        case ASTKind::DoWhileStatement: {
            ++out.statements;
            auto* n2 = static_cast<DoWhileStatementNode*>(node);
            walkAST(n2->condition, out);
            walkAST(n2->body, out);
            break;
        }
        case ASTKind::ReturnStatement: {
            ++out.statements;
            walkAST(static_cast<ReturnStatementNode*>(node)->value, out);
            break;
        }
        case ASTKind::ThrowStatement: {
            ++out.statements;
            walkAST(static_cast<ThrowStatementNode*>(node)->value, out);
            break;
        }
        case ASTKind::TryStatement: {
            ++out.statements;
            auto* n2 = static_cast<TryStatementNode*>(node);
            walkAST(n2->body, out);
            walkAST(n2->handler, out);
            break;
        }
        case ASTKind::SwitchStatement: {
            ++out.statements;
            auto* sw = static_cast<SwitchStatementNode*>(node);
            walkAST(sw->subject, out);
            for (auto& cas : sw->cases) {
                walkList(cas.values, out);
                walkList(cas.body, out);
            }
            break;
        }
        case ASTKind::ExpressionStatement: {
            ++out.statements;
            walkAST(static_cast<ExpressionStatementNode*>(node)->expression, out);
            break;
        }

        // ── Expressions ───────────────────────────────────────────────────────
        case ASTKind::BinaryExpression: {
            ++out.expressions;
            auto* n2 = static_cast<BinaryExpressionNode*>(node);
            walkAST(n2->Left, out);
            walkAST(n2->Right, out);
            break;
        }
        case ASTKind::UnaryExpression:
            // Şu an ayrı sınıf yok; parser BinaryExpressionNode/PostfixNode kullanır.
            ++out.expressions;
            break;
        case ASTKind::Postfix: {
            ++out.expressions;
            walkAST(static_cast<PostfixNode*>(node)->operand, out);
            break;
        }
        case ASTKind::Call: {
            ++out.expressions;
            auto* c = static_cast<CallExpressionNode*>(node);
            walkAST(c->callee, out);
            walkList(c->arguments, out);
            break;
        }
        case ASTKind::CastExpression: {
            ++out.expressions;
            walkAST(static_cast<CastExpressionNode*>(node)->operand, out);
            break;
        }
        case ASTKind::MemberAccess: {
            ++out.expressions;
            auto* n2 = static_cast<MemberAccessNode*>(node);
            walkAST(n2->object, out);
            // member alanı string — ASTNode* değil, gezilmez
            break;
        }
        case ASTKind::IndexExpression: {
            ++out.expressions;
            auto* n2 = static_cast<IndexExpressionNode*>(node);
            walkAST(n2->object, out);
            walkAST(n2->index, out);
            break;
        }
        case ASTKind::ArrayLiteral: {
            ++out.expressions;
            walkList(static_cast<ArrayLiteralNode*>(node)->elements, out);
            break;
        }
        case ASTKind::ScopeCall: {
            ++out.expressions;
            walkList(static_cast<ScopeCallNode*>(node)->arguments, out);
            break;
        }
        case ASTKind::Literal:
        case ASTKind::Identifier:
            ++out.expressions;
            break;

        // ── Program (root) ────────────────────────────────────────────────────
        case ASTKind::Program:
            break;

        // ── Faz 2: panic-mode kurtarma yer tutucusu — yaprak, children yok ────
        case ASTKind::Error:
            break;
    }
    // getChildren() üzerinden ulaşılabilen çocuklar (Block içindeki stmtler, vb.)
    for (ASTNode* child : node->getChildren())
        walkAST(child, out);
}

inline void collectASTStats(ASTStats& out, const std::vector<ModuleUnit>& units) {
    for (auto& u : units)
        walkAST(u.ast, out);
}

// ── Sembol istatistiklerini topla ────────────────────────────────────────────
#include "symbol/symbol_table.hpp"
#include "symbol/symbol.hpp"

inline void collectSymbolStats(SymbolStats& out, const SymbolTable& table) {
    for (Symbol* s : table.allSymbols()) {
        out.total++;
        switch (s->kind) {
            case SymbolKind::Function:   ++out.functions;  break;
            case SymbolKind::Variable:   ++out.variables;  break;
            case SymbolKind::Parameter:  ++out.parameters; break;
            case SymbolKind::Struct:     ++out.structs;    break;
            case SymbolKind::Enum:
            case SymbolKind::EnumValue:  ++out.enums;      break;
            case SymbolKind::Field:                        break;
        }
    }
}

// ── IR istatistiklerini topla ────────────────────────────────────────────────
#include "ir/ir_program.hpp"
#include "ir/instruction.hpp"

inline void collectIRStats(IRStats& out, const IRProgram& prog) {
    out.funcCount = prog.functions.size();
    // functions: unordered_map<string, IRFunction>
    for (auto& [name, fn] : prog.functions) {
        for (auto& ins : fn.instructions) {
            out.totalInstr++;
            const char* nm = opcodeName(ins.opcode);
            out.staticOpcodes[nm]++;
            if (ins.opcode == Opcode::CALLHOST) ++out.ffiSites;
            if (ins.opcode == Opcode::CALL)      ++out.callSites;
        }
    }
}

// ── Profil yazdır ────────────────────────────────────────────────────────────
inline void printBenchProfile(const BenchProfile& p,
                               uint64_t tokUs, uint64_t parseUs,
                               uint64_t symUs,  uint64_t tcUs,
                               uint64_t irUs,   uint64_t vmUs,
                               bool compileOnly) {
    auto fmtN = [](uint64_t n) -> std::string {
        // Binlik ayraç
        std::string s = std::to_string(n);
        int ins = (int)s.size() - 3;
        while (ins > 0) { s.insert(ins, "."); ins -= 3; }
        return s;
    };
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════╗\n";
    std::cout << "║             saQut Stage Profile Report               ║\n";
    std::cout << "╚══════════════════════════════════════════════════════╝\n\n";

    // ── Tokenizer ──────────────────────────────────────────────────────────
    auto& t = p.tok;
    std::cout << "┌─ [Tokenizer]  " << fmtN(tokUs) << " µs\n";
    std::cout << "│  Total tokens : " << fmtN(t.total) << "\n";
    std::cout << "│  Files        : " << fmtN(t.fileCount) << "\n";
    std::cout << "│  Keywords: " << fmtN(t.keywords)
              << "   Identifiers: " << fmtN(t.identifiers)
              << "   Numbers: " << fmtN(t.numbers) << "\n";
    std::cout << "│  String literals: " << fmtN(t.strings)
              << "   Operators: " << fmtN(t.operators_)
              << "   Delimiters: " << fmtN(t.delimiters) << "\n";
    std::cout << "│\n";

    // ── Parser ─────────────────────────────────────────────────────────────
    auto& a = p.ast;
    std::cout << "├─ [Parser]  " << fmtN(parseUs) << " µs\n";
    std::cout << "│  Total nodes  : " << fmtN(a.totalNodes) << "\n";
    std::cout << "│  Expressions  : " << fmtN(a.expressions)
              << "   Statements: " << fmtN(a.statements)
              << "   Declarations: " << fmtN(a.declarations) << "\n";
    std::cout << "│  Function decls: " << fmtN(a.funcDecls)
              << "   Variable decls: " << fmtN(a.varDecls)
              << "   Import: " << fmtN(a.importDecls) << "\n";
    std::cout << "│\n";

    // ── Sembol ─────────────────────────────────────────────────────────────
    // symUs ve tcUs AYRI aşamalardır; timing tablosu (bench.hpp) da onları
    // ayrı satırlarda raporlar. Toplanırlarsa aynı koşu iki farklı sembol
    // süresi gösterir ve "en yavaş aşama" sıralaması bozulur.
    auto& s = p.sym;
    std::cout << "├─ [Symbol Collection]  " << fmtN(symUs) << " µs\n";
    std::cout << "│  Passes         : " << s.passes << "\n";
    std::cout << "│  Total symbols  : " << fmtN(s.total) << "\n";
    std::cout << "│  Functions: " << fmtN(s.functions)
              << "   Variables: " << fmtN(s.variables)
              << "   Parameters: " << fmtN(s.parameters)
              << "   Struct: " << fmtN(s.structs) << "\n";
    std::cout << "│\n";

    // ── Tip denetimi ───────────────────────────────────────────────────────
    // Tip denetimi + yapısal doğrulama (bench.hpp aşama 4). Daha önce bu süre
    // "Sembol Toplama" satırına gömülüydü ve aşama hiç görünmüyordu.
    std::cout << "├─ [Type Checking]  " << fmtN(tcUs) << " µs\n";
    std::cout << "│  Type checking + structural validation\n";
    std::cout << "│\n";

    // ── IR ─────────────────────────────────────────────────────────────────
    auto& ir = p.ir;
    std::cout << "├─ [IR Generation]  " << fmtN(irUs) << " µs\n";
    std::cout << "│  Functions     : " << fmtN(ir.funcCount) << "\n";
    std::cout << "│  Instructions  : " << fmtN(ir.totalInstr) << "\n";
    std::cout << "│  CALL site     : " << fmtN(ir.callSites)
              << "   CALLHOST site: " << fmtN(ir.ffiSites) << "\n";
    std::cout << "│\n";

    // ── Çalıştırma (VM veya JIT) ────────────────────────────────────────────
    if (!compileOnly) {
        if (p.jitUsed) {
            // ── JIT ─────────────────────────────────────────────────────────
            std::cout << "├─ [JIT Compile/Warmup]  " << fmtN(p.jitWarmupUs) << " µs\n";
            std::cout << "│  IR→MIR lowering + native code generation\n";
            std::cout << "│\n";
            std::cout << "├─ [JIT Execution]  " << fmtN(p.jitExecUs) << " µs\n";
            std::cout << "│  CALLHOST (total) : " << fmtN(p.jitCallhostCount) << "\n";
            std::cout << "│  FFI calls        : " << fmtN(p.jitFfiCount) << "\n";
            std::cout << "│  Builtin calls    : " << fmtN(p.jitBuiltinCount) << "\n";
            std::cout << "│\n";
        } else {
            // ── VM ─────────────────────────────────────────────────────────
            auto& vm = p.vmTrace;
            std::cout << "├─ [VM Execution]  " << fmtN(vmUs) << " µs\n";
            std::cout << "│  Dispatch loop    : " << fmtN(vm.vmLoopIter) << "\n";
            std::cout << "│  saQut CALL       : " << fmtN(vm.vmSaqutCalls) << "\n";
            std::cout << "│  FFI (CALLHOST)   : " << fmtN(vm.vmFfiCalls) << "\n";
            std::cout << "│  Builtin methods  : " << fmtN(vm.vmBuiltinCalls) << "\n";
            std::cout << "│  Heap allocations : " << fmtN(p.vmHeapAllocCount) << " objects\n";
            std::cout << "│  Timing samples   : "
                      << fmtN(vm.sampleOpcodes.size())
                      << " samples (1 per "
                      << BenchVMTrace::kSampleStride << " instructions, ~"
                      << fmtN((vm.sampleOpcodes.size() * 9) / 1024)
                      << " KB)\n";
            std::cout << "│\n";
        }
    }

    // ── Opcode profili ─────────────────────────────────────────────────────
    // JIT'de native kod çalışır; opcode trace mevcut değil.
    if (!compileOnly && !p.jitUsed && !p.opcodeResult.empty()) {
        std::cout << "└─ [Opcode Profile: runtime distribution]\n\n";

        // Çalışma sayısına göre sırala (azalan)
        std::vector<std::pair<std::string, OpcodeStats>> sorted(
            p.opcodeResult.begin(), p.opcodeResult.end());
        std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) {
                return a.second.count > b.second.count;
            });

        // %Süre payı: örneklenen toplam DEĞİL, count × örneklem-ortalaması.
        // Örneklem seyrek olduğu için ham totalTick'ler opcode'lar arasında
        // farklı örneklem sayılarına dayanır; doğrudan oranlanırsa çok
        // çalışan ama az örneklenen opcode sistematik olarak küçük görünür.
        uint64_t grandTotal = 0;
        for (auto& [_, s] : sorted) grandTotal += s.estimatedTotalTick();

        // NOT: std::setw BAYT sayar, görünen karakteri değil. "Çalışma",
        // "%Süre", "Örneklem" gibi başlıklar UTF-8'de çok baytlıdır (ç,ş,ü,Ö
        // her biri 2 bayt) — setw'e ham verilirse sütun kayar. Genişliği
        // fazladan bayt kadar artırarak telafi ediyoruz.
        auto utf8Pad = [](const std::string& s, int visibleWidth) {
            int extra = 0;
            for (unsigned char c : s)
                if ((c & 0xC0) == 0x80) ++extra;  // devam baytı = görünmez
            return visibleWidth + extra;
        };
        const int W1 = 22, W2 = 12, W3 = 10, W4 = 10, W5 = 10, W6 = 8, W7 = 10;
        std::cout << std::left  << std::setw(W1) << "Opcode"
                  << std::right << std::setw(utf8Pad("Count", W2))  << "Count"
                  << std::right << std::setw(W3) << "Ort(ns)"
                  << std::right << std::setw(W4) << "Min(ns)"
                  << std::right << std::setw(W5) << "Max(ns)"
                  << std::right << std::setw(utf8Pad("%Time", W6))    << "%Time"
                  << std::right << std::setw(utf8Pad("Samples", W7)) << "Samples"
                  << "\n";
        std::string sep(W1 + W2 + W3 + W4 + W5 + W6 + W7, '-');
        std::cout << sep << "\n";

        for (const auto& [name, s] : sorted) {
            if (s.count == 0) continue;

            std::cout << std::left  << std::setw(W1) << name
                      << std::right << std::setw(W2) << fmtN(s.count);

            if (s.sampleCount == 0) {
                // Çalıştı ama hiç örneklenmedi — süre BİLİNMİYOR.
                // Sıfır basmak "0 ns sürdü" yanılgısı yaratırdı.
                // "—" U+2014, UTF-8'de 3 bayt → setw telafisi gerekir.
                const char* dash = "—";
                std::cout << std::right << std::setw(utf8Pad(dash, W3)) << dash
                          << std::right << std::setw(utf8Pad(dash, W4)) << dash
                          << std::right << std::setw(utf8Pad(dash, W5)) << dash
                          << std::right << std::setw(utf8Pad(dash, W6)) << dash
                          << std::right << std::setw(W7) << 0;
            } else {
                uint64_t avgNs = p.ticksToNs(s.avgTick());
                uint64_t minNs = (s.minTick != UINT64_MAX) ?
                    p.ticksToNs(s.minTick) : 0;
                uint64_t maxNs = p.ticksToNs(s.maxTick);
                double   pct   = grandTotal > 0 ?
                    100.0 * s.estimatedTotalTick() / grandTotal : 0.0;

                std::ostringstream pctBuf;
                pctBuf << std::fixed << std::setprecision(1) << pct << "%";

                std::cout << std::right << std::setw(W3) << avgNs
                          << std::right << std::setw(W4) << minNs
                          << std::right << std::setw(W5) << maxNs
                          << std::right << std::setw(W6) << pctBuf.str()
                          << std::right << std::setw(W7) << fmtN(s.sampleCount);
            }
            std::cout << "\n";
        }
        std::cout << "\n";
        std::cout << "  Note: counts are exact. Timing columns are estimates from one sample\n"
                  << "  every " << BenchVMTrace::kSampleStride << " instructions; \"—\" means the opcode "
                  << "was never sampled.\n\n";
    } else if (!compileOnly && p.jitUsed) {
        std::cout << "└─ (Opcode profile skipped: not available in JIT mode)\n\n";
    } else if (compileOnly) {
        std::cout << "└─ (Execution skipped: no opcode profile)\n\n";
    }
}
