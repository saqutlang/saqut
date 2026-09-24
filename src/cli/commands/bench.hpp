// ============================================================================
// saQut CLI — bench komutu
//
// Pipeline'ın her aşamasını ayrı ayrı ölçer:
//   tokenize | parse | symbol-collect | type-check | ir-gen | vm-execute
//
// Kullanım:
//   saqut bench source.sqt [--runs=N] [--compile-only]
//
// Çalışma akışı:
//   1. N timing çalışması — temiz, profil yükü yok, avg/best raporlanır.
//   2. 1 profil çalışması — trace etkin, aşama istatistikleri + opcode profili.
//      Bu çalışmanın süresi, trace yükü nedeniyle timing çalışmalarından farklıdır;
//      ayrıca raporlanır.
// ============================================================================

#ifndef SAQUT_CLI_BENCH
#define SAQUT_CLI_BENCH

#include <algorithm>
#include <numeric>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <filesystem>

#include "bench/profile.hpp"
#include "cli/args.hpp"
#include "tokenizer/tokenizer.hpp"
#include "parser/parser.hpp"
#include "parser/nodes/declarations.hpp"
#include "core/module_registry.hpp"
#include "module/module_graph.hpp"
#include "symbol/symbol_table.hpp"
#include "symbol/symbol_collector.hpp"
#include "semantic/type_checker.hpp"
#include "semantic/structural_validator.hpp"
#include "diagnostic/diagnostic_engine.hpp"
#include "ir/ir_generator.hpp"
#include "ir/instruction.hpp"
#include "vm/interpreter.hpp"
#include "mir/mir_backend.hpp"

namespace fs = std::filesystem;

// ── Yardımcı: dosyayı oku ────────────────────────────────────────────────────
static std::string benchReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ── Bağımlılık tarama (BFS, ModuleLoader atlıyor) ───────────────────────────
static std::vector<std::pair<std::string,std::string>>
discoverModules(const std::string& entryPath) {
    std::vector<std::pair<std::string,std::string>> result;
    std::unordered_set<std::string> seen;
    std::vector<std::string> queue;

    auto canon = [](const std::string& p) {
        return fs::weakly_canonical(p).string();
    };
    queue.push_back(canon(entryPath));

    while (!queue.empty()) {
        std::string cur = queue.front();
        queue.erase(queue.begin());
        if (seen.count(cur)) continue;
        seen.insert(cur);

        std::string src = benchReadFile(cur);
        if (src.empty()) continue;
        result.push_back({cur, src});

        Tokenizer tok;
        auto tokens = tok.scan(src, cur);
        Parser par;
        ASTNode* ast = par.parse(tokens);
        if (!ast) { for (auto* t : tokens) delete t; continue; }

        for (ASTNode* child : ast->getChildren()) {
            if (child->kind != ASTKind::ImportDecl) continue;
            auto* imp = static_cast<ImportDeclNode*>(child);
            if (imp->sourcePath.empty()) continue;
            fs::path base   = fs::path(cur).parent_path();
            std::string dep = canon((base / imp->sourcePath).string());
            if (!seen.count(dep)) queue.push_back(dep);
        }
        delete ast;
        for (auto* t : tokens) delete t;
    }
    return result;
}

// ── Zamanlama yardımcıları ───────────────────────────────────────────────────
using BClock     = std::chrono::high_resolution_clock;
using BTimePoint = BClock::time_point;
using BMicros    = long long;

static BMicros elapsed_us_b(BTimePoint a, BTimePoint b) {
    return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
}

struct PhaseResult {
    std::string      name;
    std::vector<BMicros> samples;
    BMicros avg()  const {
        if (samples.empty()) return 0;
        long long s = 0;
        for (auto v : samples) s += v;
        return s / (long long)samples.size();
    }
    BMicros best() const {
        if (samples.empty()) return 0;
        return *std::min_element(samples.begin(), samples.end());
    }
};

