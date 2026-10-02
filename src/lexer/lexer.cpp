// ============================================================================
// saQut Compiler — Lexer (Karakter Seviyesinde Tarayıcı) Gerçeklemesi
// ============================================================================
//
// DİZİN:   src/lexer/lexer.cpp
// KATMAN:  Katman 1 — Derleyici pipeline'ının ilk aşaması
// BAĞIMLI: lexer/lexer.hpp
//
// AMAÇ:
//   Ham kaynak kodunu karakter karakter işleyerek Tokenizer'a temel okuma,
//   konumlandırma, backtracking ve sayı-okuma hizmetleri sunar. Tüm Lexer
//   metodlarının gövdeleri bu dosyadadır.
//
// ============================================================================

#include "lexer/lexer.hpp"

// --------------------------------------------------------------------------
// beginPosition: Mevcut offset'i yığına kaydet.
// İç içe çağrılabilir: 3 kere beginPosition → 3 elemanlı yığın.
// --------------------------------------------------------------------------
void Lexer::beginPosition() {
    offsetMap.push_back(getLastPosition());
}

// --------------------------------------------------------------------------
// getLastPosition: Yığının tepesindeki konumu döndür.
// Yığın boşsa mevcut offset'i döndür (başlangıç durumu).
// --------------------------------------------------------------------------
int Lexer::getLastPosition() {
    if (offsetMap.empty()) return offset;
    return offsetMap.back();
}

// --------------------------------------------------------------------------
// acceptPosition: Yığındaki son geçici konumu kalıcı yap — ve YIĞINDAN ÇIKAR.
//
// beginPosition() bir giriş push'lar; accept ya da reject o girişi tüketmek
// zorundadır, aksi halde yığın token başına bir eleman büyür. (Eski hal
// pop yapmadan offsetMap.back()'i kendi üstüne yazıyordu: hem no-op hem
// sızıntı — 21040 token'lık girdide yığın 8481 elemana çıkıyordu.)
//
// Örnek: offsetMap=[5,10] → pop(10), yeni tepe 10 olur; yani iç tarama
// sırasında ilerletilen konum çağırana devredilir.
// --------------------------------------------------------------------------
void Lexer::acceptPosition() {
    int t = offsetMap.back();
    offsetMap.pop_back();
    setLastPosition(t);
}

// --------------------------------------------------------------------------
// setLastPosition: Yığının tepesini veya offset'i değiştir.
// --------------------------------------------------------------------------
void Lexer::setLastPosition(int n) {
    if (offsetMap.empty())
        offset = n;
    else
        offsetMap.back() = n;
}

// --------------------------------------------------------------------------
// isEnd: Dosya sonuna gelindi mi? offset >= size.
// --------------------------------------------------------------------------
bool Lexer::isEnd() {
    return size <= getOffset();
}

// --------------------------------------------------------------------------
// rejectPosition: Yığındaki son konumu at. Başarısız tryConsume() denemesi sonrası.
// --------------------------------------------------------------------------
void Lexer::rejectPosition() {
    offsetMap.pop_back();
}

// --------------------------------------------------------------------------
// positionRange: Yığındaki en dış ve en iç konumu [start, end] olarak döndür.
// --------------------------------------------------------------------------
std::pair<int, int> Lexer::positionRange() {
    int len = offsetMap.size();
    if (len == 0)
        return {0, offset};
    if (len == 1)
        return {offset, offsetMap[0]};
    return {offsetMap[len - 2], offsetMap[len - 1]};
}

// --------------------------------------------------------------------------
// getPositionRange: positionRange() aralığındaki metni string olarak döndür.
// --------------------------------------------------------------------------
std::string Lexer::getPositionRange() {
    auto [start, end] = positionRange();
    std::string mem;
    for (int i = start; i < end; i++)
        mem.push_back(input.at(i));
    return mem;
}

// --------------------------------------------------------------------------
// tryConsume: Belirtilen kelime mevcut konumda başlıyor mu? Başlıyorsa ve
// consume true ise konum kelimenin sonuna ilerler; aksi halde konum değişmez.
// --------------------------------------------------------------------------
bool Lexer::tryConsume(std::string_view word, bool consume) {
    beginPosition();
    for (size_t i = 0; i < word.size(); i++) {
        if (isEnd()) {
            rejectPosition();
            return false;
        }
        if (word[i] != getchar()) {
            rejectPosition();
            return false;
        }
        nextChar();
    }
    if (consume)
        acceptPosition();
    else
        rejectPosition();
    return true;
}

// --------------------------------------------------------------------------
// getOffset / setOffset: Konum erişimcileri.
// --------------------------------------------------------------------------
int Lexer::getOffset() {
    return getLastPosition();
}

int Lexer::setOffset(int n) {
    setLastPosition(n);
    return getLastPosition();
}

// --------------------------------------------------------------------------
// getchar(additionalOffset): offset + ek kadar ilerideki karakteri oku.
// --------------------------------------------------------------------------
char Lexer::getchar(int additionalOffset) {
    int target = getOffset() + additionalOffset;
    if (target >= size) return '\0';
    return input.at(target);
}

char Lexer::getchar() {
    int target = getOffset();
    if (target >= size) return '\0';
    return input.at(target);
}

// --------------------------------------------------------------------------
// nextChar / toChar: Konum ilerletme.
// --------------------------------------------------------------------------
void Lexer::nextChar() {
    if (!isEnd())
        setOffset(getOffset() + 1);
}

