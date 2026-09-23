// ============================================================================
// saQut CLI — run komutu
//
// Pipeline: ModuleLoader → 3-geçiş SymbolCollect → TypeCheck → Opt → IRGen → VM
// ============================================================================

#ifndef SAQUT_CLI_RUN
#define SAQUT_CLI_RUN

#include <iostream>
#include "cli/args.hpp"
#include "cli/exit_codes.hpp"
#include "module/module_loader.hpp"
#include "symbol/symbol_table.hpp"
#include "symbol/symbol_collector.hpp"
#include "semantic/type_checker.hpp"
#include "semantic/structural_validator.hpp"
#include "diagnostic/diagnostic_engine.hpp"
#include "core/config.hpp"
#include "core/module_registry.hpp"
#include "opt/optimization_manager.hpp"
#include "ir/ir_generator.hpp"
#include "vm/interpreter.hpp"
#include "mir/mir_backend.hpp"
#include "profiling/stage_timer.hpp"

inline int cmdRun(const CliArgs& args) {
    std::string filePath = inputFilePath(args);
    if (filePath.empty()) { std::cerr << "error: no input file\n"; return saqut::exit_code::kUsageError; }

    // src/profiling/ (--profile): args.profile false ise timer kullanılmaz,
    // ScopedStage'ler no-op kalır (StageTimer::ScopedStage tasarımı gereği).
    profiling::StageTimer  stageTimer;
    profiling::StageTimer* profilerPtr = args.profile ? &stageTimer : nullptr;

    // ── Aşama 1: Tüm modülleri yükle (BFS parse) ─────────────────────────
    ModuleRegistry   registry;
    DiagnosticEngine diag;
    ModuleLoader     loader(registry, diag);
    loader.setProfiler(profilerPtr);
    // --profile: "parser" node sayısı = load() boyunca kurulan AST düğümü
    // delta'sı (ayrı ağaç gezme yok; ASTNode yapıcısı sayıyor). "token"/
    // "parser" SÜRELERİ ModuleLoader içinde ScopedStage ile ölçülüyor.
    long long nodesBefore = ASTNode::s_constructedCount;
    ModuleGraph      graph = loader.load(filePath);
    if (profilerPtr)
        profilerPtr->count("parser", ASTNode::s_constructedCount - nodesBefore, "node");

    if (diag.hasErrors()) {
        diag.printAll(std::cerr);
        return saqut::exit_code::kDataError;
    }

    // ── Aşama 2: 3-geçiş sembol toplama + import doğrulama ───────────────
    SymbolTable symbolTable;
    SymbolCollector collector(symbolTable, diag);
    collector.collectModuleGraph(graph);

    if (diag.hasErrors()) {
        std::cerr << "compilation errors, cannot run program:\n";
        diag.printAll(std::cerr);
        return saqut::exit_code::kDataError;
    }

    // ── Aşama 3: Tip denetimi + yapısal doğrulama ─────────────────────────
    for (auto& unit : graph.units)
        TypeChecker(symbolTable, diag).check(unit.ast);
    for (auto& unit : graph.units)
        StructuralValidator(diag).validate(unit.ast);

    if (diag.hasErrors()) {
        std::cerr << "compilation errors, cannot run program:\n";
        diag.printAll(std::cerr);
        return saqut::exit_code::kDataError;
    }

    // ── Aşama 4 (opsiyonel): Optimizasyon ────────────────────────────────
    if (args.optimize) {
        profiling::StageTimer::ScopedStage _prof(profilerPtr, "optimize");
        CompilerConfig   cfg;
        DiagnosticEngine optDiag;
        // --profile: "geçiş" = fixpoint tur sayısı (her modül için ayrı ayrı
        // toplanır); runPassesInPlace zaten bu sayacı tutuyor (byproduct).
        long long totalRounds = 0;
        for (auto& unit : graph.units)
            totalRounds += OptimizationManager(cfg, optDiag)
                               .runPassesInPlace(unit.ast, &symbolTable);
        if (profilerPtr) profilerPtr->count("optimize", totalRounds, "round");
        if (optDiag.errorCount() + optDiag.warningCount() > 0)
            optDiag.printAll(std::cerr);
    }

    // ── Aşama 5: IR üretimi ───────────────────────────────────────────────
    IRGenerator irGenerator;
    IRProgram   program;
    {
        profiling::StageTimer::ScopedStage _prof(profilerPtr, "ir-gen");
        program = irGenerator.generateModuleGraph(graph, symbolTable);
    }
    // --profile: "instr" = üretilen toplam IR talimatı (tüm fonksiyonlar).
    if (profilerPtr) {
        long long totalInstr = 0;
        for (auto& [name, fn] : program.functions)
            totalInstr += static_cast<long long>(fn.instructions.size());
        profilerPtr->count("ir-gen", totalInstr, "instr");
    }

    // ── Aşama 6: Çalıştırma backend'i ────────────────────────────────────
    // #80/MIRPLAN.md: --jit istenirse KISMİ/sessiz VM'e düşme YOK —
    // kullanıcı talimatı: "JIT diyorsam baştan sona JIT derlemesi
    // gerekiyor". Program.functions'daki HER fonksiyon Dilim 1'in
    // desteklediği opcode kümesinde değilse, HİÇBİR ŞEY çalıştırılmadan
    // açık bir hatayla çıkılır — VM devreye asla girmez.
    //
    // #229 minor: warning'ler run modunda da GÖRÜNMELİ (stderr), ama
    // programı BLOKLAMAMALI. Hata yokken warning varsa stderr'e basılır;
    // error varsa üstteki hasErrors blokları zaten printAll ile hepsini
    // (error+warning) basmıştır. stdout'a dokunulmaz — golden etkilenmez.
    if (diag.warningCount() > 0 && !diag.hasErrors())
        diag.printAll(std::cerr);
    if (args.useJit) {
        // --gc-threshold iki backend'de de geçerlidir: aynı bayrak, aynı anlam.
        if (args.gcThreshold != 0)
            mir_backend::setGcThresholdForNextRun(args.gcThreshold);
        int                             jitResult = 0;
        mir_backend::UnsupportedReason  reason;
        // profilerPtr dogrudan iceri gecirilir — "jit-warmup" (IR->MIR ceviri
        // + native derleme) ve "jit-exec" (yalnizca calistirma) mir_backend
        // TARAFINDAN ayri ayri raporlanir, burada tek bir "vm/jit" ile
        // sarilmiyor (kullanici talimati: bu ikisi karistirilmasin).
        bool jitOk = mir_backend::tryCompileAndRunProgram(
            program, jitResult, reason, args.programArgs, profilerPtr);
        if (jitOk) {
            if (args.verbose) std::cerr << "[jit] whole program ran on the JIT (VM not used)\n";
            // --gc-stats: VM ve JIT AYNI formatta raporlar — iki backend aynı
            // GC çekirdeğini kullandığı için sayaçlar karşılaştırılabilirdir.
            if (args.gcStats) printGcStats(std::cerr, mir_backend::lastRunGcStats());
            if (args.profile) stageTimer.printReport(std::cerr);
            return jitResult;
        }
        std::cerr << "error: --jit cannot compile this program completely (function '"
                  << reason.functionName << "', unsupported opcode: " << reason.opcodeName
                  << "); not falling back to the VM silently. Run without --jit.\n";
        // Kullanıcı --jit'i doğru kullandı (kUsageError değil); programın
        // kendisi de tanı hatası vermedi (kDataError değil) — derleyicinin
        // JIT backend'i istenen işi tam yapamadı. "runtime/compiler çalışma
        // hatası" sınıfı (70).
        return saqut::exit_code::kSoftwareError;
    }

    // ── Aşama 6b: VM çalıştır (varsayılan yol) ───────────────────────────
    int exitCode = 0;
    try {
        Interpreter vm(program);
        // --gc-threshold=N: toplama eşiğini (bayt) ezer; negatif = toplama kapalı
        if (args.gcThreshold != 0) vm.setGCThreshold(args.gcThreshold);
        vm.setProgramArgs(args.programArgs);
        // "vm-warmup" (initForDebug — frame/global kurulumu) ve "vm-exec"
        // (runUntilEvent'in ana döngüsü) Interpreter TARAFINDAN ayrı ayrı
        // raporlanır — burada sarmalamaya gerek yok.
        vm.setStageProfiler(profilerPtr);
        exitCode = vm.run();
        if (args.gcStats) printGcStats(std::cerr, vm.gcStats());
    } catch (const std::exception& e) {
        std::cerr << "runtime error: " << e.what() << "\n";
        exitCode = saqut::exit_code::kSoftwareError;
    }

    if (args.profile) stageTimer.printReport(std::cerr);

    return exitCode;
}

#endif // SAQUT_CLI_RUN