// ── Timing tablosu ───────────────────────────────────────────────────────────
static void printTimingTable(const std::vector<PhaseResult>& phases, int runs,
                              const std::string& file, bool compileOnly,
                              const std::string& execLabel) {
    std::cout << "\n=== saQut bench: " << file << " ===\n";
    std::cout << "Timing runs: " << runs;
    if (compileOnly) std::cout << " | compile-only (execution skipped)";
    std::cout << "\n\n";

    const int W1 = 20, W2 = 12, W3 = 12;
    std::string sep(W1 + W2 + W3 + 6, '-');

    std::cout << std::left  << std::setw(W1) << "Stage"
              << std::right << std::setw(W2) << "Avg (µs)"
              << std::right << std::setw(W3) << "Best (µs)"
              << "\n" << sep << "\n";

    BMicros compileAvg = 0, compileBest = 0;
    for (auto& p : phases) {
        if (p.name == execLabel) continue;
        compileAvg  += p.avg();
        compileBest += p.best();
    }
    for (auto& p : phases) {
        std::cout << std::left  << std::setw(W1) << p.name
                  << std::right << std::setw(W2) << p.avg()
                  << std::right << std::setw(W3) << p.best()
                  << "\n";
    }
    std::cout << sep << "\n";
    std::cout << std::left  << std::setw(W1) << "compile-total"
              << std::right << std::setw(W2) << compileAvg
              << std::right << std::setw(W3) << compileBest << "\n";

    if (!compileOnly) {
        BMicros execAvg = 0, execBest = 0;
        for (auto& p : phases) {
            if (p.name == execLabel) { execAvg = p.avg(); execBest = p.best(); }
        }
        std::cout << std::left  << std::setw(W1) << "total"
                  << std::right << std::setw(W2) << (compileAvg + execAvg)
                  << std::right << std::setw(W3) << (compileBest + execBest) << "\n";
    }
    std::cout << "\n";
}

// ── Opcode isim köprüsü (Opcode enum → const char*) ─────────────────────────
static const char* opcodeNameBridge(int op) {
    return opcodeName(static_cast<Opcode>(op));
}

// ── Tek pipeline çalışması ───────────────────────────────────────────────────
// Hem timing hem profil çalışmaları aynı kodu paylaşır.
// profile != nullptr ise token/AST/IR/VM istatistikleri de toplanır.
struct PipelineTimes {
    BMicros tokUs = 0, parseUs = 0, symUs = 0, tcUs = 0, irUs = 0, vmUs = 0;
    BMicros jitWarmupUs = 0;  // yalnızca JIT modunda: IR→MIR+native derleme süresi
    std::vector<BMicros> jitExecSamples;
};

