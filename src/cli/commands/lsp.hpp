// ============================================================================
// saQut CLI — lsp komutu (Language Server Protocol)
// ============================================================================

#ifndef SAQUT_CLI_LSP
#define SAQUT_CLI_LSP

#include "cli/args.hpp"
#include "lsp/lsp_server.hpp"

inline int cmdLsp(const CliArgs&) {
    LspServer server;
    server.run();
    return 0;
}

inline constexpr CliCommand kLspCommand{
    .name          = "lsp",
    .usage         = "saqut lsp",
    .description   = "start LSP server (JSON-RPC on stdin/stdout)",
    .minPositional = 0,
    .maxPositional = 0,
    .options       = OPT_STDIO,
    .run           = cmdLsp,
};

#endif // SAQUT_CLI_LSP
