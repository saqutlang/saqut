// ============================================================================
// saQut Compiler — Tanılama Motoru (DiagnosticEngine)
// ============================================================================
//
// DİZİN:   src/diagnostic/diagnostic_engine.hpp
// KATMAN:  Katman 0 — Tüm analiz katmanları tarafından kullanılır
// BAĞIMLI: src/diagnostic/diagnostic.hpp
// KULLANAN: sembol toplayıcı (Faz 2), tip denetleyici (Faz 3), pipeline (main)
//
// AMAÇ:
//   Derleme boyunca üretilen tüm Diagnostic'leri EKLENME SIRASIYLA biriktirir.
//   İlk hatada DURMAZ (ADR-013): bütün hatalar toplanır, faz sonunda topluca
//   raporlanır; durdurma kararını pipeline verir (hasErrors()).
//
//   İki çıktı yüzü vardır — aynı veriden:
//     printAll() → insan-okur (terminal)
//     toJson()   → makine-okur (LSP / AI / araçlar)
//
// ============================================================================

#ifndef SAQUT_DIAGNOSTIC_ENGINE
#define SAQUT_DIAGNOSTIC_ENGINE

#include <cctype>
#include <string>
#include <vector>
#include <ostream>
#include "diagnostic/diagnostic.hpp"
#include "vendor/nlohmann/json.hpp"

// ============================================================================
// DiagnosticEngine
// ============================================================================
//
// KULLANIM:
//   DiagnosticEngine diag;
//   diag.report(makeDiagnostic("E001", loc, "x tanımsız"));
//   diag.report(DiagLevel::Warning, "W001", loc2, "y kullanılmıyor");
//   if (diag.hasErrors()) diag.printAll(std::cerr);
// ============================================================================

class DiagnosticEngine {
public:
    // --- Ekleme ---
    void report(const Diagnostic& d) {
        diagnostics_.push_back(d);
        // #246: modül ad alanı geçişinin iç adları (`helper@2`) kullanıcıya
        // görünmez; tanılar kaynakta yazılan adı gösterir.
        auto strip = [](std::string& t) {
            std::string out;
            for (size_t i = 0; i < t.size(); ++i) {
                if (t[i] == '@' && i > 0 && i + 1 < t.size() &&
                    (std::isalnum((unsigned char)t[i - 1]) || t[i - 1] == '_' || t[i - 1] == '$') &&
                    std::isdigit((unsigned char)t[i + 1])) {
                    while (i + 1 < t.size() && std::isdigit((unsigned char)t[i + 1])) ++i;
                    continue;
                }
                out += t[i];
            }
            t = out;
        };
        strip(diagnostics_.back().message);
        strip(diagnostics_.back().hint);
    }

    // Kolaylık: koddan üret + ekle (seviye kataloğdan çözülür)
    void report(const std::string& code,
                const SourceLocation& loc,
                const std::string& message,
                const std::string& hint = "",
                int tokenLength = 1) {
        auto d = makeDiagnostic(code, loc, message, hint);
        d.tokenLength = tokenLength;
        diagnostics_.push_back(d);
    }

    // Kolaylık: seviyeyi açıkça vererek
    void report(DiagLevel level,
                const std::string& code,
                const SourceLocation& loc,
                const std::string& message,
                const std::string& hint = "",
                int tokenLength = 1) {
        Diagnostic d;
        d.level = level; d.code = code; d.loc = loc; d.message = message;
        d.hint = hint; d.tokenLength = tokenLength;
        diagnostics_.push_back(d);
    }

    // --- Sorgu ---
    bool hasErrors() const { return errorCount() > 0; }

    int errorCount() const { return countLevel(DiagLevel::Error); }
    int warningCount() const { return countLevel(DiagLevel::Warning); }
    int count() const { return static_cast<int>(diagnostics_.size()); }
    bool empty() const { return diagnostics_.empty(); }

    const std::vector<Diagnostic>& all() const { return diagnostics_; }

    void clear() { diagnostics_.clear(); }

    // --- İnsan-okur çıktı (ekleme sırasıyla) ---
    void printAll(std::ostream& os) const {
        for (const auto& d : diagnostics_) {
            os << d.loc.toString() << ": "
               << diagLevelName(d.level) << " [" << d.code << "]: "
               << d.message << "\n";
            if (!d.hint.empty())
                os << "    hint: " << d.hint << "\n";
        }
        os << "— " << errorCount() << " error(s), " << warningCount() << " warning(s)\n";
    }

    // --- Makine-okur çıktı ---
    nlohmann::json toJsonObj() const {
        nlohmann::json items = nlohmann::json::array();
        for (const auto& d : diagnostics_)
            items.push_back(d.toJsonObj());
        return {
            {"diagnostics",  items},
            {"errorCount",   errorCount()},
            {"warningCount", warningCount()}
        };
    }

    std::string toJson() const { return toJsonObj().dump(); }

    nlohmann::json toLspDiagnostics() const {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& d : diagnostics_) {
            nlohmann::json item;
            auto pos = d.loc.toLspPosition();
            item["range"] = {
                {"start", {{"line", pos.line}, {"character", pos.character}}},
                {"end",   {{"line", pos.line}, {"character", pos.character + d.tokenLength}}}
            };
            item["severity"] = (d.level == DiagLevel::Error) ? 1 : 2;
            item["code"]     = d.code;
            item["message"]  = d.hint.empty() ? d.message
                                              : d.message + "\n" + d.hint;
            item["source"]   = "saQut";
            arr.push_back(item);
        }
        return arr;
    }

private:
    std::vector<Diagnostic> diagnostics_;

    int countLevel(DiagLevel level) const {
        int n = 0;
        for (const auto& d : diagnostics_)
            if (d.level == level) ++n;
        return n;
    }
};

#endif // SAQUT_DIAGNOSTIC_ENGINE
