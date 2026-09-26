// ============================================================================
// saQut CLI — check komutu
//
// Yalnızca semantik analiz: ModuleLoader → SymbolCollector → TypeChecker
//
// #144 (SQ-100-CHECK-JSONL): stdout canonical JSONL'dir.
//   İlk kayıt  : {"record":"check.header","schemaVersion":1}
//   Ara kayıt  : sıfır veya daha çok check.diagnostic (ekleme sırası, deterministik)
//   Son kayıt  : {"record":"check.end","errors":N,"warnings":M}
//   check.end yoksa stream eksiktir (tool kullanıcısı bunu incomplete sayar).
//   Her diagnostic file + exact konum (line/column/offset) taşır.
//   Telemetry → stderr (check şu an telemetry üretmez; stdout'ta yalnız JSONL).
//   exit: 0 = temiz, 65 = veri hatası (kDataError).
//   Eski tek-büyük-JSON formatı KORUNMAZ (şema geçişi bilinçlidir).
// ============================================================================

#ifndef SAQUT_CLI_CHECK
#define SAQUT_CLI_CHECK

#include <iostream>
#include "cli/args.hpp"
#include "cli/exit_codes.hpp"
#include "cli/jsonl_schema.hpp"
#include "module/module_loader.hpp"
#include "symbol/symbol_table.hpp"
#include "symbol/symbol_collector.hpp"
#include "semantic/type_checker.hpp"
#include "semantic/structural_validator.hpp"
#include "diagnostic/diagnostic_engine.hpp"
#include "core/module_registry.hpp"
#include "vendor/nlohmann/json.hpp"

inline int cmdCheck(const CliArgs& args) {
    std::string filePath = inputFilePath(args);
    if (filePath.empty()) return saqut::exit_code::kUsageError;

    ModuleRegistry   registry;
    DiagnosticEngine diag;
    ModuleGraph      graph = ModuleLoader(registry, diag).load(filePath);

    if (!diag.hasErrors()) {
        SymbolTable table;
        SymbolCollector(table, diag).collectModuleGraph(graph);

        if (!diag.hasErrors()) {
            for (auto& unit : graph.units) TypeChecker(table, diag).check(unit.ast);
            for (auto& unit : graph.units) StructuralValidator(diag).validate(unit.ast);
        }
    }

    // ── JSONL çıktı (SQ-100-CHECK-JSONL) ─────────────────────────────────
    std::cout << "{\"record\":\"check.header\",\"schemaVersion\":"
              << saqut::jsonl::kCheckJsonlSchemaVersion << "}\n";

    // Tanılar ekleme sırasıyla — deterministik (ADR-013: topla, sırayı koru).
    for (const auto& d : diag.all()) {
        nlohmann::json rec;
        rec["record"]  = "check.diagnostic";
        rec["level"]   = diagLevelName(d.level);
        rec["code"]    = d.code;
        rec["file"]    = d.loc.filePath();
        rec["line"]    = d.loc.line;
        rec["column"]  = d.loc.column;
        rec["offset"]  = d.loc.offset;
        rec["message"] = d.message;
        if (!d.hint.empty()) rec["hint"] = d.hint;
        std::cout << rec.dump() << "\n";
    }

    std::cout << "{\"record\":\"check.end\",\"errors\":" << diag.errorCount()
              << ",\"warnings\":" << diag.warningCount() << "}\n";

    return diag.hasErrors() ? saqut::exit_code::kDataError : saqut::exit_code::kSuccess;
}

inline constexpr CliCommand kCheckCommand{
    .name        = "check",
    .usage       = "saqut check <file>",
    .description = "semantic analysis — type checking + structural validation",
    .run         = cmdCheck,
};

#endif // SAQUT_CLI_CHECK