void Lexer::toChar(int n) {
    if (!isEnd())
        setOffset(getOffset() + n);
}

// --------------------------------------------------------------------------
// getLocation: Mevcut offset'in SourceLocation'ını döndür.
// --------------------------------------------------------------------------
SourceLocation Lexer::getLocation() {
    return sourceFile.offsetToLocation(getOffset());
}

// --------------------------------------------------------------------------
// setSourceText: Yeni kaynak kodu yükle ve SourceFile'ı güncelle.
// --------------------------------------------------------------------------
void Lexer::setSourceText(const std::string& path, const std::string& text) {
    sourceFile.setText(path, text);
    setText(text);
}

// --------------------------------------------------------------------------
// setText: Yeni kaynak kodu yükle. input ve size'ı günceller.
// --------------------------------------------------------------------------
void Lexer::setText(std::string text) {
    input = text;
    size  = static_cast<int>(text.length());
}

// --------------------------------------------------------------------------
// skipWhiteSpace: Boşluk, sekme, satırsonu, satırbaşı karakterlerini atla.
// --------------------------------------------------------------------------
void Lexer::skipWhiteSpace() {
    while (!isEnd()) {
        switch (getchar()) {
            case '\r':  // carriage return (Windows satırsonu \r\n)
            case '\n':  // line feed (Unix satırsonu)
            case '\b':  // backspace
            case '\t':  // tab
            case ' ':   // boşluk
                nextChar();
                break;
            default:
                return;
        }
    }
}

// --------------------------------------------------------------------------
// isNumeric: Mevcut karakter bir rakam mı? (0-9)
// --------------------------------------------------------------------------
bool Lexer::isNumeric() {
    char c = getchar();
    return (c >= '0' && c <= '9');
}

// --------------------------------------------------------------------------
// readNumeric: Tam bir sayı literal'ı oku.
// --------------------------------------------------------------------------
INumber Lexer::readNumeric() {
    INumber num;
    num.start = getLastPosition();
    num.startLoc = getLocation();

    // --- Adım 1: İsteğe bağlı işaret ---
    if (getchar() == '-') {
        nextChar();
        num.positive = false;
    } else if (getchar() == '+') {
        nextChar();
        num.positive = true;
    } else {
        num.positive = true;
    }

    // --- Adım 2: İlk karakter '0' ise özel format kontrolü ---
    bool nextDot = false;
    if (getchar() == '0') {
        num.token.push_back('0');
        nextChar();
        char c = getchar();
        switch (c) {
            case 'x': case 'X':       // Hex: 0xFF, 0X1A
                num.token.push_back(c);
                num.base = 16;
                nextChar();
                break;
            case 'b': case 'B':       // Binary: 0b1010
                num.token.push_back(c);
                num.base = 2;
                nextChar();
                break;
            case '.':                 // Float: 0.5, 0.0
                num.token.push_back(c);
                num.base   = 10;
                nextDot    = true;
                num.isFloat = true;
                nextChar();
                break;
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7':
                // Octal: 0777 — sonraki karakter octal rakam ise devam et
                num.base = 8;
                break;
            default:
                num.end = getLastPosition();
                num.endLoc = getLocation();
                return num;
        }
    } else {
        num.base = 10;
    }

    // --- Adım 3: Ana okuma döngüsü ---
    while (!isEnd()) {
        char c = getchar();
        switch (c) {
            case '0':
            case '1':
                num.token.push_back(c);
                break;
            case '2': case '3': case '4': case '5':
            case '6': case '7':
                if (num.base >= 8)
                    num.token.push_back(c);
                else {
                    num.end = getLastPosition();
                    num.endLoc = getLocation();
                    return num;
                }
                break;
            case '8': case '9':
                if (num.base >= 10)
                    num.token.push_back(c);
                else {
                    num.end = getLastPosition();
                    num.endLoc = getLocation();
                    return num;
                }
                break;
            case 'a': case 'A': case 'b': case 'B':
            case 'c': case 'C': case 'd': case 'D':
            case 'f': case 'F':
                if (num.base >= 16)
                    num.token.push_back(c);
                else {
                    num.end = getLastPosition();
                    num.endLoc = getLocation();
                    return num;
                }
                break;
            case '.':
                if (!nextDot) {
                    if (num.token.empty())
                        num.token += "0.";
                    else
                        num.token.push_back('.');
                    nextDot    = true;
                    num.isFloat = true;
                } else {
                    num.end = getLastPosition();
                    num.endLoc = getLocation();
                    return num;
                }
                break;
            case 'e': case 'E':
                if (num.base == 16) {
                    num.token.push_back(c);
                    break;
                }
                if (num.base == 10) {
                    num.hasEpsilon = true;
                    num.token.push_back(c);
                    nextChar();
                    c = getchar();
                    if (c == '+' || c == '-') {
                        num.token.push_back(c);
                        nextChar();
                    }
                    while (!isEnd()) {
                        c = getchar();
                        if (c >= '0' && c <= '9') {
                            num.token.push_back(c);
                            nextChar();
                        } else {
                            num.end = getLastPosition();
                            num.endLoc = getLocation();
                            return num;
                        }
                    }
                    break;
                }
                num.end = getLastPosition();
                num.endLoc = getLocation();
                return num;
            default:
                num.end = getLastPosition();
                return num;
        }
        nextChar();
    }
    num.end = getLastPosition();
    num.endLoc = getLocation();
    return num;
}
