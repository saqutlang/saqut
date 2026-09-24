// ============================================================================
// saQut Compiler — Parser Token Tipleri, Operatör Öncelik Tablosu ve ParserToken
// ============================================================================
//
// DİZİN:   src/parser/token.hpp
// KATMAN:  Katman 3 — Tokenizer ile Parser Arasında Köprü
// AMAÇ:    Tokenizer'ın ham token'larını anlamsal tiplere dönüştürmek,
//          operatör öncelik ve birleşme kurallarını merkezi olarak tanımlamak
//
// BAĞIMLILIKLAR:
//   - tokenizer/tokenizer.hpp: Token sınıf hiyerarşisi (Token, NumberToken, vs.)
//   - KULLANAN: parser/ast.hpp, parser/parser.hpp
//
// BU DOSYANIN İÇERDİKLERİ:
//   1. TokenType enum (uint16_t): 100+ token tipi (keyword'ler, operatörler, delimiter'lar)
//   2. KEYWORD_MAP:        string → TokenType  (keyword çözümleme)
//   3. OPERATOR_MAP:       string → TokenType  (operatör çözümleme)
//   4. OPERATOR_MAP_REV:   TokenType → string  (log çıktısı için ters harita)
//   5. OPERATOR_MAP_STRREV: TokenType → string (enum ismi, debug için)
//   6. TokenPrecedence():  Öncelik tablosu (18 seviye, Pratt parser'ın kalbi)
//   7. RightAssociative(): Sağ birleşme kontrolü (atama, üs)
//   8. ParserToken:        Parser'ın kullandığı token yapısı (Token* + TokenType)
//
// TASARIM KARARLARI (ADR-002):
//   Neden TokenType enum'ı burada tanımlı, Tokenizer'da değil?
//   -> Tokenizer sadece ham token'lar üretir. Anlamsal tipler Parser'ın işidir.
//   -> Tokenizer'ın Tokenizer'ın ham yapısını değiştirmeden yeni diller eklenebilir.
//
//   Neden uint16_t tabanlı enum?
//   -> 65K token tipi fazlasıyla yeterli. 2 byte = bellek tasarrufu.
//   -> Her AST düğümünde TokenType saklanabilir (opsiyonel).
//
//   Neden dört ayrı map?
//   -> unordered_map tek yönlüdür. Her yön için ayrı map gerekir.
//   -> OPERATOR_MAP_REV: log çıktısında "+" göstermek için.
//   -> OPERATOR_MAP_STRREV: enum ismini string olarak (debug, AST dump).
//
//   Neden bu kadar çok keyword?
//   -> saQut hem C/C++ hem Java hem de kendi sözdizimini destekler.
//   -> Tüm keyword'ler tek enum'da toplanmıştır.
//
// ============================================================================

#ifndef SAQUT_PARSER_TOKEN
#define SAQUT_PARSER_TOKEN

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "tokenizer/tokenizer.hpp"

// ============================================================================
// TokenList — Token Vektörü Tip Kısaltması
// ============================================================================
//
// Tokenizer::scan() tarafından üretilen, Parser::parse() tarafından tüketilen
// token listesi. Ham pointer'lar içerir — bellek yönetimi çağırana aittir.
//
// TODO: std::vector<std::unique_ptr<Token>> ile otomatik bellek yönetimi
//
typedef std::vector<Token*> TokenList;

// ============================================================================
// TokenType — Anlamsal Token Tipleri (Enum)
// ============================================================================
//
// Tokenizer'ın ürettiği string tipli token'ları ("number", "operator", ...)
// Parser'ın anlayacağı anlamsal tiplere dönüştürür.
//
// KATEGORİLER:
//   1. Değerler: IDENTIFIER, NUMBER, STRING, SVR_VOID (geçersiz/EOF)
//   2. Keyword'ler: KW_IF ... KW_NOEXCEPT (alfabetik sıralı)
//   3. Operatörler: Öncelik sırasına göre gruplanmış
//      - Seviye 1:   DOT, ARROW, LBRACKET, RBRACKET, LPAREN, RPAREN
//      - Seviye 2:   PLUS_PLUS, MINUS_MINUS (postfix)
//      - Seviye 3:   PLUS, MINUS, BANG, TILDE (unary prefix)
//      - Seviye 4:   STAR_STAR (üs; ^ artık XOR, Level 8)
//      - Seviye 5:   STAR, SLASH, PERCENT (çarpma/bölme)
//      - Seviye 6-16: devamı...
//   4. Diğer: LBRACE, RBRACE, SEMICOLON, COMMA, COLON_COLON
//   5. Özel: END_OF_FILE, UNKNOWN, COMMENT, PREPROCESSOR
//
// NEDEN uint16_t? Bellek optimizasyonu. Her AST düğümü bir TokenType taşır.
// Binlerce düğümde 2 byte vs 4 byte fark eder.
//
/* ================================================================
 * TokenType — Anlamsal Token Tipleri
 * ================================================================
 *
 * Tokenizer'ın ürettiği ham token'ları (string tipli) Parser'ın
 * anlayacağı anlamsal tiplere dönüştürür.
 *
 * uint16_t tabanlı — 65K token tipi yeterli.
 * Bellek: AST düğümlerinde taşınabilir (2 byte).
 *
 * KATEGORİLER (öncelik sırasına göre):
 *   1. Değerler:        IDENTIFIER, NUMBER, STRING, SVR_VOID
 *   2. Keyword'ler:     KW_IF ... KW_NOEXCEPT (C/C++/Java ortak)
 *   3. Operatörler:     DOT(18) ... COMMA(1) (Pratt öncelik seviyesi)
 *   4. Delimiter'lar:   LBRACE, RBRACE, SEMICOLON, vb.
 *   5. Özel:            END_OF_FILE, UNKNOWN, COMMENT, PREPROCESSOR
 *
 * ================================================================ */
enum class TokenType : uint16_t {
    /* ====== Değerler ve Tanımlayıcılar ====== */
    IDENTIFIER,      // Değişken/fonksiyon/sınıf ismi.
                     //   Tokenizer'da IdentifierToken olarak üretilir.
                     //   Örn: x, main, Point, calculateAverage
    NUMBER,          // Sayısal sabit: 42, 0xFF, 0b1010, 3.14, 1e-5
                     //   Tokenizer'da NumberToken olarak üretilir.
                     //   .isFloat alanı ile tamsayı/ondalık ayrımı yapılır.
    STRING,          // Metin sabiti: "merhaba", "selam"
                     //   Tokenizer'da StringToken olarak üretilir.
                     //   Kaçış dizileri (\n, \t, \") tokenizer'da çözülür.
    SVR_VOID,        // Geçersiz/EOF sinyali.
                     //   Parser içinde kullanılır. Tokenizer ÜRETMEZ.
                     //   currentToken() geçersiz indeks gösterdiğinde döner.

