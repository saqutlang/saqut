// ============================================================================
// saQut LSP — Çalışma alanı özellikleri (Bölüm 2/3, docs/lsp-decisions.md)
// ============================================================================

#include "lsp/lsp_handler.hpp"

nlohmann::json LspHandler::importableNamesFromFile(DocumentState&, const std::string&) {
    return nlohmann::json::array();
}

void LspHandler::appendProjectCompletions(DocumentState&, const std::string&,
                                          const std::set<std::string>&, nlohmann::json&) {}
