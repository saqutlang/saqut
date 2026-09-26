// ============================================================================
// saQut Compiler — Giriş Noktası (main)
// ============================================================================
//
// DİZİN:   src/main.cpp
// KATMAN:  En üst — argümanları ayrıştırır, komutu çalıştırır
//
// Komutlar cli/command_list.hpp'de listelenir; yeni komut eklemek için o
// dosyanın başlığındaki adımlara bak. Bu dosyaya dokunmak gerekmez.
//
// ============================================================================

#include <iostream>
#include "cli/args.hpp"
#include "cli/cli.hpp"
#include "cli/command_list.hpp"
#include "cli/exit_codes.hpp"
#include "runtime/isolate.hpp"

int main(int argc, char* argv[]) {
    // ADR-045: ana thread isolate'i (thread id 1). Süreç ömrü boyunca bağlı
    // kalır; Isolate::current() artık lazy değildir (c2).
    Isolate::currentOrCreate();

    const std::vector<const CliCommand*>& commands = allCommands();

    // Argümansız çağrı → yardım
    if (argc <= 1) {
        printHelp(commands);
        return saqut::exit_code::kSuccess;
    }

    CliArgs args = parseArgs(argc, argv, commands);

    // #257: kullanım hataları sessizce yutulmaz.
    if (!args.usageError.empty()) {
        std::cerr << "error: " << args.usageError << "\n";
        std::cerr << "for usage: saqut --help\n";
        return saqut::exit_code::kUsageError;
    }

    return dispatch(args, commands);
}
