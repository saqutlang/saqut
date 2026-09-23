// ============================================================================
// saQut Compiler — Giriş Noktası (main)
// ============================================================================
//
// DİZİN:   src/main.cpp
// KATMAN:  En üst — CLI dispatcher'ı başlatır
//
// KULLANIM:
//   saqut                         → yardım
//   saqut run <dosya>             → pipeline debug çıktısı
//   saqut tokens <dosya>          → token listesi
//   saqut ast <dosya> [-o çıktı]  → JSON AST + analiz
//   saqut symbols <dosya>         → sembol tablosu
//   saqut -                       → stdin modu (TODO)
//
// YENİ KOMUT EKLEMEK İÇİN:
//   1. src/cli/commands/x.hpp oluştur
//   2. Bu dosyada #include et
//   3. cli.registerCommand({...}) ile kaydet
//
// ============================================================================

#include <iostream>
#include "cli/args.hpp"
#include "cli/cli.hpp"
#include "cli/commands/run.hpp"
#include "cli/commands/tokens.hpp"
#include "cli/commands/ast.hpp"
#include "cli/commands/symbols.hpp"
#include "cli/commands/check.hpp"
#include "cli/commands/ir.hpp"
#include "cli/commands/exec.hpp"
#include "cli/commands/lsp.hpp"
#include "cli/commands/dap.hpp"
#include "cli/commands/bench.hpp"

int main(int argc, char* argv[]) {
    // Komutları kaydet
    CliDispatcher cli;

    cli.registerCommand({"run",
        "run program (token → AST → IR → VM)",
        false, cmdRun});

    cli.registerCommand({"tokens",
        "print token list",
        false, cmdTokens});

    cli.registerCommand({"ast",
        "print AST hierarchy and analysis as JSON",
        false, cmdAst});

    cli.registerCommand({"symbols",
        "print symbol table (functions, variables)",
        false, cmdSymbols});

    cli.registerCommand({"check",
        "semantic analysis — type checking + structural validation",
        false, cmdCheck});

    cli.registerCommand({"ir",
        "print IR instruction list (intermediate representation)",
        false, cmdIr});

    cli.registerCommand({"exec",
        "evaluate an expression and print the result  (saqut exec \"1+2\")",
        false, cmdExec});

    cli.registerCommand({"lsp",
        "start LSP server (JSON-RPC on stdin/stdout)",
        false, cmdLsp});

    cli.registerCommand({"dap",
        "start DAP debug adapter (JSON-RPC on stdin/stdout)",
        false, cmdDap});

    cli.registerCommand({"bench",
        "phase-level benchmark (tokenize|parse|symbol|typecheck|ir|vm)",
        false, cmdBench});

    // Argümanları ayrıştır
    CliArgs args = parseArgs(argc, argv);

    // Argümansız çağrı → help
    if (argc <= 1) {
        cli.printHelp();
        return 0;
    }

    // #257: kullanım hataları sessizce yutulmaz.
    if (!args.usageError.empty()) {
        std::cerr << "error: " << args.usageError << "\n";
        std::cerr << "for usage: saqut --help\n";
        return saqut::exit_code::kUsageError;
    }

    return cli.dispatch(args);
}
