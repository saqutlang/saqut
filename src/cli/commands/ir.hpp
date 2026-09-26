// ============================================================================
// saQut CLI — ir komutu (IR instruction dump)
// ============================================================================

#ifndef SAQUT_CLI_IR
#define SAQUT_CLI_IR

#include <iostream>
#include "cli/args.hpp"
#include "cli/exit_codes.hpp"
#include "module/module_loader.hpp"
#include "symbol/symbol_table.hpp"
#include "symbol/symbol_collector.hpp"
#include "semantic/type_checker.hpp"
#include "semantic/structural_validator.hpp"
#include "diagnostic/diagnostic_engine.hpp"
#include "core/module_registry.hpp"
#include "core/config.hpp"
#include "opt/optimization_manager.hpp"
#include "ir/ir_generator.hpp"
#include "ir/ir_cfg.hpp"

inline int cmdIr(const CliArgs& args) {
    std::string filePath = inputFilePath(args);
    if (filePath.empty()) return saqut::exit_code::kUsageError;

    ModuleRegistry   registry;
    DiagnosticEngine diag;
    ModuleGraph      graph = ModuleLoader(registry, diag).load(filePath);

    SymbolTable symbolTable;
    SymbolCollector(symbolTable, diag).collectModuleGraph(graph);
    if (!diag.hasErrors()) {
        for (auto& unit : graph.units) TypeChecker(symbolTable, diag).check(unit.ast);
        for (auto& unit : graph.units) StructuralValidator(diag).validate(unit.ast);
    }

    if (diag.hasErrors()) {
        diag.printAll(std::cerr);
        return saqut::exit_code::kDataError;
    }

    if (args.optimize) {
        CompilerConfig   cfg;
        DiagnosticEngine optDiag;
        for (auto& unit : graph.units)
            OptimizationManager(cfg, optDiag).runPassesInPlace(unit.ast, &symbolTable);
        if (optDiag.errorCount() + optDiag.warningCount() > 0)
            optDiag.printAll(std::cerr);
    }

    IRGenerator irGenerator;
    IRProgram   program = irGenerator.generateModuleGraph(graph, symbolTable);

    // #218: --cfg — flat liste yerine CFG (BasicBlock + kenarlar) bas.
    // buildCFG gerçek implementasyondur; VM yine flat listeyi kullanır.
    // Dominatör/döngü analizi dump'ın parçası: gözlenebilirlik (cam kutu) —
    // analiz sonuçları yalnızca içte değil çıktıda da görünür.
    if (args.showCfg) {
        for (const std::string& name : program.functionOrder) {
            IRFunction& fn = program.functions.at(name);
            CFG cfg = buildCFG(fn.instructions);
            cfg.computeDominance();
            cfg.computeLoops();
            std::cout << IrColor::SoftYesil() << name << IrColor::Reset() << "\n";
            std::cout << cfg.dump();
        }
        return saqut::exit_code::kSuccess;
    }

    program.dump();

    return saqut::exit_code::kSuccess;
}

inline constexpr CliCommand kIrCommand{
    .name        = "ir",
    .usage       = "saqut ir <file> [--dont-optimize] [--cfg]",
    .description = "print IR instruction list (intermediate representation)",
    .options     = OPT_DONT_OPTIMIZE | OPT_CFG,
    .run         = cmdIr,
};

#endif // SAQUT_CLI_IR
