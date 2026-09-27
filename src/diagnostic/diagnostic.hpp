// ============================================================================
// saQut Compiler — Tanılama (Diagnostic) Veri Yapıları + Hata Kataloğu
// ============================================================================
//
// DİZİN:   src/diagnostic/diagnostic.hpp
// KATMAN:  Tanı — bütün derleyici katmanları kullanır
// BAĞIMLI: src/core/location.hpp
// KULLANAN: DiagnosticEngine; tokenizer, parser, ModuleLoader, SymbolCollector,
//           TypeChecker, StructuralValidator, optimizer
//
// Derleme sırasında bulunan hata/uyarıları YAPISAL veri olarak temsil eder:
// bir Diagnostic seviye + kod + konum + mesaj taşır; ekrana basılan satır bunun
// yalnızca bir görünümüdür (terminal, `check` JSONL, LSP).
//
// KOD KAYDI (#295) — bu dosyadaki iki tablo kodların TEK kaynağıdır:
//   diagnosticCatalog()   derleme zamanı tanıları (E0xx, E9xx, W0xx, E_IMPORT_*…)
//   runtimeErrorCatalog() çalışma zamanı Error.code değerleri (E_DIVZERO, E_HOST…)
// docs/compiler-errors.md bu tablolardan üretilir (tests/gen_diagnostic_docs.cpp);
// tests/general/diagnostic_codes_test.sh kaynakta geçen her kodun kayıtlı ve
// kayıtlı her kodun kullanılıyor olduğunu denetler.
//
// YENİ TANI: tabloya bir satır (kod, seviye, başlık, açıklama) + report()
// çağrısı; ardından belgeyi yeniden üret:
//   ./build/gen_diagnostic_docs > docs/compiler-errors.md
// Var olan bir kodu yeni bir anlam için kullanma: bir kod tek bir hata
// sınıfını anlatır (E003'ün 21 anlama yayılması #295'te bölündü).
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
// Derleme zamanı tanı kataloğu — kod → seviye, başlık, açıklama
// ============================================================================
//
// Bağlama özel mesaj report() sırasında verilir; buradaki başlık ve açıklama
// kodun GENEL anlamıdır ve docs/compiler-errors.md'ye aynen yazılır.
// Makine yüzeyi (`check` JSONL, LSP) yalnız kodu taşır; kod değerleri
// kararlıdır, başlık/açıklama iyileştirilebilir.
// ============================================================================

struct DiagInfo {
    const char* code;
    DiagLevel   level;
    const char* title;        // kısa İngilizce başlık (mesajlarla aynı dil)
    const char* explanation;  // kullanıcıya açıklama: ne zaman çıkar, nasıl düzelir
};