static bool runPipeline(
    const std::vector<std::pair<std::string,std::string>>& fileSources,
    bool compileOnly,
    bool useJit,
    const std::vector<std::string>& programArgs,
    PipelineTimes& out,
    BenchProfile*  profile,   // null = timing modu, non-null = profil modu
    bool verbose = false,
    int jitExecutionRuns = 1)
{
    size_t fileCount = fileSources.size();

    // ── 1: Tokenize ──────────────────────────────────────────────────────────
    auto t0 = BClock::now();
    std::vector<std::vector<Token*>> allTokens;
    allTokens.reserve(fileCount);
    for (auto& [path, src] : fileSources) {
        Tokenizer tok;
        allTokens.push_back(tok.scan(src, path));
    }
    auto t1 = BClock::now();
    out.tokUs = elapsed_us_b(t0, t1);

    if (profile)
        collectTokenStats(profile->tok, allTokens);
    if (verbose) std::cerr << "  tokenize  " << out.tokUs/1000 << " ms\n";

    // ── 2: Parse ─────────────────────────────────────────────────────────────
    auto t2 = BClock::now();
    ModuleRegistry registry;
    ModuleGraph    graph;
    graph.units.reserve(fileCount);
    bool parseOk = true;
    for (size_t i = 0; i < fileCount; i++) {
        Parser p;
        ASTNode* ast = p.parse(allTokens[i]);
        if (!ast) { parseOk = false; break; }
        ModuleUnit unit;
        unit.filePath = fileSources[i].first;
        unit.moduleId = registry.intern(fileSources[i].first);
        unit.ast      = ast;
        unit.tokens   = std::move(allTokens[i]);
        graph.units.push_back(std::move(unit));
    }
    auto t3 = BClock::now();
    if (!parseOk) return false;
    out.parseUs = elapsed_us_b(t2, t3);

    if (profile)
        collectASTStats(profile->ast, graph.units);
    if (verbose) std::cerr << "  parse     " << out.parseUs/1000 << " ms\n";

    // ── 3: Sembol toplama ────────────────────────────────────────────────────
    SymbolTable      symTable;
    DiagnosticEngine diag;
    auto t4 = BClock::now();
    SymbolCollector(symTable, diag).collectModuleGraph(graph);
    auto t5 = BClock::now();
    out.symUs = elapsed_us_b(t4, t5);

    if (diag.hasErrors()) {
        diag.printAll(std::cerr);
        return false;
    }
    if (verbose) std::cerr << "  symbol    " << out.symUs/1000 << " ms\n";

    // ── 4: Tip denetimi + yapısal doğrulama ──────────────────────────────────
    auto t6 = BClock::now();
    for (auto& u : graph.units) TypeChecker(symTable, diag).check(u.ast);
    for (auto& u : graph.units) StructuralValidator(diag).validate(u.ast);
    auto t7 = BClock::now();
    out.tcUs = elapsed_us_b(t6, t7);

    if (profile)
        collectSymbolStats(profile->sym, symTable);
    if (verbose) std::cerr << "  typecheck " << out.tcUs/1000 << " ms\n";

    // ── 5: IR üretimi ────────────────────────────────────────────────────────
    auto t8 = BClock::now();
    IRGenerator irGen;
    IRProgram   program = irGen.generateModuleGraph(graph, symTable);
    auto t9 = BClock::now();
    out.irUs = elapsed_us_b(t8, t9);

    if (profile)
        collectIRStats(profile->ir, program);
    if (verbose) std::cerr << "  ir-gen    " << out.irUs/1000 << " ms\n";

    // ── 6: Çalıştırma (VM veya JIT) ──────────────────────────────────────────
    if (!compileOnly) {
        if (useJit) {
            // JIT yolu — IRProgram'u native koda derleyip main()'i çağırır.
            // Sayaçlar yalnızca profil modunda toplanır (nullptr = sıfır ek yük).
            mir_backend::JitCallCounters counters;
            if (profile) {
                counters.callhost = &profile->jitCallhostCount;
                counters.ffi      = &profile->jitFfiCount;
                counters.builtin  = &profile->jitBuiltinCount;
            }
            int                             jitResult = 0;
            mir_backend::UnsupportedReason  reason;
            Profiling::StageTimer           stageTimer;

            auto ta = BClock::now();
            bool jitOk = mir_backend::tryCompileAndRunProgram(
                program, jitResult, reason, programArgs, &stageTimer,
                profile ? &counters : nullptr,
                jitExecutionRuns, &out.jitExecSamples,
                (profile == nullptr && jitExecutionRuns > 1)
                    ? std::function<void(int, int)>([](int run, int total) {
                          std::cerr << "\r[bench] timing " << run << "/" << total
                                    << "  " << std::flush;
                      })
                    : std::function<void(int, int)>{});
            auto tb = BClock::now();
            // Warmup (derleme) süresi ayrı tutulur — timing tablosundaki
            // jit-execute yalnızca native çalıştırmayı göstersin (VM'deki
            // vm-execute ile adil kıyas). ScopedStage overhead'i ns
            // seviyesindedir; µs seviyesindeki ölçümü etkilemez.
            out.jitWarmupUs = stageTimer.microsecondsFor("jit-warmup");
            out.vmUs = out.jitExecSamples.empty()
                ? elapsed_us_b(ta, tb) - out.jitWarmupUs
                : (BMicros)std::accumulate(out.jitExecSamples.begin(),
                                           out.jitExecSamples.end(), BMicros{0});
            if (out.jitExecSamples.size() > 1)
                out.vmUs /= (BMicros)out.jitExecSamples.size();
            if (out.vmUs < 0) out.vmUs = 0;
            if (verbose) std::cerr << "  jit       " << out.vmUs/1000 << " ms"
                                   << " (+warmup " << out.jitWarmupUs/1000 << " ms)\n";

            if (!jitOk) {
                std::cerr << "bench: --jit cannot compile this program completely (function '"
                          << reason.functionName << "', unsupported opcode: "
                          << reason.opcodeName << "); not falling back to the VM silently.\n";
                return false;
            }
            if (profile) {
                profile->jitUsed     = true;
                profile->jitWarmupUs = (uint64_t)out.jitWarmupUs;
                profile->jitExecUs   = (uint64_t)out.vmUs;
            }
        } else {
            // VM yolu — Interpreter üzerinde dispatch döngüsü.
            Interpreter vm(program);
            if (profile) {
                // Profil modunda örneklem rezervasyonu + trace'i aktif et.
                // reserve() içeride kSampleStride'a böler — burada verilen sayı
                // beklenen TALİMAT sayısıdır, örneklem sayısı değil.
                size_t estimatedInstr = profile->ir.totalInstr * 100; // çalışma sayısı tahmini
                if (estimatedInstr < 1'000'000)  estimatedInstr = 1'000'000;
                if (estimatedInstr > 50'000'000) estimatedInstr = 50'000'000; // 50M üst sınır
                profile->vmTrace.reserve(estimatedInstr);
                vm.setVMTrace(&profile->vmTrace);
            }
            auto ta = BClock::now();
            try {
                vm.run();
            } catch (const std::exception& e) {
                std::cerr << "bench: runtime error: " << e.what() << "\n";
            }
            auto tb = BClock::now();
            out.vmUs = elapsed_us_b(ta, tb);
            if (verbose) std::cerr << "  vm        " << out.vmUs/1000 << " ms\n";

            if (profile) {
                profile->vmHeapAllocCount = (uint64_t)vm.heapAllocCount();
                // VM trace analizi — işlem BİTTİKTEN sonra
                profile->calibrateTSC();
                profile->analyzeVMTrace(opcodeNameBridge);
            }
        }
    }

    return true;
}