    /* ====== Kontrol Akışı Keyword'leri ====== */
    KW_IF,           // if (koşullu dal)
                     //   Sözdizimi: if (koşul) gövde [else gövde]
    KW_ELSE,         // else (if'in alternatif dalı)
                     //   Sözdizimi: if (...) ... else ...
                     //   Parser'da if'ten sonra else opsiyoneldir.
    KW_FOR,          // for (tekrarlı döngü)
                     //   Sözdizimi: for (init; koşul; artım) gövde
    KW_WHILE,        // while (koşullu döngü)
                     //   Sözdizimi: while (koşul) gövde
    KW_DO,           // do (en az bir kez çalışan döngü)
                     //   Sözdizimi: do gövde while (koşul);
    KW_AS,           // as (tip dönüşümü — ADR-026): expr as int, expr as float?
    KW_SWITCH,       // switch (çoklu dal — ADR-027)
    KW_CASE,         // case (switch dalı — ADR-027)
    KW_DEFAULT,      // default (switch varsayılan — ADR-027)
    KW_BREAK,        // break (döngü/switch'ten çık)
                     //   Sadece döngü veya switch içinde geçerlidir.
    KW_CONTINUE,     // continue (döngünün bir sonraki iterasyonuna geç)
                     //   Sadece döngü içinde geçerlidir.
    KW_RETURN,       // return (fonksiyondan dön)
                     //   İsteğe bağlı dönüş değeri: return expr;

    /* ====== OOP Keyword'leri ====== */
    KW_CLASS,        // class (sınıf tanımı — Java/C++ tarzı)
    KW_STRUCT,       // struct (yapı tanımı — C tarzı)
                     //   saQut'ta class ve struct ikisi de desteklenir.
    KW_INTERFACE,    // interface (soyut tip — Java tarzı)
    KW_ENUM,         // enum (sabit listesi — C/C++/Java tarzı)
    KW_EXTENDS,      // extends (kalıtım — Java tarzı)
    KW_IMPLEMENTS,   // implements (interface gerçekleme — Java tarzı)
    KW_NEW,          // new (nesne oluşturma — Java/C++ tarzı)
    KW_PUBLIC,       // public (erişim belirteci)
    KW_PRIVATE,      // private (erişim belirteci)
    KW_PROTECTED,    // protected (erişim belirteci — alt sınıflara açık)
    KW_STATIC,       // static (sınıf üyesi / dosya içi bağlantı)
    KW_FINAL,        // final (değiştirilemez — Java tarzı)
    KW_ABSTRACT,     // abstract (soyut sınıf/metot — Java tarzı)

    /* ====== Tip Keyword'leri ====== */
    KW_VOID,         // void (değer döndürmeyen fonksiyon / tip yok)
                     //   C/C++/Java uyumluluğu için.
    KW_BOOL,         // bool (mantıksal tip: true/false)
                     //   C++ bool ile aynı.
    KW_INT,          // int (tamsayı tipi: 32-bit işaretli)
                     //   Varsayılan tamsayı tipi.
    KW_FLOAT_TYPE,   // float (32-bit ondalıklı sayı)
                     //   FLOAT_MATH hatasından kaçınmak için _TYPE eki.
                     //   math.h'deki float tanımıyla çakışmaz.
    KW_DOUBLE,       // double (64-bit ondalıklı sayı)
    KW_CHAR,         // char (8-bit karakter)
                     //   Tek tırnak içindeki karakterler için: 'A'
    KW_STRING_TYPE,  // string (metin tipi)
    KW_DECIMAL,      // decimal (ondalık hassasiyet — ADR-028)
                     //   string.h'daki string işlevleriyle çakışmaz.
    KW_BYTE,         // byte (8-bit işaretsiz değer tipi 0-255 — #86)
    KW_DATE,         // date (UTC epoch-ms değer tipi — #88, ADR-035)

    /* ====== Literal Keyword'ler ====== */
    KW_TRUE,         // true (mantıksal doğru sabiti)
                     //   Boolean literal: if (true) { ... }
    KW_FALSE,        // false (mantıksal yanlış sabiti)
                     //   Boolean literal: while (false) { ... }
    KW_NULL,         // null (boş referans sabiti)
                     //   Pointer/referans tipleri için: Object obj = null;

    /* ====== İstisna Yönetimi ====== */
    KW_TRY,          // try (istisna deneme bloğu — Java/C++ tarzı)
                     //   Sözdizimi: try { ... } catch (Ex e) { ... }
    KW_CATCH,        // catch (istisna yakalama bloğu)
    KW_FINALLY,      // finally (her durumda çalışan blok — Java tarzı)
    KW_THROW,        // throw (istisna fırlatma — C++/Java tarzı)
                     //   Sözdizimi: throw new Exception("hata");
    KW_THROWS,       // throws (metot imzasında istisna bildirimi — Java)
    KW_ASSERT,       // assert (debug assertions — C/Java tarzı)

    /* ====== Modül/Paket ====== */
    KW_IMPORT,       // import (modül içe aktarma)
                     //   Sözdizimi: import {add, Vector} from "math.sqt";
    KW_EXPORT,       // export (sembol dışa aktarma — fonksiyon/struct/enum)
                     //   Sözdizimi: export void add(int a, int b) { ... }
    KW_PACKAGE,      // package (modül bildirimi — Java tarzı)
                     //   Sözdizimi: package com.saqut.compiler;

    /* ====== C/C++ Ekleri ====== */
    KW_NATIVE,       // native (yerel kod bildirimi — JNI)
                     //   Java native metotları için.
    KW_SYNCHRONIZED, // synchronized (iş parçacığı senkronizasyonu — Java)
    KW_VOLATILE,     // volatile (derleyici optimizasyonunu engelle — C/C++/Java)
    KW_TRANSIENT,    // transient (serileştirmeyi atla — Java)
    KW_CONST,        // const (değişmez değer — C/C++ tarzı)
                     //   Örn: const int MAX = 100;
    KW_EXTERN,       // extern (harici bağlantı — C/C++ tarzı)
    KW_FFI,          // ffi (gömülü host fonksiyon bildirimi — ADR-034, #107)
    KW_TYPEDEF,      // typedef (tip takma adı — C/C++ tarzı)
    KW_SIZEOF,       // sizeof (tip/boyut sorgulama — C/C++ tarzı)
                     //   Sözdizimi: sizeof(int) veya sizeof x
    KW_ALIGNOF,      // alignof (hizalama sorgulama — C++11)
    KW_DECLTYPE,     // decltype (ifade tipi çıkarımı — C++11)
    KW_AUTO,         // auto (otomatik tip çıkarımı — C++11)
    KW_CONSTEXPR,    // constexpr (derleme zamanı sabiti — C++11)
    KW_NOEXCEPT,     // noexcept (istisna fırlatmayan bildirimi — C++11)

