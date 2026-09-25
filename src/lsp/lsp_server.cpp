#include "lsp/lsp_server.hpp"
#include "lsp/json_rpc.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// İki thread (docs/lsp-decisions.md "İş parçacığı modeli"):
//   - okuyucu: stdin'den mesajları okuyup kuyruğa atar (yalnız G/Ç);
//   - analiz (bu thread): kuyruktaki mesajları sırayla işler; kuyruk boşken
//     proje indeksini dosya dosya ilerletir (handler_.idleStep()).
// Ön ucun tekilleri (FileRegistry vb.) kilitsiz olduğu için tüm derleyici
// işi tek thread'de kalır; indeksleme bir isteği en fazla tek dosyanın
// indeksleme süresi kadar bekletir.
void LspServer::run() {
    std::mutex                 mu;
    std::condition_variable    cv;
    std::deque<nlohmann::json> queue;
    bool                       eof = false;

    std::thread reader([&] {
        while (std::cin.good()) {
            auto msg = JsonRpc::readMessage(std::cin);
            if (msg.is_null() || msg.is_discarded()) continue;
            {
                std::lock_guard<std::mutex> lk(mu);
                queue.push_back(std::move(msg));
            }
            cv.notify_one();
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            eof = true;
        }
        cv.notify_one();
    });

    for (;;) {
        nlohmann::json msg;
        {
            std::unique_lock<std::mutex> lk(mu);
            if (queue.empty() && !eof && handler_.hasIdleWork()) {
                lk.unlock();
                handler_.idleStep();
                continue;
            }
            cv.wait(lk, [&] { return !queue.empty() || eof; });
            if (queue.empty()) break;   // eof ve iş kalmadı
            msg = std::move(queue.front());
            queue.pop_front();
        }

        // Faz 6 (#84): handler'da beklenmedik istisna (eksik params alanı,
        // tip uyuşmazlığı) sunucuyu düşürmemeli — istek ise InternalError
        // yanıtı dön, notification ise yut ve sonraki mesaja geç.
        nlohmann::json response;
        try {
            response = handler_.dispatch(msg);
        } catch (const std::exception& e) {
            nlohmann::json id = msg.value("id", nlohmann::json(nullptr));
            if (id.is_null()) continue;
            response = JsonRpc::makeError(id, -32603,
                std::string("Internal error: ") + e.what());
        }

        if (!response.is_null())
            JsonRpc::writeMessage(std::cout, response);
    }
    reader.join();
}
