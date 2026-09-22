// ============================================================================
// saQut Core — Tanımlı Tamsayı Aritmetiği (ADR-040)
// ============================================================================
//
// DİZİN:   src/core/int_arithmetic.hpp
// KATMAN:  Core — backend ve pipeline aşamalarından bağımsız
//
// AMAÇ:
//   saQut'un int32/longint64 aritmetiğinin TEK KAYNAĞI. Taşma davranışı
//   dilin gözlemlenebilir sözleşmesidir (ADR-040: tanımlı 2's-complement
//   wrap), dolayısıyla onu hesaplayan her yol aynı fonksiyonları
//   çağırmalıdır — ayrı kopyalar sessizce ayrışır.
//
// KİM KULLANIR:
//   - VM yorumlayıcı (src/vm/interpreter.cpp) — çalışma zamanı aritmetiği
//   - Sabit katlama (src/opt/constant_folding.hpp) — derleme zamanı
//   MIR JIT native talimat üretir (MIR_ADD/MUL/LSH...); bu fonksiyonlar
//   onun davranışını TAKLİT eder, dolayısıyla üçü de aynı sonucu verir.
//
// NEDEN GEREKLİ — C++ ile saQut aynı şey değildir:
//   C++'ta signed overflow TANIMSIZDIR (UB). `a + b` yazmak derleyiciye
//   "bu asla taşmaz" demektir; optimizasyon bu varsayıma dayanıp kodu
//   yeniden yazabilir. saQut'ta ise taşma TANIMLIDIR ve gözlemlenebilir bir
//   sonuç üretir. Bu yüzden toplama/çıkarma/çarpma unsigned üzerinden
//   yapılır: unsigned taşması C++'ta da tanımlıdır (modüler aritmetik) ve
//   2's-complement gösterimde istenen sonucu birebir verir.
//
//   Aynı gerekçeyle:
//     - INT_MIN / -1 ve INT_MIN % -1 donanımda TUZAK üretir (x86 #DE,
//       süreç çöker) — matematiksel sonuç int32'ye sığmadığı için. Elle
//       2's-complement karşılığı döndürülür.
//     - Kaydırma miktarı 32 (veya 64) veya üzeriyse C++'ta UB'dir; x86 ve
//       MIR miktarı 5 (veya 6) bit maskeler. Maskeleme burada AÇIKÇA
//       yapılır ki davranış donanıma değil bu dosyaya bağlı olsun.
//
// TAŞMA POLİTİKASI (ADR-040): taşma HATA DEĞİLDİR, sessizce wrap eder.
//   Kontrollü aritmetik istendiğinde çözüm operatöre kontrol gömmek DEĞİL,
//   ayrı bir yüzey sunmaktır (Rust modeli: `a + b` ucuz kalır, `checkedAdd`
//   isteyen öder). Sıcak yol bu dosyada tek talimatlık kalmalıdır.
// ============================================================================

#ifndef SAQUT_CORE_INT_ARITHMETIC
#define SAQUT_CORE_INT_ARITHMETIC

#include <climits>
#include <cstdint>

namespace saqut::intmath {

// ── int32 (saQut `int`) ─────────────────────────────────────────────────────

inline int wrapAddI32(int a, int b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
inline int wrapSubI32(int a, int b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}
inline int wrapMulI32(int a, int b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}
// Çağıran b == 0 durumunu ÖNCEDEN elemelidir (VM'de runtime hatası, sabit
// katlamada W002 + katlama atlanır). Buradaki tek özel durum donanım tuzağıdır.
inline int wrapDivI32(int a, int b) {
    return (a == INT_MIN && b == -1) ? INT_MIN : a / b;
}
inline int wrapModI32(int a, int b) {
    return (a == INT_MIN && b == -1) ? 0 : a % b;
}
inline int wrapShlI32(int a, int b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) << (b & 31));
}
inline int wrapShrI32(int a, int b) {  // aritmetik kaydırma (işaret korunur)
    return a >> (b & 31);
}
inline int wrapNegI32(int a) {
    return static_cast<int32_t>(0U - static_cast<uint32_t>(a));
}

// #237: tamsayı üs alma (`**`).
//
// libm pow() KULLANILMAZ. pow() double üzerinden çalışır ve 2^53'ü aşan
// tamsayı sonuçlarda yuvarlama hatası verir (ör. pow(3,34) tam değeri
// veremez); ayrıca farklı libm sürümleri son bitte ayrışabilir. Tekrarlı
// çarpma hem tam sonucu verir hem de VM ile JIT'in bit-birebir aynı değeri
// üretmesini garanti eder (ADR-040 wrap sözleşmesi taşma için de geçerli:
// sonuç int32'ye sarar, tanımsız davranış yok).
//
// Negatif üs çağıran tarafından ELENMELİDİR (tamsayı sonucu kesirli olurdu):
// VM'de E_POWNEG runtime hatası, sabit katlamada katlama atlanır.
inline int wrapPowI32(int base, int exp) {
    int result = 1;
    int b = base;
    unsigned e = static_cast<unsigned>(exp);
    while (e) {                       // kare-al-ve-çarp: O(log e)
        if (e & 1u) result = wrapMulI32(result, b);
        e >>= 1;
        if (e) b = wrapMulI32(b, b);
    }
    return result;
}

// ── int64 (saQut `longint`) ─────────────────────────────────────────────────

inline long long wrapAddI64(long long a, long long b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
}
inline long long wrapSubI64(long long a, long long b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b));
}
inline long long wrapMulI64(long long a, long long b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
}
inline long long wrapDivI64(long long a, long long b) {
    return (a == INT64_MIN && b == -1) ? INT64_MIN : a / b;
}
inline long long wrapModI64(long long a, long long b) {
    return (a == INT64_MIN && b == -1) ? 0 : a % b;
}
inline long long wrapShlI64(long long a, long long b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) << (b & 63));
}
inline long long wrapShrI64(long long a, long long b) {
    return a >> (b & 63);
}
inline long long wrapNegI64(long long a) {
    return static_cast<int64_t>(0ULL - static_cast<uint64_t>(a));
}

// #237: longint üs alma — wrapPowI32 ile aynı sözleşme, 64-bit sarma.
inline long long wrapPowI64(long long base, long long exp) {
    long long result = 1;
    long long b = base;
    unsigned long long e = static_cast<unsigned long long>(exp);
    while (e) {
        if (e & 1ull) result = wrapMulI64(result, b);
        e >>= 1;
        if (e) b = wrapMulI64(b, b);
    }
    return result;
}

}  // namespace saqut::intmath

#endif  // SAQUT_CORE_INT_ARITHMETIC