    /* ====== İzole thread modeli (ADR-045 Faz 3) ====== */
    KW_SHARED,       // shared (global niteleyici: thread'ler arası görünür)
    KW_LOCK,         // lock a; / lock a, b;  (kapsam sonunda otomatik unlock)
    KW_UNLOCK,       // unlock a;
    KW_WAIT,         // wait(koşul);
    KW_THREAD,       // thread { gövde }  (ifade; tipi Thread)
    KW_POOL,         // Pool (tip) / Pool(T) (intrinsic, tip argümanlı)
    KW_LIST,         // List (tip) / List(T) (intrinsic, tip argümanlı)
    KW_THREAD_TYPE,  // Thread (tip — ThreadTable id'si)

    /* ================================================================
     * Operatörler — Öncelik sırasına göre gruplanmış
     *
     * Her operatörün yanında Pratt parser öncelik seviyesi yazılıdır.
     * Yüksek sayı = daha sıkı bağlanma (önce işlenir).
     *
     * Seviye 18 (en yüksek): Üye erişimi ve çağrı
     * Seviye 17:             Postfix ++ --
     * Seviye 16:             Unary prefix + - ! ~
     * Seviye 15:             Üs alma ** ^
     * Seviye 14:             Çarpma/Bölme * / %
     * Seviye 13:             Toplama/Çıkarma + -
     * Seviye 12:             Bitsel kaydırma << >>
     * Seviye 11:             İlişkisel < <= > >=
     * Seviye 10:             Eşitlik == !=
     * Seviye 9:              Bitsel VE &
     * Seviye 8:              Bitsel XOR ^ (#230; C/Python ile hizalı)
     * Seviye 7:              Bitsel VEYA |
     * Seviye 6:              Mantıksal VE &&
     * Seviye 5:              Mantıksal VEYA ||
     * Seviye 4:              `?` (nullable tip işareti; ternary YOK)
     * Seviye 3:              `:` (etiket; ternary else YOK)
     * Seviye 2:              Atama = += -= vb.
     * Seviye 1 (en düşük):  Virgül ,
     * ================================================================ */

    // Seviye 18: Üye erişimi ve çağrı — En yüksek öncelik
    DOT,             // . (üye erişimi) — obj.field
                     //   Öncelik 18. En sıkı bağlanan operatör.
    ARROW,           // -> (pointer üye erişimi) — ptr->field
                     //   C++ tarzı. Öncelik 18.
    LBRACKET,        // [ (dizi/indeks erişimi başlangıcı) — a[i]
                     //   Açılış köşeli parantez. Öncelik 18.
    RBRACKET,        // ] (dizi/indeks erişimi bitişi) — a[i]
                     //   Kapanış köşeli parantez. Tek başına kullanılmaz.
    LPAREN,          // ( (fonksiyon çağrısı/grouping başlangıcı)
                     //   İki anlamı: f(args) çağrı, (expr) gruplama.
                     //   Öncelik 18.
    RPAREN,          // ) (fonksiyon çağrısı/grouping bitişi)
                     //   Kapanış parantez. Tek başına kullanılmaz.

    // Seviye 17: Postfix — Soldaki ifadeye sonradan uygulanan operatörler
    PLUS_PLUS,       // ++ (postfix artım) — x++
                     //   Önce x'in değerini döndür, sonra artır.
                     //   Öncelik 17. Sağ birleşmeli DEĞİL.
    MINUS_MINUS,     // -- (postfix azaltım) — x--
                     //   Önce x'in değerini döndür, sonra azalt.
                     //   Öncelik 17.

    // Seviye 16: Unary Prefix — Sağındaki ifadeye uygulanan tekli operatörler
    PLUS,            // + (unary plus / binary toplama)
                     //   Unary: +x (pozitif işareti, genelde etkisiz).
                     //   Binary: a + b (toplama, öncelik 13).
                     //   Hangi anlamda kullanıldığı parse bağlamında belirlenir.
    MINUS,           // - (unary minus / binary çıkarma)
                     //   Unary: -x (negatif yap).
                     //   Binary: a - b (çıkarma, öncelik 13).
    BANG,            // ! (mantıksal değil) — !x
                     //   Örn: if (!flag) { ... }
                     //   Sadece prefix. Öncelik 16.
    TILDE,           // ~ (bitsel değil) — ~x
                     //   Bitwise NOT. Sadece prefix. Öncelik 16.

    // Seviye 15: Üs alma — Sağ birleşmeli
    STAR_STAR,       // ** (üs alma) — a ** b = a^b
                     //   Python tarzı. Öncelik 15. Sağ birleşmeli.
                     //   2 ** 3 ** 2 = 2 ** (3 ** 2) = 512
    CARET,           // ^ (bitsel XOR)
                     //   Öncelik 8 (Level 8). Sol birleşmeli. #230 kararı.
                     //   Üs yalnız ** (STAR_STAR) iledir.

    // Seviye 14: Çarpma/Bölme — Sol birleşmeli
    STAR,            // * (çarpma) — a * b
                     //   Öncelik 14.
    SLASH,           // / (bölme) — a / b
                     //   Tamsayı bölmesi: int / int = int.
                     //   Ondalık bölme: float / float = float.
    PERCENT,         // % (mod alma) — a % b
                     //   Sadece tamsayılar için.

    // Seviye 13: Toplama/Çıkarma
    //   PLUS ve MINUS yukarıda tanımlandı (hem unary 16 hem binary 13).
    //   Pratt parser bağlama göre doğru önceliği kullanır.

    // Seviye 12: Bitsel kaydırma
    LSHIFT,          // << (sola kaydırma) — a << b
                     //   a * 2^b. Öncelik 12.
    RSHIFT,          // >> (sağa kaydırma) — a >> b
                     //   a / 2^b (işaretli: arithmetic, işaretsiz: logical).

    // Seviye 11: İlişkisel karşılaştırma
    LESS,            // < (küçüktür) — a < b
                     //   true/false döndürür.
    LESS_EQUAL,      // <= (küçük eşittir) — a <= b
    GREATER,         // > (büyüktür) — a > b
    GREATER_EQUAL,   // >= (büyük eşittir) — a >= b

    // Seviye 10: Eşitlik
    EQUAL_EQUAL,     // == (eşittir) — a == b
                     //   Değer eşitliği. Öncelik 10.
    BANG_EQUAL,      // != (eşit değildir) — a != b

    // Seviye 9: Bitsel VE
    AMPERSAND,       // & (bitsel VE) — a & b
                     //   Bitwise AND. Öncelik 9.

    // Seviye 8: Bitsel XOR — CARET, &  (9) ile | (7) arasındadır (#230)

    // Seviye 7: Bitsel VEYA
    PIPE,            // | (bitsel VEYA) — a | b
                     //   Bitwise OR. Öncelik 7.

