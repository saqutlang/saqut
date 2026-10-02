// ============================================================================
// saQut Compiler — Token Türleri ve Keyword / Operatör Tabloları
// ============================================================================
//
// DİZİN:   src/tokenizer/token_kind.hpp
// KATMAN:  Tokenizer — her token'ın türünün (TokenType) TEK tanımı
// BAĞIMLI: yalnız standart kütüphane
//
// İÇERİK:
//   1. TokenType enum: bütün token türleri (değer, keyword, operatör, delimiter)
//   2. KEYWORD_MAP:      metin → TokenType. Tokenizer bir adı keyword sayıp
//                        saymamaya BU tabloya bakarak karar verir; parser aynı
//                        türü token'dan okur. Tek keyword listesi budur.
//   3. OPERATOR_MAP:     operatör/delimiter metni → TokenType
//   4. OPERATOR_MAP_REV: TokenType → operatör metni (AST dökümü, tanılar)
//
// Tokenizer her token'ın `kind` alanını bu tablolardan doldurur; parser
// yeniden sınıflandırmaz. Öncelik tablosu parser'a aittir:
// parser/parser_token.hpp.
//
// YENİ KEYWORD: TokenType'a bir değer (ya da var olanı yeniden kullan) +
// KEYWORD_MAP'e bir satır. Tokenizer ve parser başka bir yere dokunmadan tanır.
//
// ============================================================================

#ifndef SAQUT_TOKENIZER_TOKEN_KIND
#define SAQUT_TOKENIZER_TOKEN_KIND

#include <cstdint>
#include <string_view>
#include <unordered_map>

// ============================================================================
// TokenType — Token Türleri (Enum)
// ============================================================================
//
// KATEGORİLER:
//   1. Değerler:      IDENTIFIER, NUMBER, STRING, SVR_VOID (geçersiz/EOF)
//   2. Keyword'ler:   KW_* (KEYWORD_MAP'teki metinler)
//   3. Operatörler ve delimiter'lar (OPERATOR_MAP'teki metinler)
//   4. Özel:          END_OF_FILE, UNKNOWN, COMMENT, PREPROCESSOR
//
// Enum'da değer olması o kelimenin/operatörün dilde desteklendiği anlamına
// gelmez (ör. KW_CLASS ayrılmış kelimedir, parser'da dalı yoktur).
//
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
                     //   Tokenizer yalnız OPERATOR_MAP'te olmayan bir
                     //   operatör metninde üretir (derleyici hatası; parser
                     //   "beklenmeyen token" raporlar).
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
// KEYWORD_MAP — keyword listesinin TEK kaynağı: metin → TokenType
// ============================================================================
//
// Tokenizer okuduğu adı burada bulursa keyword token'ı üretir ve türünü
// buradan yazar. Burada olmayan her ad identifier'dır. Bir kelime keyword
// olunca değişken/modül adı olarak kullanılamaz (`src/internal/*.sqt` dahil).
//
// `date`, `longint` keyword DEĞİLDİR: tip adı olarak identifier yolundan
// çözülürler (`import {now} from date;` gibi kullanımlar bu yüzden geçerli).
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

    // --- İzole thread modeli (ADR-045) ---
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
// Tokenizer operatör ve delimiter token'larının türünü bu tablodan yazar
// (ikisi parser için aynı şekilde işlenir). Hangi karakter dizisinin tek
// operatör olarak okunacağına (">>=" mi, ">>" + "=" mi) tokenizer.cpp
// scope()'daki karakter switch'i karar verir; yeni operatör iki yere de girer.
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

#endif // SAQUT_TOKENIZER_TOKEN_KIND