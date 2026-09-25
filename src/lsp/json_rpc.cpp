#include "lsp/json_rpc.hpp"
#include <fstream>
#include <chrono>
#include <cstdlib>
#include <ctime>

// İletişim günlüğü yalnız SAQUT_LSP_LOG=<dosya> verildiğinde yazılır. Eskiden
// her mesaj /tmp/saqut-lsp.log'a girintili dökülüyordu: büyük yanıtlarda
// (semanticTokens, workspace/symbol) gecikme ekliyor ve kullanıcının kaynak
// kodunu her zaman diske yazıyordu (docs/lsp-decisions.md).
static void lspLog(const char* dir, const nlohmann::json& msg) {
    static const char* path = std::getenv("SAQUT_LSP_LOG");
    if (!path || !*path) return;
    static std::ofstream log(path, std::ios::app);
    if (!log.is_open()) return;
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char ts[20];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&t));
    std::string method = msg.contains("method") ? msg["method"].get<std::string>() : "";
    log << "[" << ts << "] " << dir;
    if (!method.empty()) log << " " << method;
    log << "\n" << msg.dump(2) << "\n---\n";
    log.flush();
}

nlohmann::json JsonRpc::readMessage(std::istream& in) {
    int contentLength = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        if (line.rfind("Content-Length:", 0) == 0) {
            // Faz 6 (#84): bozuk başlık ("Content-Length: abc") sunucuyu
            // düşürmemeli — stoi fırlatırsa 0 kalır, mesaj atlanır.
            try {
                contentLength = std::stoi(line.substr(16));
            } catch (...) {
                contentLength = 0;
            }
        }
    }
    if (contentLength <= 0) return nullptr;
    std::string body(contentLength, '\0');
    in.read(body.data(), contentLength);
    auto msg = nlohmann::json::parse(body, nullptr, false);
    if (!msg.is_discarded()) lspLog("<<<", msg);
    return msg;
}

void JsonRpc::writeMessage(std::ostream& out, const nlohmann::json& msg) {
    lspLog(">>>", msg);
    std::string body = msg.dump();
    out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    out.flush();
}

nlohmann::json JsonRpc::makeResponse(const nlohmann::json& id,
                                      const nlohmann::json& result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

nlohmann::json JsonRpc::makeNotification(const std::string& method,
                                          const nlohmann::json& params) {
    return {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
}

nlohmann::json JsonRpc::makeError(const nlohmann::json& id,
                                   int code, const std::string& msg) {
    return {{"jsonrpc", "2.0"}, {"id", id},
            {"error", {{"code", code}, {"message", msg}}}};
}