    // Seviye 6: Mantıksal VE
    AMPERSAND_AMPERSAND, // && (mantıksal VE) — a && b
                         //   Kısa devre (short-circuit): a false ise b değerlendirilmez.
                         //   Öncelik 6.

    // Seviye 5: Mantıksal VEYA
    PIPE_PIPE,       // || (mantıksal VEYA) — a || b
                     //   Kısa devre: a true ise b değerlendirilmez.
                     //   Öncelik 5.

    // Seviye 4: `?` — ternary DESTEKLENMİYOR (ürün kararı, 2026-08-14).
    //   `?` bugün yalnızca nullable tip işaretidir (`int?`, `Point?` —
    //   parser isNullable kontrolü); ifade konumunda kullanılamaz.
    TERNARY,         // ? — nullable tip işareti (a ? b : c sözdizimi YOK)
                     //   Token sınıfı öncelik tablosunda 4 seviyesinde
                     //   kalır; dilde bu önceliği kullanan kural yoktur.
    COLON,           // : (etiket) — ternary else DEĞİL (ternary yok)

    // Seviye 2: Atama — Sağ birleşmeli
    EQUAL,           // = (basit atama) — a = b
                     //   Öncelik 2. Sağ birleşmeli.
                     //   a = b = 5 = a = (b = 5)
    PLUS_EQUAL,      // += (topla ve ata) — a += b → a = a + b
    MINUS_EQUAL,     // -= (çıkar ve ata) — a -= b → a = a - b
    STAR_EQUAL,      // *= (çarp ve ata) — a *= b → a = a * b
    SLASH_EQUAL,     // /= (böl ve ata) — a /= b → a = a / b
    PERCENT_EQUAL,   // %= (mod al ve ata) — a %= b → a = a % b
    AMPERSAND_EQUAL, // &= (bitsel VE ve ata) — a &= b → a = a & b
    PIPE_EQUAL,      // |= (bitsel VEYA ve ata) — a |= b → a = a | b
    CARET_EQUAL,     // ^= (XOR ve ata) — a ^= b → a = a ^ b
    LSHIFT_EQUAL,    // <<= (sola kaydır ve ata) — a <<= b → a = a << b
    RSHIFT_EQUAL,    // >>= (sağa kaydır ve ata) — a >>= b → a = a >> b

    /* ====== Diğer Semboller ====== */
    LBRACE,          // { (açılış süslü parantez) — blok başlangıcı
                     //   Sözdizimi: { statement1; statement2; }
    RBRACE,          // } (kapanış süslü parantez) — blok bitişi
    SEMICOLON,       // ; (noktalı virgül) — ifade sonu belirteci
                     //   C/C++/Java tarzında her ifade ; ile biter.
    COMMA,           // , (virgül) — ifade ayırıcı
                     //   Örn: int a, b, c; veya f(1, 2, 3)
                     //   Öncelik 1 (en düşük).
    COLON_COLON,     // :: (kapsam çözümleme) — Class::method
                     //   C++ tarzı. Şu anda sadece token tanımlı, parse yok.

    /* ====== Özel Token'lar ====== */
    END_OF_FILE,     // Dosya sonu belirteci.
                     //   Tokenizer dosya sonuna gelindiğinde üretir.
                     //   Parser'ın durma koşuludur.
    UNKNOWN,         // Bilinmeyen/tanınamayan karakter.
                     //   Tokenizer'ın çözemediği her şey.
                     //   Hata raporlamada kullanılır.
    COMMENT,         // Yorum token'ı (// veya /* */).
                     //   ŞU ANDA TOKEN ÜRETİLMEZ — tokenizer yorumları atlar.
                     //   Gelecekte belge yorumları (///, /** */) için kullanılabilir.
    PREPROCESSOR,    // Önişlemci direktifi (#).
                     //   ŞU ANDA KULLANILMIYOR — C önişlemcisi yok.
                     //   Gelecekte #include, #define için.
};

// ============================================================================
// KEYWORD_MAP — Keyword String → TokenType Dönüşüm Haritası
// ============================================================================
//
// AMAÇ: Tokenizer'ın ürettiği KeywordToken'ların token değerini (örn: "if")
//       Parser'ın anlayacağı TokenType'a (KW_IF) dönüştürür.
//
// ANAHTAR: std::string_view — keyword string'i (kopyalanmaz, salt okunur)
// DEĞER:   TokenType — Parser'ın anlayacağı anlamsal tip
//
// VERİ YAPISI: std::unordered_map<string_view, TokenType>
//   - O(1) ortalama arama süresi
//   - constexpr: derleme zamanı sabiti (derleyici tabloya gömer)
//   - std::string_view: string kopyalamadan kaçınır (performans)
//
// BOYUT: ~60 girdi (tüm keyword'ler)
// NEDEN unordered_map, neden map değil?
//   - Arama sıklığı: her token için bir kez
//   - unordered_map O(1) vs map O(log n) — fark küçük ama var
//   - Sıralı erişim gerekmez
//
// SENKRONİZASYON UYARISI:
//   Bu harita, Tokenizer'daki keywords[] dizisi İLE EŞLEŞMELİDİR.
//   Birinde ekleme yapılırsa diğerine de eklenmelidir.
//   TODO: İki listeyi ortak bir kaynaktan üretecek bir makro/kod üreteci.
//
inline const std::unordered_map<std::string_view, TokenType> KEYWORD_MAP = {
    // --- Tip dönüşümü (ADR-026) ---
    {"as",          TokenType::KW_AS},

    // --- Control flow ---
    {"if",          TokenType::KW_IF},
    {"else",        TokenType::KW_ELSE},
    {"for",         TokenType::KW_FOR},
    {"while",       TokenType::KW_WHILE},
    {"do",          TokenType::KW_DO},
    {"switch",      TokenType::KW_SWITCH},
    {"case",        TokenType::KW_CASE},
    {"default",     TokenType::KW_DEFAULT},
    {"break",       TokenType::KW_BREAK},
    {"continue",    TokenType::KW_CONTINUE},
    {"return",      TokenType::KW_RETURN},

    // --- OOP ---
    {"class",       TokenType::KW_CLASS},
    {"struct",      TokenType::KW_STRUCT},
    {"interface",   TokenType::KW_INTERFACE},
    {"enum",        TokenType::KW_ENUM},
    {"extends",     TokenType::KW_EXTENDS},
    {"implements",  TokenType::KW_IMPLEMENTS},
    {"new",         TokenType::KW_NEW},

    // --- Access modifiers ---
    {"public",      TokenType::KW_PUBLIC},
    {"private",     TokenType::KW_PRIVATE},
    {"protected",   TokenType::KW_PROTECTED},
    {"static",      TokenType::KW_STATIC},
    {"final",       TokenType::KW_FINAL},
    {"abstract",    TokenType::KW_ABSTRACT},

    // --- Types ---
    {"void",        TokenType::KW_VOID},
    {"bool",        TokenType::KW_BOOL},
    {"int",         TokenType::KW_INT},
    {"float",       TokenType::KW_FLOAT_TYPE},
    {"double",      TokenType::KW_DOUBLE},
    {"char",        TokenType::KW_CHAR},
    {"string",      TokenType::KW_STRING_TYPE},
    {"decimal",     TokenType::KW_DECIMAL},
    {"byte",        TokenType::KW_BYTE},
    {"date",        TokenType::KW_DATE},

    // --- Literals ---
    {"true",        TokenType::KW_TRUE},
    {"false",       TokenType::KW_FALSE},
    {"null",        TokenType::KW_NULL},

    // --- Exception handling ---
    {"try",         TokenType::KW_TRY},
    {"catch",       TokenType::KW_CATCH},
    {"finally",     TokenType::KW_FINALLY},
    {"throw",       TokenType::KW_THROW},
    {"throws",      TokenType::KW_THROWS},
    {"assert",      TokenType::KW_ASSERT},

    // --- Modules/packages ---
    {"import",      TokenType::KW_IMPORT},
    {"export",      TokenType::KW_EXPORT},
    {"package",     TokenType::KW_PACKAGE},

    // --- C/C++ specific ---
    {"const",       TokenType::KW_CONST},
    {"extern",      TokenType::KW_EXTERN},
    {"ffi",         TokenType::KW_FFI},
    {"typedef",     TokenType::KW_TYPEDEF},
    {"sizeof",      TokenType::KW_SIZEOF},
    {"auto",        TokenType::KW_AUTO},
    {"constexpr",   TokenType::KW_CONSTEXPR},
    {"noexcept",    TokenType::KW_NOEXCEPT},
    {"native",      TokenType::KW_NATIVE},
    {"synchronized",TokenType::KW_SYNCHRONIZED},
    {"volatile",    TokenType::KW_VOLATILE},
    {"transient",   TokenType::KW_TRANSIENT},

    // --- İzole thread modeli (ADR-045 Faz 3) ---
    {"shared",      TokenType::KW_SHARED},
    {"lock",        TokenType::KW_LOCK},
    {"unlock",      TokenType::KW_UNLOCK},
    {"wait",        TokenType::KW_WAIT},
    {"thread",      TokenType::KW_THREAD},
    {"Pool",        TokenType::KW_POOL},
    {"List",        TokenType::KW_LIST},
    {"Thread",      TokenType::KW_THREAD_TYPE},
};

