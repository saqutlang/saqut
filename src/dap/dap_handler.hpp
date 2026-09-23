// ============================================================================
// saQut DAP — DapHandler (Hata Ayıklama Protokolü Dispatch)
// ============================================================================

#ifndef SAQUT_DAP_HANDLER
#define SAQUT_DAP_HANDLER

#include "vendor/nlohmann/json.hpp"
#include "lsp/json_rpc.hpp"
#include "dap/dap_types.hpp"
#include "dap/frame_reader.hpp"
#include "vm/interpreter.hpp"
#include "ir/ir_program.hpp"
#include "vm/value.hpp"
#include <deque>
#include <ostream>
#include <map>
#include <memory>
#include <unordered_map>

class DapHandler {
public:
    explicit DapHandler(std::ostream& out) : out_(out) {}

    // Ana giriş: DAP-format mesajı işle, response döndür (null = response yok).
    // Event'ler doğrudan out_'a yazılır.
    nlohmann::json dispatch(const nlohmann::json& msg);

    // Faz 8 (#105): DAP okuma yolu std::cin DEĞİL — FrameReader (fd tabanlı).
    // Server ana döngüsü de runWithBudget'ın tur-arası kontrolü de aynı
    // okuyucuyu kullanır; stdio tamponunun baytları yutması diye bir şey yok.
    FrameReader& reader() { return reader_; }

private:
    std::ostream&               out_;
    FrameReader                 reader_;
    std::unique_ptr<IRProgram>  irProgram_;
    std::unique_ptr<Interpreter> vm_;
    int                         nextBpId_   = 1;
    // Child variablesReference'ları: scopes 1000+frameId kullandığı için
    // çakışmasın diye 100000'den başlar. Her koşu devamında (continue/step)
    // geçersizleşir — DAP spec'i de öyle söyler (resume → eski ref'ler ölür).
    int                          nextVarRef_ = 100000;
    std::unordered_map<int, Value> varRefs_;

    // Yeni yaşam döngüsü durumu
    bool  initialized_    = false;
    int   responseSeq_    = 100;     // cevap/event seq numaraları
    // Faz 7 (#105): launch argümanından; false = configurationDone sonrası
    // entry'de durmadan koşuya başla (DAP varsayılanı).
    bool  stopOnEntry_    = false;
    // D-7: exited/terminated oturumda yalnız bir kez gönderilir.
    bool  terminationSent_ = false;
    // D-4: (dosya, satır) → breakpoint id; stopped olayında hitBreakpointIds.
    std::map<std::pair<std::string, int>, int> bpIds_;
    void sendTermination(bool withExit);
    // Program bittiyse ya da durduysa uygun olayı gönderir (adım işleyicileri).
    void reportStepResult();

    // ── Handler'lar ──────────────────────────────────────────────────────────
    nlohmann::json handleInitialize(const nlohmann::json& req);
    nlohmann::json handleLaunch(const nlohmann::json& req);
    nlohmann::json handleSetBreakpoints(const nlohmann::json& req);
    nlohmann::json handleConfigurationDone(const nlohmann::json& req);
    nlohmann::json handleContinue(const nlohmann::json& req);
    nlohmann::json handleNext(const nlohmann::json& req);
    nlohmann::json handleStepIn(const nlohmann::json& req);
    nlohmann::json handleStepOut(const nlohmann::json& req);
    nlohmann::json handlePause(const nlohmann::json& req);
    nlohmann::json handleThreads(const nlohmann::json& req);
    nlohmann::json handleStackTrace(const nlohmann::json& req);
    nlohmann::json handleScopes(const nlohmann::json& req);
    nlohmann::json handleVariables(const nlohmann::json& req);
    nlohmann::json handleEvaluate(const nlohmann::json& req);
    nlohmann::json handleTerminate(const nlohmann::json& req);
    nlohmann::json handleDisconnect(const nlohmann::json& req);

    // ── Yardımcılar ──────────────────────────────────────────────────────────
    nlohmann::json makeResponse(int requestSeq, const std::string& command,
                                const nlohmann::json& body,
                                bool success = true);
    void sendEvent(const std::string& event, const nlohmann::json& body);

    // Değişken değerini DAP string'ine çevir (struct/array için özet)
    std::string valueToString(const Value& v, int depth = 0) const;

    // Ref taşıyan Value'yu kaydet, VS Code'un sonradan variables isteğinde
    // bulacağı variablesReference numarasını döndür (Ref değilse 0).
    int registerVarRef(const Value& v);
    // Koşu devam ederken eski referanslar geçersizleşir.
    void invalidateVarRefs();

    // Struct/array child variable'ları oluştur (çocuklar da register edilir
    // → istenildiği kadar derine inilebilir)
    nlohmann::json buildChildVariables(const Value& v);

    // "a", "a.x", "vecs[0].z" gibi basit ifadeleri frame slotlarından çözer.
    // Başarısızsa nullptr-value döner (found=false).
    bool resolveExpression(const std::string& expr, int frameId, Value& out) const;

    // Koşu: budget döngüsüyle VM çalıştır, event'leri yönet.
    // Faz 8 (#105): tur arası stdin kontrolü — pause işlenir, diğer istekler
    // pendingRequests_'e kuyruklanıp koşu durunca drainPendingRequests ile işlenir.
    void runWithBudget();
    void drainPendingRequests();

    // Faz 8 (#105): bütçe turu boyutu — tur arası pause gecikmesinin üst sınırı.
    static constexpr int kRunBudgetChunk = 100000;
    std::deque<nlohmann::json> pendingRequests_;
};

#endif // SAQUT_DAP_HANDLER
