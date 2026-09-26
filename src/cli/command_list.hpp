// ============================================================================
// saQut Compiler — CLI komut listesi
// ============================================================================
//
// DİZİN:   src/cli/command_list.hpp
// BAĞIMLI: cli/commands/*
//
// `saqut`'un tanıdığı komutların tek listesi. parseArgs, yardım metni ve
// dispatch bu listeyi okur; komut adı başka hiçbir yerde tekrar yazılmaz.
//
// YENİ KOMUT EKLEMEK İÇİN:
//   1. src/cli/commands/x.hpp: `int cmdX(const CliArgs&)` fonksiyonu ve onun
//      `inline constexpr CliCommand kXCommand{...}` tanımı (örnek: tokens.hpp;
//      alanlar için cli/args.hpp → CliCommand).
//   2. Bu dosyada: #include + listeye `&kXCommand`.
// Komutun yeni bir seçeneğe ihtiyacı varsa: cli/args.hpp başlığındaki
// "YENİ SEÇENEK" adımları.
//
// ============================================================================

#ifndef SAQUT_CLI_COMMAND_LIST
#define SAQUT_CLI_COMMAND_LIST

#include <vector>
#include "cli/args.hpp"
#include "cli/commands/ast.hpp"
#include "cli/commands/bench.hpp"
#include "cli/commands/check.hpp"
#include "cli/commands/dap.hpp"
#include "cli/commands/exec.hpp"
#include "cli/commands/ir.hpp"
#include "cli/commands/lsp.hpp"
#include "cli/commands/run.hpp"
#include "cli/commands/symbols.hpp"
#include "cli/commands/tokens.hpp"

inline const std::vector<const CliCommand*>& allCommands() {
    // Yardımdaki "Commands" sırası bu listenin sırasıdır.
    static const std::vector<const CliCommand*> list = {
        &kRunCommand,
        &kTokensCommand,
        &kAstCommand,
        &kSymbolsCommand,
        &kCheckCommand,
        &kIrCommand,
        &kExecCommand,
        &kLspCommand,
        &kDapCommand,
        &kBenchCommand,
    };
    return list;
}

#endif // SAQUT_CLI_COMMAND_LIST