// ============================================================================
// OPERATOR_MAP — Operatör/Delimiter String → TokenType Dönüşüm Haritası
// ============================================================================
//
// AMAÇ: Tokenizer'ın ürettiği OperatorToken ve DelimiterToken'ları TokenType'a
//       dönüştürür. Her iki token tipi de aynı haritayı kullanır çünkü
//       Parser seviyesinde delimiter'lar da operatör gibi işlenir.
//
// ANAHTAR: std::string_view — operatör/delimiter string'i (örn: "+", "->", "{")
// DEĞER:   TokenType — Parser'ın anlayacağı anlamsal tip
//
// VERİ YAPISI: std::unordered_map<string_view, TokenType>
//   - O(1) ortalama arama
//   - const: derleme zamanı sabiti
//   - Boyut: ~40 girdi
//
// NEDEN İKİ AYRI HARİTA DEĞİL (operator + delimiter)?
//   - Parser seviyesinde fark yok: {, }, ; hepsi operatör gibi işlenir.
//   - Tek harita = tek arama = daha basit kod.
//
// SIRALAMA UYARISI:
//   Bu haritada sıralama önemli DEĞİL (unordered_map).
//   ANCAK Tokenizer'daki operators[] ve delimiters[] dizilerindeki
//   sıralama ÖNEMLİDİR — çok karakterliler önce gelmelidir!
//   Örn: "->" önce, "-" sonra kontrol edilmelidir.
//
inline const std::unordered_map<std::string_view, TokenType> OPERATOR_MAP = {
    // --- 2 karakterli ---
    {"->",  TokenType::ARROW},
    {"::",  TokenType::COLON_COLON},
    {"==",  TokenType::EQUAL_EQUAL},
    {"!=",  TokenType::BANG_EQUAL},
    {"<=",  TokenType::LESS_EQUAL},
    {">=",  TokenType::GREATER_EQUAL},
    {"&&",  TokenType::AMPERSAND_AMPERSAND},
    {"||",  TokenType::PIPE_PIPE},
    {"++",  TokenType::PLUS_PLUS},
    {"--",  TokenType::MINUS_MINUS},
    {"<<",  TokenType::LSHIFT},
    {">>",  TokenType::RSHIFT},
    {"**",  TokenType::STAR_STAR},

    // --- Birleşik atama ---
    {"+=",  TokenType::PLUS_EQUAL},
    {"-=",  TokenType::MINUS_EQUAL},
    {"*=",  TokenType::STAR_EQUAL},
    {"/=",  TokenType::SLASH_EQUAL},
    {"%=",  TokenType::PERCENT_EQUAL},
    {"&=",  TokenType::AMPERSAND_EQUAL},
    {"|=",  TokenType::PIPE_EQUAL},
    {"^=",  TokenType::CARET_EQUAL},
    {"<<=", TokenType::LSHIFT_EQUAL},
    {">>=", TokenType::RSHIFT_EQUAL},

    // --- 1 karakterli operatörler ---
    {"+",   TokenType::PLUS},
    {"-",   TokenType::MINUS},
    {"*",   TokenType::STAR},
    {"/",   TokenType::SLASH},
    {"%",   TokenType::PERCENT},
    {"<",   TokenType::LESS},
    {">",   TokenType::GREATER},
    {"^",   TokenType::CARET},
    {"!",   TokenType::BANG},
    {"~",   TokenType::TILDE},
    {"&",   TokenType::AMPERSAND},
    {"|",   TokenType::PIPE},
    {"=",   TokenType::EQUAL},

    // --- Delimiter'lar (operatör gibi işlenir) ---
    {"[",   TokenType::LBRACKET},
    {"]",   TokenType::RBRACKET},
    {"(",   TokenType::LPAREN},
    {")",   TokenType::RPAREN},
    {"{",   TokenType::LBRACE},
    {"}",   TokenType::RBRACE},
    {";",   TokenType::SEMICOLON},
    {",",   TokenType::COMMA},
    {":",   TokenType::COLON},
    {".",   TokenType::DOT},
    {"?",   TokenType::TERNARY},
};

