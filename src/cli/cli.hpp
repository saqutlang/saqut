// ============================================================================
// saQut Compiler — CLI Dispatcher
// ============================================================================
//
// DİZİN:   src/cli/cli.hpp
// BAĞIMLI: args.hpp, commands/*
//
// AMAÇ:
//   Komut kaydı ve dağıtımı. Yeni bir komut eklemek için:
//   1. src/cli/commands/x.hpp oluştur
//   2. registerCommand() ile kaydet
//
// MİMARİ:
//   Her komut bir CliCommand struct'ıdır:
//     - name:        "run", "tokens", "ast", ...
//     - description: Yardım metninde görünür
//     - hidden:      true ise yardımda listelenmez (alias'lar için)
//     - execute:     int(CliArgs&) döndürür (0 = başarılı)
//
//   Komutlar lazy olarak include edilmez — her biri kendi header'ında
//   inline fonksiyon olarak tanımlanır ve cli.hpp tarafından include edilir.
//
// ============================================================================

#ifndef SAQUT_CLI
#define SAQUT_CLI

#include <cstddef>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include "cli/args.hpp"

// ============================================================================
// CliCommand — Kayıtlı bir komut
// ============================================================================
struct CliCommand {
    std::string name;
    std::string description;
    bool hidden = false;  // true = yardımda gösterme
    std::function<int(const CliArgs&)> execute;
};

// ============================================================================
// CliDispatcher — Komut kaydı ve çalıştırma
// ============================================================================
class CliDispatcher {
public:
    void registerCommand(const CliCommand& cmd) {
        commands.push_back(cmd);
    }

    int dispatch(const CliArgs& args) const {
        // Yardım özel durumu
        if (args.showHelp || args.command == "help") {
            printHelp();
            return 0;
        }

        // Komutu bul
        for (auto& cmd : commands) {
            if (cmd.name == args.command) {
                return cmd.execute(args);
            }
        }

        // Unknown command
        std::cerr << "error: unknown command '" << args.command << "'\n";
        std::cerr << "for available commands: saqut --help\n";
        return saqut::exit_code::kUsageError;
    }

    void printHelp() const {
        std::cout << "saQut " << SAQUT_VERSION << "\n\n";
        std::cout << "Usage: saqut [options] <command> [arguments]\n\n";
        std::cout << "Global options:\n";
        std::cout << "  -h, --help                 Display this help message\n";
        std::cout << "  -V, --version              Display the compiler version\n";
        std::cout << "\nCommands:\n";

        for (auto& cmd : commands) {
            if (cmd.hidden) continue;
            const std::string usage = commandUsage(cmd.name);
            std::cout << "  " << usage;
            if (usage.size() < kUsageColumn)
                std::cout << std::string(kUsageColumn - usage.size(), ' ');
            else
                std::cout << " ";
            std::cout << cmd.description << "\n";
        }

        // Bayraklar komuta özgüdür — yalnız onları tüketen komutta geçerlidir.
        std::cout << "\nCommand options:\n";
        std::cout << "      --jit                  Use the MIR JIT backend                      (run, exec, bench)\n";
        std::cout << "      --dont-optimize        Disable constant folding + dead code elim.   (run, ast, ir)\n";
        std::cout << "      --verbose              Print stage progress                         (run, bench)\n";
        std::cout << "      --profile              Report per-stage timings                     (run)\n";
        std::cout << "      --gc-stats             Print GC statistics on exit                  (run)\n";
        std::cout << "      --max-call-depth=N     Maximum call depth (default 100000)          (run, exec, bench)\n";
        std::cout << "      --gc-threshold=N       GC threshold in bytes; 0 = default,          (run)\n";
        std::cout << "                             negative disables collection\n";
        std::cout << "      --runs=N               Benchmark iterations (default 5)             (bench)\n";
        std::cout << "      --compile-only         Skip execution, compile only                 (bench)\n";
        std::cout << "      --json                 JSON output                                  (ast)\n";
        std::cout << "      --jsonl                Canonical JSONL output                       (symbols)\n";
        std::cout << "      --compact              Whitespace-free JSON                         (symbols)\n";
        std::cout << "      --cfg                  Print CFG instead of a flat list             (ir)\n";
        std::cout << "  -o, --output=<file>        Write output to a file (with --json)         (ast)\n";
        std::cout << "      --                     Pass remaining args to the program           (run, exec, bench)\n";
        std::cout << "                             via sys::args()\n";

        std::cout << "\nNotes:\n";
        std::cout << "  check always emits JSONL; it takes no output flags.\n";
        std::cout << "  Optimization (constant folding + dead code elimination) is ON by\n";
        std::cout << "  default; --dont-optimize turns it off. Optimization never changes a\n";
        std::cout << "  program's output, only how fast it gets there.\n";

        std::cout << "\nUse 'saqut --help' to display this message again.\n";
    }

private:
    // Komut kullanım sütunu — en uzun usage satırını barındıracak genişlik.
    static constexpr std::size_t kUsageColumn = 55;

    static std::string commandUsage(const std::string& command) {
        if (command == "run")     return "saqut run <file> [options] [-- args]";
        if (command == "tokens")  return "saqut tokens <file>";
        if (command == "ast")     return "saqut ast <file> [--json] [--dont-optimize] [-o <f>]";
        if (command == "symbols") return "saqut symbols <file> [--jsonl]";
        if (command == "check")   return "saqut check <file>";
        if (command == "ir")      return "saqut ir <file> [--dont-optimize] [--cfg]";
        if (command == "exec")    return "saqut exec \"<expression>\" [--jit] [-- args]";
        if (command == "lsp")     return "saqut lsp";
        if (command == "dap")     return "saqut dap";
        if (command == "bench")   return "saqut bench <file> [--runs=N] [--jit] [options]";
        return "saqut " + command;
    }

    std::vector<CliCommand> commands;
};

#endif // SAQUT_CLI
