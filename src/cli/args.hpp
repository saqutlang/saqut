// ============================================================================
// saQut Compiler — CLI Argüman Ayrıştırıcı ve Kaynak Okuma
// ============================================================================
//
// DİZİN:   src/cli/args.hpp
// BAĞIMLI: Yok (sadece standart kütüphane)
//
// AMAÇ:
//   1. POSIX tarzı argüman ayrıştırma
//   2. Kaynak dosya okuma (tüm komutlar tarafından paylaşılır)
//
// DESTEKLENEN FORMATLAR:
//   saqut <komut> [dosya] [-o çıktı] [--help]
//   saqut run file:source.sqt           (eski sözdizimi)
//
// Tanınmayan bayrak, bozuk sayısal değer, fazladan/eksik konumsal argüman
// kullanım hatasıdır (64): eskiden hepsi sessizce yutuluyordu (#257) —
// `run a.sqt foo` argümanı düşürüyor, `--jitt` yok sayılıyor, argümansız
// `run` gizli bir `source.sqt` arıyordu.
//
// ============================================================================

#ifndef SAQUT_CLI_ARGS
#define SAQUT_CLI_ARGS

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "cli/exit_codes.hpp"
#include "core/config.hpp"

struct CliArgs {
    std::string command;
    std::vector<std::string> positional;
    std::string outputFile;
    bool showHelp    = false;
    bool stdinMode   = false;
    bool compact     = false;  // --compact: boşluksuz JSON
    // Optimizasyon VARSAYILAN OLARAK AÇIKTIR (sabit katlama + ölü kod eleme).
    // --dont-optimize kapatır. Gerekçe: production koşuları optimize edilmiş
    // derlemeyi kullanır; varsayılanın onunla aynı olması, geliştirmede
    // görülen davranışın dağıtılan davranış olmasını garantiler. (Optimizasyon
    // gözlenen çıktıyı değiştirmez — ADR-038; tests/run.sh "optimizasyon
    // sonucu degistirmiyor" gate'i bunu her fixture'da doğrular.)
    bool optimize    = true;   // --dont-optimize ile false olur
    bool jsonOutput  = false;  // --json: JSON çıktı üret (varsayılan: düz metin)
    bool jsonlOutput = false;  // --jsonl: canonical JSONL çıktı (SQ-100 ailesi, #145)
    bool showCfg     = false;  // --cfg: saqut ir — flat liste yerine CFG (BasicBlock + kenar) bas
    int  benchRuns   = 5;      // --runs=N: benchmark tekrar sayısı
    bool compileOnly = false;  // --compile-only: VM çalıştırmasını atla
    bool verbose     = false;  // --verbose: her aşamanın bitişini canlı yaz
    int  gcThreshold = 0;      // --gc-threshold=N: toplama eşiği BAYT (0 = varsayılan, negatif = toplama kapalı)
    bool gcStats     = false;  // --gc-stats: koşu sonunda GC istatistiklerini stderr'e yaz

    // #80/MIRPLAN.md: --jit — Dilim 0 kapsamındaki fonksiyonlar için MIR
    // JIT backend'ini dener (yalnızca LOAD_CONST/ADD/SUB/MUL/RETURN,
    // parametresiz main). Desteklenmeyen bir şey görülürse VM'e düşer.
    // 0.8.0'da opt-in; 1.0.0'da varsayılan yön döner (bkz. CLAUDE.md).
    bool useJit = false;

    // src/profiling/: --profile — token/parser/ir-gen/vm-veya-jit
    // aşamalarını ayrı ayrı ölçüp stderr'e yazdırır (saqut run).
    bool profile = false;

    // `--` sonrası argümanlar — sys::args() ile programa geçilir.
    std::vector<std::string> programArgs;

    // Boş değilse ayrıştırma bir kullanım hatası buldu; main 64 ile çıkar.
    std::string usageError;
};