// ============================================================================
// OPERATOR_MAP_REV — TokenType → Operatör String (Log/Görüntüleme İçin)
// ============================================================================
//
// AMAÇ: TokenType enum değerini insan tarafından okunabilir operatör
//       sembolüne dönüştürür. Log çıktısı ve hata mesajları için kullanılır.
//
// ANAHTAR: TokenType — enum değeri (örn: TokenType::PLUS)
// DEĞER:   std::string_view — operatör sembolü (örn: "+")
//
// KULLANIM:
//   auto it = OPERATOR_MAP_REV.find(type);
//   if (it != OPERATOR_MAP_REV.end()) std::cout << it->second;
//
// NOT: TokenType::IDENTIFIER, NUMBER, STRING, KW_* ve özel token'lar
//      bu haritada YOKTUR (operatör değiller). Onlar için ayrı dönüşüm gerekir.
//
inline const std::unordered_map<TokenType, std::string_view> OPERATOR_MAP_REV = {
    {TokenType::ARROW,              "->"},
    {TokenType::COLON_COLON,        "::"},
    {TokenType::EQUAL_EQUAL,        "=="},
    {TokenType::BANG_EQUAL,         "!="},
    {TokenType::LESS_EQUAL,         "<="},
    {TokenType::GREATER_EQUAL,      ">="},
    {TokenType::AMPERSAND_AMPERSAND,"&&"},
    {TokenType::PIPE_PIPE,          "||"},
    {TokenType::PLUS_PLUS,          "++"},
    {TokenType::MINUS_MINUS,        "--"},
    {TokenType::LSHIFT,             "<<"},
    {TokenType::RSHIFT,             ">>"},
    {TokenType::STAR_STAR,          "**"},
    {TokenType::PLUS_EQUAL,         "+="},
    {TokenType::MINUS_EQUAL,        "-="},
    {TokenType::STAR_EQUAL,         "*="},
    {TokenType::SLASH_EQUAL,        "/="},
    {TokenType::PERCENT_EQUAL,      "%="},
    {TokenType::AMPERSAND_EQUAL,    "&="},
    {TokenType::PIPE_EQUAL,         "|="},
    {TokenType::CARET_EQUAL,        "^="},
    {TokenType::LSHIFT_EQUAL,       "<<="},
    {TokenType::RSHIFT_EQUAL,       ">>="},
    {TokenType::PLUS,               "+"},
    {TokenType::MINUS,              "-"},
    {TokenType::STAR,               "*"},
    {TokenType::SLASH,              "/"},
    {TokenType::PERCENT,            "%"},
    {TokenType::LESS,               "<"},
    {TokenType::GREATER,            ">"},
    {TokenType::CARET,              "^"},
    {TokenType::BANG,               "!"},
    {TokenType::TILDE,              "~"},
    {TokenType::AMPERSAND,          "&"},
    {TokenType::PIPE,               "|"},
    {TokenType::EQUAL,              "="},
    {TokenType::LBRACKET,           "["},
    {TokenType::RBRACKET,           "]"},
    {TokenType::LPAREN,             "("},
    {TokenType::RPAREN,             ")"},
    {TokenType::LBRACE,             "{"},
    {TokenType::RBRACE,             "}"},
    {TokenType::SEMICOLON,          ";"},
    {TokenType::COMMA,              ","},
    {TokenType::COLON,              ":"},
    {TokenType::DOT,                "."},
    {TokenType::TERNARY,            "?"},
};

// ============================================================================
// OPERATOR_MAP_STRREV — TokenType → Enum İsmi (Debug/Log İçin)
// ============================================================================
//
// AMAÇ: AST log çıktısında operatörün enum ismini (string olarak) gösterir.
//       OPERATOR_MAP_REV'den farkı: sembol yerine enum adı döndürür.
//
// KULLANIM:
//   AST dump/debug çıktısı: TokenPrecedence(PLUS) yerine "PLUS(13)" gösterimi.
//
// ANAHTAR: TokenType — enum değeri (örn: TokenType::PLUS)
// DEĞER:   std::string_view — enum ismi (örn: "PLUS")
//
// ÖRN: TokenType::PLUS       → "PLUS"
//      TokenType::PLUS_EQUAL → "PLUS_EQUAL"
//
// NOT: İki harita da (REV ve STRREV) aynı anahtarları içerir ama farklı değerler.
//      REV: "+" (operatör sembolü)
//      STRREV: "PLUS" (enum ismi)
//
inline const std::unordered_map<TokenType, std::string_view> OPERATOR_MAP_STRREV = {
    {TokenType::ARROW,              "ARROW"},
    {TokenType::COLON_COLON,        "COLON_COLON"},
    {TokenType::EQUAL_EQUAL,        "EQUAL_EQUAL"},
    {TokenType::BANG_EQUAL,         "BANG_EQUAL"},
    {TokenType::LESS_EQUAL,         "LESS_EQUAL"},
    {TokenType::GREATER_EQUAL,      "GREATER_EQUAL"},
    {TokenType::AMPERSAND_AMPERSAND,"AMPERSAND_AMPERSAND"},
    {TokenType::PIPE_PIPE,          "PIPE_PIPE"},
    {TokenType::PLUS_PLUS,          "PLUS_PLUS"},
    {TokenType::MINUS_MINUS,        "MINUS_MINUS"},
    {TokenType::LSHIFT,             "LSHIFT"},
    {TokenType::RSHIFT,             "RSHIFT"},
    {TokenType::STAR_STAR,          "STAR_STAR"},
    {TokenType::PLUS_EQUAL,         "PLUS_EQUAL"},
    {TokenType::MINUS_EQUAL,        "MINUS_EQUAL"},
    {TokenType::STAR_EQUAL,         "STAR_EQUAL"},
    {TokenType::SLASH_EQUAL,        "SLASH_EQUAL"},
    {TokenType::PERCENT_EQUAL,      "PERCENT_EQUAL"},
    {TokenType::AMPERSAND_EQUAL,    "AMPERSAND_EQUAL"},
    {TokenType::PIPE_EQUAL,         "PIPE_EQUAL"},
    {TokenType::CARET_EQUAL,        "CARET_EQUAL"},
    {TokenType::LSHIFT_EQUAL,       "LSHIFT_EQUAL"},
    {TokenType::RSHIFT_EQUAL,       "RSHIFT_EQUAL"},
    {TokenType::PLUS,               "PLUS"},
    {TokenType::MINUS,              "MINUS"},
    {TokenType::STAR,               "STAR"},
    {TokenType::SLASH,              "SLASH"},
    {TokenType::PERCENT,            "PERCENT"},
    {TokenType::LESS,               "LESS"},
    {TokenType::GREATER,            "GREATER"},
    {TokenType::CARET,              "CARET"},
    {TokenType::BANG,               "BANG"},
    {TokenType::TILDE,              "TILDE"},
    {TokenType::AMPERSAND,          "AMPERSAND"},
    {TokenType::PIPE,               "PIPE"},
    {TokenType::EQUAL,              "EQUAL"},
    {TokenType::LBRACKET,           "LBRACKET"},
    {TokenType::RBRACKET,           "RBRACKET"},
    {TokenType::LPAREN,             "LPAREN"},
    {TokenType::RPAREN,             "RPAREN"},
    {TokenType::LBRACE,             "LBRACE"},
    {TokenType::RBRACE,             "RBRACE"},
    {TokenType::SEMICOLON,          "SEMICOLON"},
    {TokenType::COMMA,              "COMMA"},
    {TokenType::COLON,              "COLON"},
    {TokenType::DOT,                "DOT"},
    {TokenType::TERNARY,            "TERNARY"},
};