inline const std::vector<DiagInfo>& diagnosticCatalog() {
    static const std::vector<DiagInfo> catalog = {
        // ── İç hata ─────────────────────────────────────────────────────────
        {"E000", DiagLevel::Error,   "Internal: AST could not be built",
         "Parser hiç AST üretemedi ve başka tanı da vermedi. Derleyici hatasıdır; bildirin."},

        // ── Ad, tip ve bildirim kuralları ───────────────────────────────────
        {"E001", DiagLevel::Error,   "Undefined name or member",
         "Ad (değişken, fonksiyon, alan, metot, enum üyesi) tanımlı değil ya da bu noktada görünmüyor. Yazımı ve kapsamı kontrol edin."},
        {"E002", DiagLevel::Error,   "Duplicate definition in same scope",
         "Aynı kapsamda aynı ad ikinci kez tanımlandı."},
        {"E003", DiagLevel::Error,   "Type mismatch in assignment",
         "Değer hedefin tipine atanamıyor: değişken başlatma/atama, fonksiyon argümanı ya da dönüş değeri. Gizli dönüşüm yoktur (ADR-010); `as` ile açık dönüşüm yapın. Nullable (`T?`) değer null denetimi olmadan `T`'ye atanamaz."},
        {"E004", DiagLevel::Error,   "break/continue outside loop/switch",
         "`break` yalnız döngü ya da switch içinde, `continue` yalnız döngü içinde kullanılabilir."},
        {"E005", DiagLevel::Error,   "return outside function",
         "`return` fonksiyon gövdesi dışında kullanıldı."},
        {"E006", DiagLevel::Error,   "Missing return value",
         "void olmayan fonksiyonun bazı yolları değer döndürmüyor ya da değersiz `return;` kullanılmış. Dönen değerin tipi uymuyorsa E003 verilir."},
        {"E007", DiagLevel::Error,   "Unknown type",
         "Tip adı tanınmıyor: ilkel tip, struct, enum ya da import edilmiş bir tip değil."},
        {"E008", DiagLevel::Error,   "Call argument count mismatch",
         "Fonksiyon ya da metot beklediğinden farklı sayıda argümanla çağrıldı."},
        {"E010", DiagLevel::Error,   "Recursive struct through non-nullable fields",
         "Struct kendisini nullable olmayan alanlar zinciriyle içeriyor; böyle bir değer hiç kurulamaz. Zincirdeki bir alanı nullable (`T?`) yapın."},
        {"E011", DiagLevel::Error,   "Declaration inside a function body",
         "struct, enum, fonksiyon ya da import bildirimi fonksiyon gövdesi içinde yapılamaz; modül düzeyine taşıyın."},
        {"E012", DiagLevel::Error,   "Type does not support [index] access",
         "`[ ]` ile indeksleme yalnız dizilerde tanımlıdır."},
        {"E013", DiagLevel::Error,   "Statement at module scope",
         "Modül (global) düzeyinde yalnız bildirim yazılabilir; deyimleri bir fonksiyonun (ör. main) içine taşıyın."},

        // ── İzole thread modeli (ADR-045) ───────────────────────────────────
        {"E014", DiagLevel::Error,   "shared / Pool / List declaration rule",
         "`shared` yerel bildirimde; shared tipi int/float/bool/Pool/List değil; `Pool(T)`/`List(T)` shared global başlatıcısı dışında; ya da Pool/List değeri shared global adı dışında kullanıldı."},
        {"E015", DiagLevel::Error,   "Type is not sendable",
         "Pool/List eleman tipi ya da `thread { }` yakalaması Pool/List/fonksiyon içeriyor; bu değerler thread'ler arasında kopyalanamaz."},
        {"E016", DiagLevel::Error,   "Assignment to a captured variable in a thread body",
         "`thread { x = 5; }` — `x` çevreleyen fonksiyonun yereli; gövde onun bir kopyasını görür, atama dışarı yansımaz."},
        {"E017", DiagLevel::Error,   "lock / unlock rule",
         "Kilit hedefi shared int/float/bool değil; aynı kilit ikinci kez alınıyor; ya da tutulmayan kilit bırakılıyor."},
        {"E018", DiagLevel::Error,   "wait condition has no shared symbol",
         "`wait` koşulu hiçbir shared değişkene bakmıyor; beklerken hiç değişemez."},
        {"E019", DiagLevel::Error,   "wait inside a lock scope",
         "`lock` kapsamı içinde `wait` (v1'de desteklenmez)."},

        // ── Tip kuralları (E003'ten ayrılan sınıflar, #295) ─────────────────
        {"E020", DiagLevel::Error,   "Literal does not fit its context",
         "Sayı literali hedef tipin aralığına sığmıyor (int, longint, byte) ya da ondalık literal tamsayı bağlamında kullanıldı."},
        {"E021", DiagLevel::Error,   "Operator not defined for these types",
         "Aritmetik, karşılaştırma, tekli işaret ya da `++`/`--` operatörü bu işlenen tiplerinde tanımlı değil (ör. string'de `<`, decimal'de `**`, longint ile başka bir sayı tipinin karışımı)."},
        {"E022", DiagLevel::Error,   "Nullable value used without a null check",
         "Nullable (`T?`) değer operatöre, üye erişimine ya da `++`/`--`'ye null denetimi olmadan verildi. `if (x != null)` ile daraltın ya da önce yerel bir değişkene alın."},
        {"E023", DiagLevel::Error,   "Invalid 'as' conversion",
         "`as` dönüşümü bu kaynak ve hedef tip çifti için tanımlı değil ya da hedef tip bilinmiyor."},
        {"E024", DiagLevel::Error,   "switch / case type mismatch",
         "switch konusu desteklenmeyen tipte, `case` değeri konunun tipine uymuyor ya da `case null` nullable olmayan bir konuda kullanıldı."},
        {"E025", DiagLevel::Error,   "Value is not callable",
         "Fonksiyon olmayan bir değer çağrıldı."},
        {"E026", DiagLevel::Error,   "Invalid method receiver",
         "Metot bu alıcı tipinde tanımlı değil (ör. `toString` yalnız `byte[]`'da) ya da `array::`/`struct::` ad alanına yanlış tipte ilk argüman verildi."},
        {"E027", DiagLevel::Error,   "Assignable location required",
         "`++`/`--` yalnız değişken, struct alanı ya da dizi elemanına uygulanabilir."},

        // ── Modül ve import ─────────────────────────────────────────────────
        {"E_MODULE_NOT_FOUND", DiagLevel::Error, "Imported module file not found",
         "`import ... from \"yol.sqt\"` dosyası bulunamadı. Yol, import eden dosyanın dizinine göre çözülür."},
        {"E_MODULE_PARSE", DiagLevel::Error, "Imported module could not be parsed",
         "İçe aktarılan dosyadan AST kurulamadı."},
        {"E_MODULE_CYCLE", DiagLevel::Error, "Circular module dependency",
         "Modüller birbirini döngüsel olarak içe aktarıyor; mesaj zinciri gösterir."},
        {"E_IMPORT_UNKNOWN", DiagLevel::Error, "Unknown module or imported name",
         "Gömülü modül (math, fs, …) ya da içe aktarılan ad bulunamadı."},
        {"E_IMPORT_NOT_EXPORTED", DiagLevel::Error, "Imported name is not exported",
         "Ad kaynak modülde tanımlı ama `export` ile işaretlenmemiş."},
        {"E_SYMBOL_NOT_IMPORTED", DiagLevel::Error, "Symbol used without import",
         "Başka bir modüle ait ad bu dosyada `import` edilmeden kullanıldı ya da farklı bir adla içe aktarıldı."},

        // ── Uyarılar ────────────────────────────────────────────────────────
        {"W002", DiagLevel::Warning, "Division by zero in a constant expression",
         "Sabit katlama sıfıra bölme buldu; çalışma zamanında E_DIVZERO atılır."},
        {"W003", DiagLevel::Warning, "Unreachable code",
         "return/break/continue'dan sonra gelen deyimler hiç çalışmaz ve derlemeden çıkarılır."},
        {"W004", DiagLevel::Warning, "Implicit numeric widening",
         "Sayı daha geniş bir tipe (int → float/double/decimal) örtük olarak genişletildi."},
        {"W005", DiagLevel::Warning, "Float case value is not exactly representable",
         "`case` ondalık değeri IEEE 754'te tam temsil edilemiyor; eşitlik karşılaştırması beklenen sonucu vermeyebilir."},
        {"W006", DiagLevel::Warning, "Deprecated builtin call syntax (ADR-033)",
         "Eleman tipi önekli eski metot sözdizimi (`int::push(a, x)`); `a.push(x)` ya da `array::push(a, x)` kullanın."},
        {"W007", DiagLevel::Warning, "Ignored capability requirement (ADR-043)",
         "`requires <yetenek>` yok sayılır: capability sistemi kaldırıldı."},
        {"W008", DiagLevel::Warning, "Non-atomic update of a shared variable",
         "`x = x + 1` / `x *= 2` ayrı yükle + yaz yapar; `+=`, `-=`, `++`, `--` atomiktir."},
        {"W009", DiagLevel::Warning, "Blocking call inside a lock scope",
         "Kilit tutulurken bekleyen bir çağrı (ör. `pop`, `join`) yapılıyor."},

        // ── Sözdizimi ve sözcüksel hatalar ──────────────────────────────────
        {"E901", DiagLevel::Error,   "Syntax error: unexpected token",
         "Bu noktada beklenmeyen bir token var; deyim atlanıp bir sonraki sınırdan devam edilir."},
        {"E902", DiagLevel::Error,   "Syntax error: expected type name after 'as'",
         "`as` sonrasında bir tip adı bekleniyor."},
        {"E903", DiagLevel::Error,   "Syntax error: expected member name",
         "`.` ya da `->` sonrasında üye adı bekleniyor."},
        {"E904", DiagLevel::Error,   "Syntax error: expected a name",
         "Değişken, parametre, fonksiyon ya da alan adı bekleniyor (ör. bir keyword ad olarak kullanıldı)."},
        {"E905", DiagLevel::Error,   "Syntax error: expected closing delimiter",
         "Kapanış sınırlayıcısı (`;`, `)`, `]`, `}`) eksik."},
        {"E906", DiagLevel::Error,   "Unknown escape sequence in string literal",
         "Desteklenen kaçışlar: \\n \\t \\r \\b \\\\ \\\"."},
        {"E907", DiagLevel::Error,   "Unterminated string literal",
         "String literali kapanış `\"` bulunmadan dosya sonuna ulaştı."},
    };
    return catalog;
}

