// ============================================================================
// saQut — Program çıktısı kilidi (ADR-045 §18, Faz 1-f)
//
// DİZİN:   src/runtime/output_lock.hpp
// KATMAN:  runtime — tüm isolate'lerin paylaştığı süreç-global çıktı kilidi
//
// "print satır bazında atomiktir": her print / stdout::write / stderr::write
// çağrısı tek bir kilit altında TEK parça yazılır; iki thread'in çıktısı bir
// çağrının ortasında karışmaz. Kilit süreç-globaldir (stdout/stderr zaten
// süreç-global). Sink (DAP output olayı) da aynı kilit altında çağrılır ki
// eşzamanlı thread'ler protokol akışını karıştırmasın; sink bu başlıktaki
// yardımcıları geri çağırmamalıdır (yeniden giriş = kilitlenme).
// ============================================================================

#ifndef SAQUT_RUNTIME_OUTPUT_LOCK
#define SAQUT_RUNTIME_OUTPUT_LOCK

#include <mutex>
#include <ostream>
#include <string_view>

inline std::mutex& programOutputMutex() {
    static std::mutex m;
    return m;
}

// Metni tek parça yazar ve flush eder (çağrı başına atomik).
inline void writeProgramOutput(std::ostream& os, std::string_view text) {
    std::lock_guard<std::mutex> lock(programOutputMutex());
    os.write(text.data(), static_cast<std::streamsize>(text.size()));
    os.flush();
}

#endif // SAQUT_RUNTIME_OUTPUT_LOCK
