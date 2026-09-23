// ============================================================================
// saQut Compiler — Tanılama (Diagnostic) Veri Yapıları + Hata Kataloğu
// ============================================================================
//
// DİZİN:   src/diagnostic/diagnostic.hpp
// KATMAN:  Katman 0 — Tüm analiz katmanları tarafından kullanılır
// BAĞIMLI: src/core/location.hpp
// KULLANAN: DiagnosticEngine, sembol toplayıcı (Faz 2), tip denetleyici (Faz 3)
//
// AMAÇ:
//   Derleme sırasında bulunan hata/uyarıları YAPISAL veri olarak temsil eder.
//   "Veri birincil, insan-okur metin bir görünümdür" (readme → Tasarım felsefesi):
//   bir Diagnostic; seviye + kod + konum + mesaj taşır; ekrana basılan satır
//   bunun yalnızca bir render'ıdır. Bu sayede aynı tanı LSP, AI veya `saqut
//   explain` tarafından da tüketilebilir.
//
// HATA KATALOĞU (baştan sabitlenir — yeni kodlar buraya eklenir):
//   E001  Tanımsız değişken/isim (declare-before-use ihlali dâhil)        Faz 2/3
//   E002  Aynı scope'ta çift tanım                                         Faz 2
//   E003  Tip uyuşmazlığı (gizli dönüşüm yok, ADR-010)                     Faz 3
//   E004  Döngü/switch dışı break/continue                                 Faz 3
//   E005  Fonksiyon dışı return                                           Faz 3
//   E006  Return tipi imzaya uymuyor                                       Faz 3
//   E007  Tanımsız tip (bilinmeyen tip adı)                               Faz 2/3
//   E008  Fonksiyon çağrısı argüman sayısı/tipi uyuşmuyor                  Faz 3
//   E009  Array boyutu sabit değil / geçersiz                             Faz 3
//   E010  Özyinelemeli/döngüsel struct (by-value çevrim → sonsuz boyut)   Faz 2/3
//   E011  struct/fonksiyon bildirimi fonksiyon gövdesi içinde             Faz 3
//   W001  Kullanılmayan değişken                                          Faz 4
//   W002  Sıfıra bölme (sabit folding)                                    Faz 4
//   W003  Erişilemez (ölü) kod                                            Faz 4
//   E901  Sözdizimi hatası — beklenmeyen token (statement seviyesi)        Faz 2
//   E902  Sözdizimi hatası — 'as' sonrası tip adı bekleniyor                Faz 2
//   E903  Sözdizimi hatası — '.'/'->' sonrası üye adı bekleniyor            Faz 2
//   E904  Sözdizimi hatası — değişken adı bekleniyor                       Faz 2
//   E905  Sözdizimi hatası — kapanış sınırlayıcısı (';' ')' ']' '}') bekleniyor
//   E906  Sözcüksel hata — string'de tanınmayan kaçış dizisi (#256)
//   E907  Sözcüksel hata — kapanmamış string literali (#256)
//
// ============================================================================

#ifndef SAQUT_DIAGNOSTIC_DIAGNOSTIC
#define SAQUT_DIAGNOSTIC_DIAGNOSTIC

#include <string>
#include <vector>
#include <algorithm>
#include "core/location.hpp"
#include "tools.hpp"   // jsonEscape — TEK tanım (tools.hpp); çakışmayı önler

// ============================================================================
// DiagLevel — Tanı seviyesi
// ============================================================================

enum class DiagLevel { Error, Warning, Note, Hint };

inline const char* diagLevelName(DiagLevel l) {
    switch (l) {
        case DiagLevel::Error:   return "error";
        case DiagLevel::Warning: return "warning";
        case DiagLevel::Note:    return "note";
        case DiagLevel::Hint:    return "hint";
    }
    return "?";
}


// NOT: jsonEscape() tools.hpp'de tanımlıdır (tek tanım — ODR çakışması olmaz).

// ============================================================================
// Diagnostic — Tek bir tanı (hata/uyarı/not/ipucu)
// ============================================================================
//
// KULLANIM:
//   Diagnostic d{DiagLevel::Error, "E003", loc, "int'e string atanamaz"};
//   d.hint = "açık dönüşüm gerekiyor";
//   std::cout << d.toJson();
// ============================================================================

struct Diagnostic {
    DiagLevel      level = DiagLevel::Error;
    std::string    code;
    SourceLocation loc;
    std::string    message;
    std::string    hint;
    int            tokenLength = 1; // LSP range genişliği (karakter sayısı)

    nlohmann::json toJsonObj() const {
        nlohmann::json j;
        j["level"]    = diagLevelName(level);
        j["code"]     = code;
        j["location"] = loc.toJsonObj();
        j["message"]  = message;
        if (!hint.empty()) j["hint"] = hint;
        return j;
    }

    std::string toJson() const { return toJsonObj().dump(); }
};

// ============================================================================
// Hata Kataloğu — kod → (seviye, kanonik başlık)
// ============================================================================
//
// Bağlama özel mesaj report sırasında verilir; buradaki başlık, kodun GENEL
// anlamıdır (ileride `saqut explain E003` bunu kullanabilir, #107/#98).
// ============================================================================

// #131: tek katalog — kod, seviye, başlık (kısa) ve kanonik mesaj.
// Bağlama özel mesaj report sırasında verilir (katalogdaki mesaj sabit
// kısmı taşır); title `saqut explain <kod>` için, message üretim noktalarının
// şablonu içindir. 1.0 freeze (ADR-038) bu tabloyu dondurur.
struct DiagInfo {
    const char* code;
    DiagLevel   level;
    const char* title;
    const char* message;  // kanonik mesaj şablonu (#131)
};

