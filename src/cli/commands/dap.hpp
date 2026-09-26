// ============================================================================
// saQut CLI — dap komutu (Debug Adapter Protocol)
// ============================================================================

#ifndef SAQUT_CLI_DAP
#define SAQUT_CLI_DAP

#include "cli/args.hpp"
#include "dap/dap_server.hpp"

inline int cmdDap(const CliArgs&) {
    DapServer server;
    server.run();
    return 0;
}

inline constexpr CliCommand kDapCommand{
    .name          = "dap",
    .usage         = "saqut dap",
    .description   = "start DAP debug adapter (JSON-RPC on stdin/stdout)",
    .minPositional = 0,
    .maxPositional = 0,
    .options       = OPT_STDIO,
    .run           = cmdDap,
};

#endif // SAQUT_CLI_DAP