// ============================================================================
// TokenPrecedence — Operatör Öncelik Tablosu (Pratt Parser'ın Kalbi)
// ============================================================================
//
// AMAÇ: Her TokenType için bir öncelik seviyesi döndürür.
//       Yüksek sayı = daha sıkı bağlanma (önce işlenir).
//
// PARAMETRE: type — sorgulanan token tipi
// DÖNÜŞ:     uint16_t — öncelik seviyesi (0-18)
// KARMAŞIKLIK: O(1) — switch/case (derleyici jump table üretir)
//
// KULLANIM:
//   uint16_t prec = TokenPrecedence(current.type);
//   if (prec >= minPrec) { parseLeftDenotation(left); }
//
// ÖNCELİK SEVİYELERİ (yüksekten düşüğe):
//   18: Üye erişimi       . -> [ ] ( )     — En yüksek
//   17: Postfix           ++ --
//   16: Unary prefix      ! ~ + -
//   15: Üs alma           ** ^             — Sağ birleşmeli
//   14: Çarpma/Bölme      * / %
//   13: Toplama/Çıkarma   + -
//   12: Bitsel kaydırma   << >>
//   11: İlişkisel         < <= > >=
//   10: Eşitlik           == !=
//    9: Bitsel VE         &
//    8: Bitsel XOR        ^ (Level 8; üs yalnız ** , #230)
//    7: Bitsel VEYA       |
//    6: Mantıksal VE      &&
//    5: Mantıksal VEYA    ||
//    4: `?`               ? (ternary YOK — nullable tip işareti)
//    3: `:`               : (ternary else YOK — etiket)
//    2: Atama             = += -= vb.      — Sağ birleşmeli
//    1: Virgül            ,
//    0: Önceliksiz        (değerler, EOF, bilinmeyen)
//
// KARAR (#230, ürün sahibi): ^ (CARET) bitsel XOR'tur ve Level 8'dedir;
// üs alma yalnız ** (STAR_STAR, Level 15) ile yapılır. C/C++/Python ile
// hizalıdır: & (9) ile | (7) arasına düşer, unary '-'den (13) gevşektir.
// Geçmişte CARET, STAR_STAR ile birlikte "üs" diye Level 15'e konmuştu; IR
// yine de BXOR üretiyordu. Bu uyumsuzluk, unary '-'nin CARET'ten gevşek
// bağlanıp `-a ^ b`'yi `-(a ^ b)` diye parse etmesine yol açtı (bug #230).
// Seviye 8'e inince `-a ^ b` doğru şekilde `(-a) ^ b` olur.
//
inline uint16_t TokenPrecedence(TokenType type) {
    switch (type) {
        // Level 18: Member access / call
        case TokenType::DOT:
        case TokenType::ARROW:
        case TokenType::LBRACKET:
        case TokenType::LPAREN:
            return 18;

        // Level 17: Postfix
        case TokenType::PLUS_PLUS:
        case TokenType::MINUS_MINUS:
            return 17;

        // Level 16: Unary prefix — sadece her zaman prefix olanlar
        case TokenType::BANG:      // !
        case TokenType::TILDE:     // ~
            return 16;

        // Level 15: Exponentiation
        case TokenType::STAR_STAR: // ** (tek üs operatörü; ^ XOR'dur, #230)
            return 15;

        // Level 14: Multiplicative
        case TokenType::STAR:      // *
        case TokenType::SLASH:     // /
        case TokenType::PERCENT:   // %
            return 14;

        // Level 13: Additive — PLUS ve MINUS hem unary hem binary
        case TokenType::PLUS:      // +
        case TokenType::MINUS:     // -
            return 13;

        // Level 12: Bit shift + as cast (sola-bağlı, aritmetikten gevşek)
        case TokenType::LSHIFT:    // <<
        case TokenType::RSHIFT:    // >>
        case TokenType::KW_AS:     // as (ADR-026)
            return 12;

        // Level 11: Relational
        case TokenType::LESS:      // <
        case TokenType::LESS_EQUAL:// <=
        case TokenType::GREATER:   // >
        case TokenType::GREATER_EQUAL: // >=
            return 11;

        // Level 10: Equality
        case TokenType::EQUAL_EQUAL:   // ==
        case TokenType::BANG_EQUAL:    // !=
            return 10;

        // Level 9: Bitwise AND
        case TokenType::AMPERSAND: // &
            return 9;

        // Level 8: Bitwise XOR
        case TokenType::CARET:     // ^ bitsel XOR (#230; C/Python ile hizalı)
            return 8;
        // Level 7: Bitwise OR
        case TokenType::PIPE:      // |
            return 7;

        // Level 6: Logical AND
        case TokenType::AMPERSAND_AMPERSAND: // &&
            return 6;

        // Level 5: Logical OR
        case TokenType::PIPE_PIPE: // ||
            return 5;

        // Level 4: Ternary
        case TokenType::TERNARY:  // ?
            return 4;
        case TokenType::COLON:     // : (ternary için)
            return 3;             // ternary'den düşük, atamadan yüksek

        // Level 2: Assignment
        case TokenType::EQUAL:     // =
        case TokenType::PLUS_EQUAL:// +=
        case TokenType::MINUS_EQUAL:// -=
        case TokenType::STAR_EQUAL:// *=
        case TokenType::SLASH_EQUAL:// /=
        case TokenType::PERCENT_EQUAL:// %=
        case TokenType::AMPERSAND_EQUAL:// &=
        case TokenType::PIPE_EQUAL:// |=
        case TokenType::CARET_EQUAL:// ^=
        case TokenType::LSHIFT_EQUAL:// <<=
        case TokenType::RSHIFT_EQUAL:// >>=
            return 2;

        // Level 1: Comma
        case TokenType::COMMA:     // ,
            return 1;

        default:
            return 0;  // Önceliksiz: değerler, EOF, bilinmeyen
    }
}

