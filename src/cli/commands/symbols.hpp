// ============================================================================
// saQut CLI — symbols komutu (sembol tablosu)
//
// Varsayılan: insan-okur düz metin (KORUNUR).
// #145 (SQ-100-SYMBOLS-JSONL): makine yüzeyi açıkça --jsonl ile seçilir.
//   İlk kayıt  : {"record":"symbols.header","schemaVersion":1}
//   Ara kayıt  : satır başına bir symbol veya diagnostic kaydı (deterministik)
//   Son kayıt  : {"record":"symbols.end","errors":N,"warnings":M,"symbolCount":K}
//   Eski `--json` tek-büyük-JSON preview KALDIRILDI: symbols `--json`'ı
//   kabul etmez, parseArgs usage error 64 verir ve geçerli seçenekleri sayar.
//   `--compact` JSONL'de anlamsız → sessizce yutulmaz (usage error 64).
//   Error-tolerant davranış ve nonzero exit korunur (65 = veri hatası).
// ============================================================================

#ifndef SAQUT_CLI_SYMBOLS
#define SAQUT_CLI_SYMBOLS

#include <iostream>
#include "tools.hpp"
#include "cli/args.hpp"
#include "cli/exit_codes.hpp"
#include "cli/jsonl_schema.hpp"
#include "tokenizer/tokenizer.hpp"
#include "parser/parser.hpp"
#include "symbol/symbol_table.hpp"
#include "symbol/symbol_collector.hpp"
#include "diagnostic/diagnostic_engine.hpp"
#include "vendor/nlohmann/json.hpp"

inline int cmdSymbols(const CliArgs& args) {
    std::string filePath = inputFilePath(args);
    std::string source   = readSource(args);
    if (source.empty()) return saqut::exit_code::kUsageError;

    // --compact JSONL'de anlamsız → sessizce yutulmaz.
    if (args.compact && args.jsonlOutput) {
        std::cerr << "error: --compact is meaningless with --jsonl\n";
        return saqut::exit_code::kUsageError;
    }

    // RG-7 (#157): #134/ast ile aynı sınıf düzeltme — Parser'a gerçek
    // DiagnosticEngine verilmezse syntax hatası panic-mode kurtarma ile
    // yutulur, `!ast` hiç true olmaz. Tokenizer da aynı motora raporlar.
    DiagnosticEngine diag;
    Tokenizer        tokenizer(&diag);
    auto tokens = tokenizer.scan(source, filePath);

    Parser           parser(&diag);
    ASTNode*         ast = parser.parse(tokens);

    SymbolTable table;
    if (ast) {
        SymbolCollector(table, diag).collect(ast);
    } else if (!diag.hasErrors()) {
        diag.report("E000", SourceLocation{}, "failed to build AST");
    }

    if (args.jsonlOutput) {
        // ── JSONL çıktı (SQ-100-SYMBOLS-JSONL) ─────────────────────────────
        std::cout << "{\"record\":\"symbols.header\",\"schemaVersion\":"
                  << saqut::jsonl::kSymbolsJsonlSchemaVersion << "}\n";

        int symbolCount = 0;
        for (Symbol* s : table.allSymbols()) {
            if (s->isBuiltin) continue;
            ++symbolCount;

            nlohmann::json refs = nlohmann::json::array();
            for (const SourceLocation& r : s->references)
                refs.push_back(r.toJsonObj());

            nlohmann::json rec;
            rec["record"]        = "symbols.symbol";
            rec["name"]          = s->name;
            rec["kind"]          = symbolKindName(s->kind);
            rec["type"]          = s->type.toString();
            rec["typeDetail"]    = s->type.toJsonObj();
            rec["sourceModule"]  = s->moduleId == 0 ? "__builtin__"
                                : s->moduleId < 0  ? "<main>"
                                : "<module:" + std::to_string(s->moduleId) + ">";
            rec["definition"]    = s->definitionLoc.toJsonObj();
            rec["referenceCount"] = static_cast<int>(s->references.size());
            rec["references"]    = refs;
            std::cout << rec.dump() << "\n";
        }

        // Tanılar ekleme sırasıyla — deterministik; exact konum taşır.
        for (const auto& d : diag.all()) {
            nlohmann::json rec;
            rec["record"]  = "symbols.diagnostic";
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

        std::cout << "{\"record\":\"symbols.end\",\"errors\":" << diag.errorCount()
                  << ",\"warnings\":" << diag.warningCount()
                  << ",\"symbolCount\":" << symbolCount << "}\n";
    } else {
        // ── Düz metin çıktı (varsayılan, KORUNUR) ───────────────────────────
        for (Symbol* s : table.allSymbols()) {
            if (s->isBuiltin) continue;

            // "<def_loc>  <tip>  <isim>" — fonksiyonlarda kind parantezde
            std::string defLoc = s->definitionLoc.isValid()
                ? s->definitionLoc.toString()
                : "<unknown>";
            std::string kindSuffix = (s->kind != SymbolKind::Variable &&
                                      s->kind != SymbolKind::Parameter)
                ? std::string(" (") + symbolKindName(s->kind) + ")"
                : "";
            std::cout << Color::SoftGri << defLoc << Color::Reset << "  "
                      << Color::SoftPembe << s->type.toString() << Color::Reset << "  "
                      << Color::SoftYesil << s->name << Color::Reset
                      << Color::SoftMor << kindSuffix << Color::Reset << "\n";

            if (!s->references.empty()) {
                std::cout << "\t" << Color::SoftTurkuaz << "refs" << Color::Reset;
                for (const SourceLocation& r : s->references)
                    std::cout << "  " << Color::SoftGri << r.toString() << Color::Reset;
                std::cout << "\n";
            }
        }
        if (diag.hasErrors() || diag.warningCount() > 0)
            diag.printAll(std::cerr);
    }

    delete ast;
    for (auto* t : tokens) delete t;
    return diag.hasErrors() ? saqut::exit_code::kDataError : saqut::exit_code::kSuccess;
}

inline constexpr CliCommand kSymbolsCommand{
    .name        = "symbols",
    .usage       = "saqut symbols <file> [--jsonl]",
    .description = "print symbol table (functions, variables)",
    .options     = OPT_JSONL | OPT_COMPACT,
    .run         = cmdSymbols,
};

#endif // SAQUT_CLI_SYMBOLS