// Sayısal bayrak değeri: tamamı tamsayı değilse false (`--runs=abc`, `--runs=5x`).
inline bool parseIntFlag(const std::string& text, int& out) {
    if (text.empty()) return false;
    try {
        size_t used = 0;
        int v = std::stoi(text, &used);
        if (used != text.size()) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

inline bool isKnownCommand(const std::string& s) {
    return s == "run" || s == "tokens" || s == "ast" || s == "symbols" || s == "check" ||
           s == "ir" || s == "exec" || s == "lsp" || s == "dap" || s == "bench" || s == "help";
}

// ============================================================================
// parseArgs
// ============================================================================
inline CliArgs parseArgs(int argc, char* argv[]) {
    CliArgs args;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        // `--` sonrası her şey programArgs — sys::args() (#90).
        if (arg == "--") {
            for (int j = i + 1; j < argc; j++)
                args.programArgs.push_back(argv[j]);
            break;
        }
        if (arg == "-") {
            // Kaynaktan okuma yalnız dosya yoluyla: modül çözümlemesi
            // import yollarını dosyanın dizinine göre çözer (bkz. modules).
            args.usageError = "reading the program from standard input is not supported; "
                              "pass a file path";
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            args.showHelp = true;
            continue;
        }
        if (arg.compare(0, 9, "--output=") == 0) {
            args.outputFile = arg.substr(9);
            continue;
        }
        if (arg.compare(0, 9, "--format=") == 0 || arg == "--format") {
            std::cerr << "error: --format option was removed; it was not consumed by any command\n";
            exit(saqut::exit_code::kUsageError);
        }
        if (arg == "--output" || arg == "-o") {
            if (i + 1 < argc) args.outputFile = argv[++i];
            continue;
        }
        if (arg == "--compact") {
            args.compact = true;
            continue;
        }
        if (arg == "--json") {
            args.jsonOutput = true;
            continue;
        }
        if (arg == "--jsonl") {
            args.jsonlOutput = true;
            continue;
        }
        if (arg == "--cfg") {
            args.showCfg = true;
            continue;
        }
        if (arg == "--dont-optimize") {
            args.optimize = false;
            continue;
        }
        // --optimized artık varsayılan davranıştır; bayrak no-op olarak
        // kabul edilir ki mevcut betikler/komut geçmişi kırılmasın.
        if (arg == "--optimized") {
            continue;
        }
        if (arg == "--compile-only") {
            args.compileOnly = true;
            continue;
        }
        if (arg == "--verbose" || arg == "-v") {
            args.verbose = true;
            continue;
        }
        if (arg == "--version" || arg == "-V") {
            std::cout << "saQut " << SAQUT_VERSION << "\n";
            exit(0);
        }
        if (arg.compare(0, 7, "--runs=") == 0) {
            if (!parseIntFlag(arg.substr(7), args.benchRuns) || args.benchRuns < 1)
                args.usageError = "invalid value for --runs: '" + arg.substr(7) +
                                  "' (expected a positive integer)";
            continue;
        }
        if (arg.compare(0, 17, "--max-call-depth=") == 0) {
            int v = 0;
            if (!parseIntFlag(arg.substr(17), v) || v < 1)
                args.usageError = "invalid value for --max-call-depth: '" + arg.substr(17) +
                                  "' (expected a positive integer)";
            else
                gMaxCallDepth = v;
            continue;
        }
        if (arg.compare(0, 15, "--gc-threshold=") == 0) {
            if (!parseIntFlag(arg.substr(15), args.gcThreshold))
                args.usageError = "invalid value for --gc-threshold: '" + arg.substr(15) +
                                  "' (expected an integer)";
            continue;
        }
        if (arg == "--gc-stats") {
            args.gcStats = true;
            continue;
        }
        if (arg == "--jit") {
            args.useJit = true;
            continue;
        }
        if (arg == "--profile") {
            args.profile = true;
            continue;
        }
        if (arg.compare(0, 5, "file:") == 0) {
            args.positional.push_back(arg.substr(5));
            continue;
        }
        if (arg.compare(0, 7, "output:") == 0) {
            args.outputFile = arg.substr(7);
            continue;
        }
        if (arg.compare(0, 4, "ast:") == 0) {
            args.outputFile = arg.substr(4);
            continue;
        }

        // LSP/DAP istemcilerinin yaygın taşıma bayrağı — stdio zaten tek yol.
        if (arg == "--stdio") {
            continue;
        }
        // ADR-043: capability sistemi 0.9.4'te kaldırıldı; eski betikler
        // `--allow*` / `--capabilities` geçebilir — neden reddedildiğini söyle.
        if (arg.compare(0, 7, "--allow") == 0 || arg == "--capabilities") {
            if (args.usageError.empty())
                args.usageError = "option '" + arg + "' was removed: the capability system no "
                                  "longer exists (ADR-043); host calls are open by default";
            continue;
        }
        // Tanınmayan bayrak: sessizce konumsal argüman ya da modül adı
        // sayılmaz (eskiden `--gc-treshold=5` "cannot open module" veriyordu).
        if (arg.size() > 1 && arg[0] == '-') {
            if (args.usageError.empty())
                args.usageError = "unknown option '" + arg + "'";
            continue;
        }

        // İlk argüman komut mu? Değilse ve bir dosyaya benziyorsa `run`
        // kısayolu (`saqut prog.sqt`); ikisi de değilse bilinmeyen komut —
        // eskiden `saqut compile x.sqt` "cannot open module 'compile'" diyordu.
        if (args.command.empty() && i == 1) {
            if (isKnownCommand(arg)) {
                args.command = arg;
                continue;
            }
            std::ifstream probe(arg);
            const bool looksLikeFile = probe.good() ||
                (arg.size() > 4 && arg.compare(arg.size() - 4, 4, ".sqt") == 0);
            if (!looksLikeFile) {
                args.usageError = "unknown command '" + arg + "'";
                continue;
            }
            args.command = "run";
            args.positional.push_back(arg);
            continue;
        }

        args.positional.push_back(arg);
    }

    if (args.command.empty()) args.command = "run";

    // Komut başına konumsal argüman sayısı. Fazlası sessizce düşmez: program
    // argümanı olmaları muhtemeldir ve `--` sonrasına gitmeleri gerekir.
    if (args.usageError.empty() && !args.showHelp && args.command != "help") {
        const bool noPositional = args.command == "lsp" || args.command == "dap";
        const size_t maxPositional = noPositional ? 0 : 1;
        if (args.positional.size() > maxPositional) {
            const std::string& extra = args.positional[maxPositional];
            args.usageError = "unexpected argument '" + extra + "'";
            if (args.command == "run" || args.command == "exec" || args.command == "bench")
                args.usageError += " (program arguments go after '--': saqut " + args.command +
                                   " <file> -- " + extra + ")";
        } else if (!noPositional && args.positional.empty()) {
            args.usageError = args.command == "exec" ? "no expression given (saqut exec \"1 + 2\")"
                                                     : "no input file";
        }
    }

    return args;
}

// ============================================================================
// readSource: Dosyadan veya stdin'den kaynak kod oku (TODO: stdin)
// ============================================================================
inline std::string readSource(const CliArgs& args) {
    if (args.stdinMode) {
        // TODO: read from std::cin until EOF
        std::cerr << "TODO: stdin mode not yet supported\n";
        return "";
    }
    if (args.positional.empty()) return "";

    std::string path = args.positional[0];
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "error: cannot open file '" << path << "'\n";
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// ============================================================================
// inputFilePath: Kaynak dosyanın yolunu döndür (stdin modunda boş string)
// ============================================================================
inline std::string inputFilePath(const CliArgs& args) {
    if (args.stdinMode || args.positional.empty()) return "";
    return args.positional[0];
}

#endif // SAQUT_CLI_ARGS