inline const std::vector<DiagInfo>& diagnosticCatalog() {
    static const std::vector<DiagInfo> catalog = {
        {"E001", DiagLevel::Error,   "Undefined variable/name", "'{0}' is not defined"},
        {"E002", DiagLevel::Error,   "Duplicate definition in same scope", "'{0}' already defined in this scope"},
        {"E003", DiagLevel::Error,   "Type mismatch", "type mismatch: expected {0}, got {1}"},
        {"E004", DiagLevel::Error,   "break/continue outside loop/switch", "break/continue outside loop/switch"},
        {"E005", DiagLevel::Error,   "return outside function", "return outside function"},
        {"E006", DiagLevel::Error,   "Return type does not match signature", "'{0}' function must return {1} but some paths have no return"},
        {"E007", DiagLevel::Error,   "Undefined type", "unknown type: '{0}'"},
        {"E008", DiagLevel::Error,   "Function call argument mismatch", "argument mismatch in call to '{0}'"},
        {"E009", DiagLevel::Error,   "Array size is not constant / invalid", "array size must be a constant expression"},
        {"E010", DiagLevel::Error,   "Recursive/cyclic struct definition", "recursive struct definition: '{0}'"},
        {"E011", DiagLevel::Error,   "struct/function declaration inside a function body", "declaration inside a function body"},
        {"E012", DiagLevel::Error,   "Type does not support [index] access", "type '{0}' does not support [index] access"},
        {"E013", DiagLevel::Error,   "Statement at module (global) scope", "statements are not allowed at module scope; move it inside a function (e.g. main)"},
        {"W001", DiagLevel::Warning, "Unused variable", "'{0}' is never used"},
        {"W002", DiagLevel::Warning, "Division by zero (constant expression)", "division by zero in constant expression"},
        {"W003", DiagLevel::Warning, "Unreachable (dead) code", "unreachable code"},
        {"W004", DiagLevel::Warning, "Implicit numeric widening", "implicit numeric widening from {0} to {1}"},
        {"W006", DiagLevel::Warning, "Deprecated builtin call syntax (ADR-033)", "deprecated builtin call syntax"},
        {"W007", DiagLevel::Warning, "Deprecated capability requirement (ADR-043)", "requires '{0}' is ignored — capabilities were removed (ADR-043)"},
        {"E901", DiagLevel::Error,   "Syntax error: unexpected token", "unexpected token"},
        {"E902", DiagLevel::Error,   "Syntax error: expected type name after 'as'", "expected type name after 'as'"},
        {"E903", DiagLevel::Error,   "Syntax error: expected member name", "expected member name"},
        {"E904", DiagLevel::Error,   "Syntax error: expected variable name", "expected variable name"},
        {"E905", DiagLevel::Error,   "Syntax error: expected closing delimiter", "expected closing delimiter"},
        {"E906", DiagLevel::Error,   "Syntax error: unknown escape sequence in string literal", "unknown escape sequence"},
        {"E907", DiagLevel::Error,   "Syntax error: unterminated string literal", "unterminated string literal"},
    };
    return catalog;
}

// Kod kataloğda var mı? (yoksa nullptr)
inline const DiagInfo* findDiag(const std::string& code) {
    for (const auto& d : diagnosticCatalog())
        if (code == d.code) return &d;
    return nullptr;
}

// Bir koddan Diagnostic üretir; seviye kataloğdan çözülür (yoksa: E→Error,
// W→Warning, diğer→Note). Bağlama özel mesajı çağıran verir.
inline Diagnostic makeDiagnostic(const std::string& code,
                                 const SourceLocation& loc,
                                 const std::string& message,
                                 const std::string& hint = "") {
    DiagLevel level = DiagLevel::Note;
    if (const DiagInfo* info = findDiag(code)) {
        level = info->level;
    } else if (!code.empty()) {
        if (code[0] == 'E') level = DiagLevel::Error;
        else if (code[0] == 'W') level = DiagLevel::Warning;
    }
    Diagnostic d;
    d.level   = level;
    d.code    = code;
    d.loc     = loc;
    d.message = message;
    d.hint    = hint;
    return d;
}

// ============================================================================
// Yazım-hatası önerisi — E001 "did you mean?" için
// ============================================================================

inline int diagEditDistance(const std::string& a, const std::string& b) {
    size_t m = a.size(), n = b.size();
    if (m > 32 || n > 32) return 99;
    std::vector<std::vector<int>> dp(m+1, std::vector<int>(n+1, 0));
    for (size_t i = 0; i <= m; i++) dp[i][0] = (int)i;
    for (size_t j = 0; j <= n; j++) dp[0][j] = (int)j;
    for (size_t i = 1; i <= m; i++)
        for (size_t j = 1; j <= n; j++) {
            int sub = dp[i-1][j-1] + (a[i-1] == b[j-1] ? 0 : 1);
            dp[i][j] = std::min(dp[i-1][j]+1, std::min(dp[i][j-1]+1, sub));
        }
    return dp[m][n];
}

// Adaylar arasından en yakın ismi döndürür; mesafe ≥ 3 ise boş string.
inline std::string suggestName(const std::string& unknown,
                                const std::vector<std::string>& candidates) {
    std::string best;
    int bestDist = 3;
    for (const auto& c : candidates) {
        if (c.empty() || c[0] == '_') continue;
        int d = diagEditDistance(unknown, c);
        if (d < bestDist) { bestDist = d; best = c; }
    }
    return best;
}

#endif // SAQUT_DIAGNOSTIC_DIAGNOSTIC