// ============================================================================
// Çalışma zamanı hata kodları — saQut `Error.code` değerleri
// ============================================================================
//
// VM, JIT, built-in metotlar ve host fonksiyonları çalışma zamanında bir hata
// fırlatırken Error.code'a bu kodlardan birini yazar; program `try/catch` ile
// yakalayıp `e.code`'a bakabilir. Kod değerleri kararlıdır.
// ============================================================================

struct RuntimeErrorInfo {
    const char* code;
    const char* title;
    const char* explanation;
};

inline const std::vector<RuntimeErrorInfo>& runtimeErrorCatalog() {
    static const std::vector<RuntimeErrorInfo> catalog = {
        {"E_DIVZERO", "Division or modulo by zero",
         "int / longint / float bölme ya da mod işleminde bölen sıfır."},
        {"E_DECIMAL_DIVZERO", "Decimal division or modulo by zero",
         "decimal bölme ya da mod işleminde bölen sıfır."},
        {"E_DECIMAL_OVERFLOW", "Decimal overflow",
         "decimal işleminin sonucu temsil aralığını aştı."},
        {"E_POWNEG", "Negative integer exponent",
         "Tamsayı `**` işleminde üs negatif; sonuç tamsayı olamaz."},
        {"E_CAST", "Checked conversion failed",
         "`as` dönüşümü değeri hedef tipe sığdıramadı (aralık dışı, NaN/Inf, sayı olmayan metin). `as T?` biçimi hata yerine null verir."},
        {"E_OOB", "Array index out of bounds",
         "Dizi indeksi 0 ile uzunluk-1 aralığının dışında."},
        {"E_NULL", "Null reference access",
         "null struct/dizi değerinin alanına ya da elemanına erişildi."},
        {"E_TYPE", "Unexpected runtime value type",
         "İşlem beklediği değer türünü (ör. dizi) almadı."},
        {"E_STACK_OVERFLOW", "Maximum call depth exceeded",
         "Özyineleme `--max-call-depth` sınırını aştı (varsayılan 100000)."},
        {"E_HOST", "Built-in or host function failed",
         "Bir built-in metot ya da FFI host fonksiyonu hata bildirdi (ör. dosya açılamadı, geçersiz argüman); ayrıntı mesajdadır."},
        {"E_LIST_INDEX", "List index out of range",
         "`List.get(i)` için i < 0 ya da i >= length()."},
        {"E_LIST_FULL", "List is full",
         "List kapasitesi (≈16,7 milyon eleman) doldu."},
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
