// ============================================================================
// saQut Compiler — Lexer (Karakter Seviyesinde Tarayıcı)
// ============================================================================
//
// DİZİN:   src/lexer/lexer.hpp
// KATMAN:  Katman 1 — Derleyici pipeline'ının ilk aşaması
// BAĞIMLI: Yok (sadece standart kütüphane)
// KULLANAN: Tokenizer (src/tokenizer/tokenizer.hpp)
//
// AMAÇ:
//   Ham kaynak kodu (std::string) karakter karakter tarayarak:
//   1. Karakter konumunu takip eder (offset)
//   2. Backtracking (geri alma) desteği ile desen eşleme yapar
//   3. Sayısal literal'ları okur ve sınıflandırır (decimal, hex, binary, octal, float)
//   4. Boşluk karakterlerini atlar
//   5. Satır/sütun bilgisi sağlar (hata mesajları için temel)
//
// ============================================================================

#ifndef SAQUT_LEXER
#define SAQUT_LEXER

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "core/location.hpp"
#include "core/sourcefile.hpp"

// ============================================================================
// INumber — Ara Sayısal Veri Yapısı
// ============================================================================

struct INumber {
    int start = 0;           // Kaynak koddaki başlangıç offset'i
    int end   = 0;           // Kaynak koddaki bitiş offset'i
    SourceLocation startLoc; // Kaynak koddaki başlangıç konumu (line, column)
    SourceLocation endLoc;   // Kaynak koddaki bitiş konumu
    std::string token;       // Sayının ham metni (örn: "42", "0xFF", "3.14e-2")
    bool isFloat    = false; // true ise float/double literal
    bool hasEpsilon = false; // true ise bilimsel gösterim (örn: 1e10)
    int base        = 10;    // Sayı tabanı: 2, 8, 10, 16
    bool positive   = true;  // false ise sayı negatif
};

// ============================================================================
// Lexer — Karakter Seviyesinde Tarayıcı
// ============================================================================

class Lexer {
public:
    // --- Ham Veri ---
    std::string input;       // Kaynak kodun tamamı
    int size   = 0;          // input.length() önbelleği
    int offset = 0;          // Mevcut okuma konumu (0 = başlangıç)
    std::vector<int> offsetMap; // Backtracking yığını

    // --- Pozisyon Yönetimi (Backtracking API) ---
    void beginPosition();    // Şu anki konumu yığına kaydet
    int  getLastPosition();  // Yığındaki son konumu döndür
    void acceptPosition();   // Yığındaki son konumu kalıcı yap (apply)
    void setLastPosition(int n); // Yığındaki son konumu n olarak değiştir
    void rejectPosition();   // Yığındaki son konumu at (discard)

    // --- Dosya Sonu ve Pozisyon Sorgulama ---
    bool isEnd();            // offset >= size ise true (EOF)
    std::pair<int, int> positionRange(); // [start, end] offset aralığı
    std::string getPositionRange(); // Pozisyon aralığındaki metni döndür

    // --- Desen Eşleme ---
    // `word` mevcut konumda başlıyorsa true; consume true ise üstünden geçer.
    bool tryConsume(std::string_view word, bool consume = true);

    // --- Konum Okuma/Yazma ---
    int  getOffset();        // Mevcut offset'i döndür
    int  setOffset(int n);   // Offset'i n olarak ayarla, yeni değeri döndür

    // --- Karakter Okuma ---
    char getchar(int additionalOffset); // offset + ek'teki karakteri oku
    char getchar();          // Mevcut offset'teki karakteri oku
    void nextChar();         // offset'i 1 ilerlet (EOF kontrolü yapar)
    void toChar(int n);      // offset'i n kadar ilerlet

    // --- Üst Seviye İşlemler ---
    SourceFile sourceFile;             // Kaynak kod ve satır başı offset'leri
    SourceLocation getLocation();      // Mevcut offset'in SourceLocation'ını döndür
    void setSourceText(const std::string& path, const std::string& text);

    void setText(std::string input); // Yeni kaynak kodu yükle
    void skipWhiteSpace();   // Boşluk/sekme/satırsonu karakterlerini atla
    bool isNumeric();        // Mevcut karakter 0-9 aralığında mı?
    INumber readNumeric();   // Sayı literal'ı oku ve INumber olarak döndür
};

#endif // SAQUT_LEXER
