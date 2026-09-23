#ifndef SAQUT_CORE_CONFIG
#define SAQUT_CORE_CONFIG

// Derleyici yapılandırması — hangi optimizasyon pass'lerinin çalışacağı.
struct CompilerConfig {
    bool optConstantFolding = true;
    bool optDeadCodeElim    = true;
    int  maxFixpointRounds  = 10;
};

// #254: saQut fonksiyon çağrısı derinlik sınırı. VM ve JIT AYNI sınırı
// sayar (VM≡JIT: aynı program aynı noktada E_STACK_OVERFLOW verir). Aşılınca
// yakalanabilir hata üretilir; eskiden VM sınırsız büyüyüp belleği tüketiyor,
// JIT native yığını taşırıp segfault veriyordu. JIT ayrıca native yığın
// payını da denetler (büyük çerçeveli fonksiyonlar sınıra varmadan taşmasın).
constexpr int kMaxCallDepth = 100000;
// Etkin sınır: varsayılan kMaxCallDepth, `--max-call-depth=N` ile değişir.
inline int gMaxCallDepth = kMaxCallDepth;

#endif // SAQUT_CORE_CONFIG
