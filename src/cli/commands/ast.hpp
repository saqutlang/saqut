// ============================================================================
// saQut CLI — ast komutu
//
// KATMAN: CLI komutu. Tüm kaynak modül grafiğini yükler, sembol/tip geçişlerini
// çalıştırır (AST'yi anotlar) ve HER modülün AST'sini döker.
//
// BORU HATTI (run ile ortak önek; kuyruk farklı):
//   ModuleLoader → collectModuleGraph → (birim başına TypeChecker/StructuralValidator)
//   → [--optimize] → döküm
// run'da bu noktadan sonra IRGen + VM vardır; ast sonunda döküm yapar.
//
// TANI AKIŞI YOKTUR: geçişlerin ürettiği tanılar kullanıcıya gösterilmez ve
// çıktıyı ya da çıkış kodunu etkilemez. Hatalı kodda bile kısmi AST basılır
// ("hatalı kod → hatalı AST"). stdin modu desteklenmez; dosya yolu zorunludur.
//
// --optimize verilirse geçişler grafiğin kendi AST'leri üzerinde YERİNDE
// çalışır (klon yok). ast yalnız optimize edilmiş AST'yi bastığı ve orijinali
// saklamadığı için klon gereksizdir; ayrıca klonlama yolundaki deepClone'un
// işlemediği düğüm tipleri orijinali paylaştığından çift-serbest riski taşır.
// Optimizasyon tanıları da (diğerleri gibi) gösterilmez.
// ============================================================================

#ifndef SAQUT_CLI_AST
#define SAQUT_CLI_AST

#include <iostream>
#include <fstream>
#include <vector>
#include "cli/args.hpp"
#include "cli/exit_codes.hpp"
#include "module/module_loader.hpp"
#include "core/module_registry.hpp"
#include "core/config.hpp"
#include "symbol/symbol_table.hpp"
#include "symbol/symbol_collector.hpp"
#include "semantic/type_checker.hpp"
#include "semantic/structural_validator.hpp"
#include "diagnostic/diagnostic_engine.hpp"
#include "opt/optimization_manager.hpp"
#include "json.hpp"

inline int cmdAst(const CliArgs& args) {
    std::string filePath = inputFilePath(args);
    if (filePath.empty()) {
        std::cerr << "error: no input file\n";
        return saqut::exit_code::kUsageError;
    }

    // ── Aşama 1: tüm modülleri yükle (BFS parse) ─────────────────────────────
    ModuleRegistry   registry;
    DiagnosticEngine diag;                       // tanılar birikir; hiçbiri basılmaz
    ModuleGraph      graph = ModuleLoader(registry, diag).load(filePath);

    if (graph.units.empty()) {
        // Girdi okunamadı/çözülemedi — gösterilecek AST yok.
        // Tanı akışı olmadığı için hiçbir mesaj basılmaz.
        return saqut::exit_code::kDataError;
    }

    // ── Aşama 2: sembol toplama + import/FFI çözümü (run/check/ir ile aynı yol) ──
    SymbolTable symbolTable;
    SymbolCollector(symbolTable, diag).collectModuleGraph(graph);

    // ── Aşama 3: tip denetimi + yapısal doğrulama ────────────────────────────
    // AST'yi anotlar (resolvedType doldurulur → JSON'da görünür).
    // Tanılar YOK SAYILIR: ne basılır ne de akışı keser.
    for (auto& unit : graph.units)
        TypeChecker(symbolTable, diag).check(unit.ast);
    for (auto& unit : graph.units)
        StructuralValidator(diag).validate(unit.ast);

    // ── Gösterilecek AST'ler ─────────────────────────────────────────────────
    // Sahiplik grafikte kalır; ast hiçbir AST düğümünü serbest bırakmaz.
    // --optimize: geçişler yerinde çalışır (klon yok — bkz. dosya başlığı).
    if (args.optimize) {
        CompilerConfig      cfg;
        OptimizationManager mgr(cfg, diag);
        for (auto& unit : graph.units)
            mgr.runPassesInPlace(unit.ast, &symbolTable);
    }

    std::vector<ASTNode*> display;
    display.reserve(graph.units.size());
    for (auto& unit : graph.units)
        display.push_back(unit.ast);

    std::ostream* out = &std::cout;
    std::ofstream outFile;
    if (!args.outputFile.empty()) {
        outFile.open(args.outputFile);
        if (outFile.is_open()) out = &outFile;
    }

    // ── Döküm ────────────────────────────────────────────────────────────────
    if (args.jsonOutput) {
        *out << "{\n  \"units\": [\n";
        for (size_t i = 0; i < display.size(); ++i) {
            AstAnalysis analysis = analyzeAst(display[i]);
            *out << "    {\n"
                 << "      \"file\": \"" << jsonEscape(graph.units[i].filePath) << "\",\n"
                 << "      \"moduleId\": " << graph.units[i].moduleId << ",\n"
                 << "      \"ast\":\n"
                 << jsonIndent(3) << astToJson(display[i], 3) << ",\n"
                 << "      \"analysis\": {\n"
                 << analysisToJson(analysis) << "\n"
                 << "      }\n"
                 << "    }" << (i + 1 < display.size() ? "," : "") << "\n";
        }
        *out << "  ]\n}\n";
    } else {
        for (size_t i = 0; i < display.size(); ++i) {
            *out << "// ==== " << graph.units[i].filePath << " ====\n";
            display[i]->log(0);
        }
    }

    return saqut::exit_code::kSuccess;
}

#endif // SAQUT_CLI_AST
