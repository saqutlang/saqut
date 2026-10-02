// ============================================================================
// saQut Compiler — CLI Dispatcher
// ============================================================================
//
// DİZİN:   src/cli/cli.hpp
// BAĞIMLI: cli/args.hpp
//
// Yardım metni ve komut dağıtımı. İkisi de komut listesinden
// (cli/command_list.hpp) ve seçenek tablosundan (cliOptions) türetilir:
// kullanım satırı, açıklama ve "bu seçenek hangi komutlarda geçerli"
// parantezi elle yazılmaz.
//
// ============================================================================

#ifndef SAQUT_CLI
#define SAQUT_CLI

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>
#include "cli/args.hpp"
#include "cli/exit_codes.hpp"

// Metni `width` sütuna boşlukla tamamlar; sığmıyorsa tek boşluk bırakır.
inline std::string cliPadRight(const std::string& text, std::size_t width) {
    if (text.size() < width) return text + std::string(width - text.size(), ' ');
    return text + " ";
}

inline void printHelp(const std::vector<const CliCommand*>& commands) {
    // Sütun genişlikleri: kullanım satırı, seçenek adı, seçenek açıklaması.
    constexpr std::size_t kUsageColumn  = 55;
    constexpr std::size_t kOptionColumn = 29;
    constexpr std::size_t kHelpColumn   = 45;

    std::cout << "saQut " << SAQUT_VERSION << "\n\n";
    std::cout << "Usage: saqut [options] <command> [arguments]\n\n";
    std::cout << "Global options:\n";
    std::cout << "  -h, --help                 Display this help message\n";
    std::cout << "  -V, --version              Display the compiler version\n";

    std::cout << "\nCommands:\n";
    for (const CliCommand* cmd : commands) {
        if (cmd->hidden) continue;
        std::cout << "  " << cliPadRight(cmd->usage, kUsageColumn) << cmd->description << "\n";
    }

    // Seçenekler komuta özgüdür — yalnız onları kabul eden komutta geçerlidir.
    std::cout << "\nCommand options:\n";
    for (const CliOptionSpec& opt : cliOptions()) {
        if (!opt.help) continue;

        std::string left = opt.alias ? std::string("  ") + opt.alias + ", " : std::string(6, ' ');
        left += opt.name;
        if (opt.valueName) left += std::string("=") + opt.valueName;

        std::string where;
        for (const CliCommand* cmd : commands) {
            if (cmd->hidden || !(cmd->options & opt.bit)) continue;
            where += (where.empty() ? "" : ", ") + std::string(cmd->name);
        }

        // İlk satır komut parantezini taşır; '\n' sonrası satırlar hizalanır.
        const std::string help = opt.help;
        const std::size_t nl = help.find('\n');
        std::cout << cliPadRight(left, kOptionColumn)
                  << cliPadRight(help.substr(0, nl), kHelpColumn) << "(" << where << ")\n";
        if (nl != std::string::npos)
            std::cout << std::string(kOptionColumn, ' ') << help.substr(nl + 1) << "\n";
    }

    std::cout << "\nNotes:\n";
    std::cout << "  check always emits JSONL; it takes no output flags.\n";
    std::cout << "  Optimization (constant folding + dead code elimination) is ON by\n";
    std::cout << "  default; --dont-optimize turns it off. Optimization never changes a\n";
    std::cout << "  program's output, only how fast it gets there.\n";

    std::cout << "\nUse 'saqut --help' to display this message again.\n";
}

// parseArgs komutu listeden seçtiği için bulunamaması iç hatadır; yine de
// kullanım hatası olarak raporlanır.
inline int dispatch(const CliArgs& args, const std::vector<const CliCommand*>& commands) {
    if (args.showHelp) {
        printHelp(commands);
        return saqut::exit_code::kSuccess;
    }
    const CliCommand* cmd = findCliCommand(commands, args.command);
    if (!cmd || !cmd->run) {
        std::cerr << "error: unknown command '" << args.command << "'\n";
        std::cerr << "for available commands: saqut --help\n";
        return saqut::exit_code::kUsageError;
    }
    return cmd->run(args);
}

#endif // SAQUT_CLI