// ── Ana komut ─────────────────────────────────────────────────────────────────
inline int cmdBench(const CliArgs& args) {
    std::string filePath = inputFilePath(args);
    if (filePath.empty()) {
        std::cerr << "bench: an input file is required\n";
        std::cerr << "usage: saqut bench <file.sqt> [--runs=N] [--compile-only]\n";
        return 1;
    }

    const int  N           = std::max(1, args.benchRuns);
    const bool compileOnly = args.compileOnly;
    const bool verbose     = args.verbose;
    const bool useJit      = args.useJit;
    const std::vector<std::string>& programArgs = args.programArgs;
    const std::string execLabel = useJit ? "jit-execute" : "vm-execute";

    // ── Bağımlılık tarama ────────────────────────────────────────────────────
    auto fileSources = discoverModules(filePath);
    if (fileSources.empty()) {
        std::cerr << "bench: cannot read or parse file: " << filePath << "\n";
        return 1;
    }

    size_t totalBytes = 0;
    for (auto& [_, src] : fileSources) totalBytes += src.size();

    std::cerr << "[bench] " << fileSources.size() << " module(s), "
              << (totalBytes / 1024) << " KB\n";
    std::cerr << "[bench] " << N << " timing run(s)...\n";

    // ── N timing çalışması (profil YOK) ─────────────────────────────────────
    PhaseResult rTok  {"tokenize",       {}};
    PhaseResult rPar  {"parse",          {}};
    PhaseResult rSym  {"symbol-collect", {}};
    PhaseResult rTc   {"type-check",     {}};
    PhaseResult rIr   {"ir-gen",         {}};
    PhaseResult rWarm {"jit-warmup",     {}};  // yalnızca JIT modunda doldurulur
    PhaseResult rExec {execLabel,        {}};

    if (useJit && !compileOnly) {
        // JIT timing contract: compile/native warmup exactly once, then reuse
        // the same native function for all N execution samples.
        PipelineTimes pt;
        if (!runPipeline(fileSources, false, true, programArgs, pt, nullptr,
                         verbose, N))
            return 1;
        rTok.samples.push_back(pt.tokUs);
        rPar.samples.push_back(pt.parseUs);
        rSym.samples.push_back(pt.symUs);
        rTc.samples.push_back(pt.tcUs);
        rIr.samples.push_back(pt.irUs);
        rWarm.samples.push_back(pt.jitWarmupUs);
        for (BMicros sample : pt.jitExecSamples)
            rExec.samples.push_back(sample);
    } else for (int run = 0; run < N; run++) {
        if (verbose) std::cerr << "[run " << (run+1) << "/" << N << "]\n";
        PipelineTimes pt;
        if (!runPipeline(fileSources, compileOnly, useJit, programArgs,
                         pt, nullptr, verbose))
            return 1;
        rTok.samples.push_back(pt.tokUs);
        rPar.samples.push_back(pt.parseUs);
        rSym.samples.push_back(pt.symUs);
        rTc.samples.push_back(pt.tcUs);
        rIr.samples.push_back(pt.irUs);
        if (useJit && !compileOnly) rWarm.samples.push_back(pt.jitWarmupUs);
        if (!compileOnly) rExec.samples.push_back(pt.vmUs);

        if (N > 3)
            std::cerr << "\r[bench] timing " << (run + 1) << "/" << N << "  " << std::flush;
    }
    if (N > 3) std::cerr << "\n";

    // ── Timing tablosunu yazdır ───────────────────────────────────────────────
    std::vector<PhaseResult> phases = {rTok, rPar, rSym, rTc, rIr};
    if (useJit && !compileOnly) phases.push_back(rWarm);
    if (!compileOnly) phases.push_back(rExec);
    printTimingTable(phases, N, filePath, compileOnly, execLabel);

    double compileSec = (rTok.best() + rPar.best() + rSym.best() +
                         rTc.best()  + rIr.best()) / 1e6;
    // JIT'te native derleme (warmup) derleme işidir — verime dahil edilir.
    if (useJit && !compileOnly) compileSec += rWarm.best() / 1e6;
    if (compileSec > 0) {
        double mbps = (totalBytes / 1024.0 / 1024.0) / compileSec;
        std::cout << "Compile throughput (best): "
                  << std::fixed << std::setprecision(2) << mbps << " MB/s\n";
    }

    // ── 1 Profil çalışması (trace etkin) ─────────────────────────────────────
    std::cerr << "[bench] Profiling run (tracing enabled, timings may differ)...\n";
    if (verbose) std::cerr << "[Profiling run]\n";
    BenchProfile profile;
    PipelineTimes profTimes;
    if (!runPipeline(fileSources, compileOnly, useJit, programArgs,
                     profTimes, &profile, verbose))
        return 1;

    // Profil raporunu yazdır
    printBenchProfile(
        profile,
        (uint64_t)profTimes.tokUs,
        (uint64_t)profTimes.parseUs,
        (uint64_t)profTimes.symUs,
        (uint64_t)profTimes.tcUs,
        (uint64_t)profTimes.irUs,
        (uint64_t)profTimes.vmUs,
        compileOnly
    );

    return 0;
}

#endif // SAQUT_CLI_BENCH