// ============================================================================
// RightAssociative — Sağdan Sola Birleşme (Associativity) Kontrolü
// ============================================================================
//
// AMAÇ: Bir operatörün sağdan sola mı, yoksa soldan sağa mı birleştiğini
//       belirler. Pratt parser'da doğru ağaç yapısını oluşturmak için kritik.
//
// PARAMETRE: type — sorgulanan operatör tipi
// DÖNÜŞ:     bool — true: sağ birleşmeli, false: sol birleşmeli
// KARMAŞIKLIK: O(1) — switch/case
//
// Sağ birleşmeli operatörler (a OP b OP c = a OP (b OP c)):
//   - STAR_STAR (üs alma): 2 ** 3 ** 2 = 2 ** (3 ** 2) = 2^9 = 512
//     (matematiksel kural: üs sağdan sola birleşir)
//   - EQUAL (atama): a = b = 5 → a = (b = 5)
//     (önce b = 5 çalışır, sonra a = b)
//   - +=, -=, *=, vb. (birleşik atama): a += b += 5 → a += (b += 5)
//   - TERNARY (üçlü koşul): a ? b : c ? d : e → a ? b : (c ? d : e)
//     (iç içe ternary'lerde sağdan sola)
//
// Sol birleşmeli operatörler (a OP b OP c = (a OP b) OP c):
//   - Tüm diğerleri: +, -, *, /, ^ (XOR), ==, &&, ||, vb.
//     (a + b + c = (a + b) + c, yani önce a+b, sonuç + c)
//
// NOT (#230): CARET artık sağ birleşimli üs değil, bitsel XOR'dur — sol
// birleşimlidir. Üs birleşim yönü yalnız STAR_STAR'a aittir.
//
inline bool RightAssociative(TokenType type) {
    switch (type) {
        case TokenType::STAR_STAR:  // ** (üs)
        case TokenType::EQUAL:      // =
        case TokenType::PLUS_EQUAL: // +=
        case TokenType::MINUS_EQUAL:// -=
        case TokenType::STAR_EQUAL: // *=
        case TokenType::SLASH_EQUAL:// /=
        case TokenType::PERCENT_EQUAL:// %=
        case TokenType::AMPERSAND_EQUAL:// &=
        case TokenType::PIPE_EQUAL: // |=
        case TokenType::CARET_EQUAL:// ^=
        case TokenType::LSHIFT_EQUAL:// <<=
        case TokenType::RSHIFT_EQUAL:// >>=
        case TokenType::TERNARY:   // ? (ternary)
            return true;
        default:
            return false;
    }
}

// ============================================================================
// ParserToken — Parser'ın Kullandığı Token Yapısı (Köprü)
// ============================================================================
//
// AMAÇ: Tokenizer'ın ürettiği ham Token ile Parser'ın ihtiyaç duyduğu
//       anlamsal tipi (TokenType) bir arada tutar. İki katman arasında
//       köprü görevi görür.
//
// ALANLAR:
//
//   token (Token*):
//     Tokenizer'dan gelen orijinal token'a pointer.
//     Neden pointer, neden değer (Token token) değil?
//
//     Çünkü Token polimorfik bir sınıf hiyerarşisidir:
//       Token (base)
//         +-- NumberToken   (isFloat, numberValue alanları)
//         +-- StringToken   (context alanı)
//         +-- IdentifierToken
//         +-- OperatorToken
//         +-- DelimiterToken
//         +-- KeywordToken
//
//     Değer kopyası (Token token) OBJECT SLICING'e neden olur:
//     NumberToken → Token'a kopyalanırken isFloat, numberValue KAYBOLUR.
//
//     BUG FIX (commit 40579ca):
//       Eskiden "Token token" (değer) olarak tanımlanmıştı.
//       NumberToken.isFloat her zaman false dönüyordu çünkü slicing oluyordu.
//       "Token* token" (pointer) olarak değiştirildi.
//
//   type (TokenType):
//     Token'ın anlamsal tipi. Örn: NUMBER, PLUS, KW_IF.
//     Tokeni parselerken parseToken() tarafından atanır.
//
// METOTLAR:
//   is(TokenType):                Tek tip kontrolü (O(1))
//   is(initializer_list):         Çoklu tip kontrolü (O(k), k = liste boyutu)
//   getPowerOperator():           Öncelik sorgulama (O(1), TokenPrecedence'a yönlendirir)
//   isRightAssociative():         Birleşme yönü sorgulama (O(1))
//
struct ParserToken {
    /* ====== Alanlar ====== */

    // Tokenizer'dan gelen orijinal token pointer'ı.
    //   nullptr olabilir mi? Hayır — geçerli bir token her zaman vardır.
    //   SVR_VOID durumunda token nullptr olabilir (EOF sinyali).
    Token*    token = nullptr;

    // Token'ın anlamsal tipi.
    //   Varsayılan: SVR_VOID (geçersiz/başlangıç değeri).
    //   parseToken() tarafından atanır.
    TokenType type  = TokenType::SVR_VOID;

    /* ====== Kolaylık Metotları ====== */

    // is() — Tek tip kontrolü
    // PARAMETRE: t — sorgulanan token tipi
    // DÖNÜŞ:     true — bu token t tipinde
    // KARMAŞIKLIK: O(1)
    // KULLANIM:   if (current.is(TokenType::SEMICOLON)) { ... }
    bool is(TokenType t) const {
        return type == t;
    }

    // is() — Çoklu tip kontrolü
    // PARAMETRE: types — kontrol edilecek tipler listesi
    // DÖNÜŞ:     true — bu token listedeki tiplerden birine aitse
    // KARMAŞIKLIK: O(k) — k = liste boyutu
    // KULLANIM:
    //   if (current.is({KW_INT, KW_FLOAT, KW_VOID})) { ... }
    //   if (current.is({TokenType::SEMICOLON, TokenType::RPAREN})) { ... }
    bool is(std::initializer_list<TokenType> types) const {
        for (TokenType t : types)
            if (type == t) return true;
        return false;
    }

    // getPowerOperator() — Operatör önceliği sorgulama (Pratt parser için)
    // DÖNÜŞ:     uint16_t — öncelik seviyesi (0-18)
    // KARMAŞIKLIK: O(1) — TokenPrecedence'a yönlendirir
    // KULLANIM:
    //   uint16_t prec = current.getPowerOperator();
    //   while (prec >= minPrec) { ... parseLeftDenotation(left); }
    uint16_t getPowerOperator() const {
        return TokenPrecedence(type);
    }

    // isRightAssociative() — Birleşme yönü sorgulama
    // DÖNÜŞ:     true — sağ birleşmeli (atama, üs, ternary)
    //            false — sol birleşmeli (toplama, çarpma, vb.)
    // KARMAŞIKLIK: O(1) — RightAssociative'a yönlendirir
    // KULLANIM:
    //   bool rightAssoc = current.isRightAssociative();
    //   uint16_t nextPrec = rightAssoc ? prec : prec + 1;
    bool isRightAssociative() const {
        return RightAssociative(type);
    }
};

#endif // SAQUT_PARSER_TOKEN