// ============================================================================
// saQut — ModuleLoader (Bağımlılık Zinciri Çözücü)
// ============================================================================
//
// DİZİN:   src/module/module_loader.hpp
// KATMAN:  Modül Sistemi — import bildirimlerini izleyerek dosyaları yükler
//
// AMAÇ:
//   Giriş dosyasından başlayarak tüm import bağımlılıklarını BFS benzeri
//   yükler ve parse eder. LSP için SourceOverlay seam'i sunar.
//
// ============================================================================

#ifndef SAQUT_MODULE_LOADER
#define SAQUT_MODULE_LOADER

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include "module/module_graph.hpp"
#include "core/module_registry.hpp"
#include "diagnostic/diagnostic_engine.hpp"
#include "profiling/stage_timer.hpp"

// ModuleLoader: import bildirimlerini izleyerek tüm bağımlı dosyaları
// yükler ve parse eder. Döngüsel bağımlılık (A→B→A) E_MODULE_CYCLE
// tanısıyla derleme hatası üretir (ADR-031); elmas bağımlılık (A→B→D,
// A→C→D) meşrudur ve D yalnızca bir kez yüklenir.
//
// Kullanım:
//   ModuleRegistry registry;
//   DiagnosticEngine diag;
//   ModuleLoader loader(registry, diag);
//   ModuleGraph graph = loader.load("main.sqt");
class ModuleLoader {
public:
    // path → içerik sağlayan kaynak sağlayıcı seam'i (LSP editör buffer'ı için).
    // true dönerse `out` kullanılır; false dönerse loadUnit diske düşer.
    using SourceOverlay = std::function<bool(const std::string& path, std::string& out)>;

    ModuleLoader(ModuleRegistry& registry, DiagnosticEngine& diag,
                 SourceOverlay overlay = nullptr)
        : registry_(registry), diag_(diag), overlay_(std::move(overlay)) {}

    // Giriş dosyasından başlayarak tüm bağımlı modülleri yükle.
    // units[0] her zaman giriş dosyasıdır.
    ModuleGraph load(const std::string& entryFilePath);

    // `saqut run --profile` (src/profiling/) için: verilirse her dosyanın
    // tokenize/parse süresi "token"/"parser" adları altında toplanır.
    // nullptr (varsayılan) = ölçüm yapılmaz, hiçbir ek maliyet yok.
    void setProfiler(Profiling::StageTimer* profiler) { profiler_ = profiler; }

private:
    // Tek bir dosyayı yükle, parse et, ImportDeclNode'larını takip et.
    // Zaten yüklenmiş dosyalar atlanır (seen_ ile kontrol); yükleme
    // zincirinde tekrar görünen dosya döngü hatasıdır (loadChain_).
    // importLoc: bu dosyayı isteyen import bildiriminin konumu (tanı için).
    void loadUnit(const std::string& filePath, ModuleGraph& graph,
                  const SourceLocation& importLoc = SourceLocation{});

    // İmport yolunu çözümle: import eden dosyanın dizinine göre canonical yol üret.
    std::string resolvePath(const std::string& importerPath,
                            const std::string& rawPath);

    ModuleRegistry&   registry_;
    DiagnosticEngine& diag_;
    SourceOverlay     overlay_;
    Profiling::StageTimer* profiler_ = nullptr;

    // Yüklemesi başlatılmış dosyalar (canonical path) — tekrar yüklemeyi
    // ve hata alan dosya için mükerrer tanıyı önler.
    std::unordered_set<std::string> seen_;

    // Aktif yükleme zinciri (canonical path, sıralı) — döngü tespiti ve
    // E_MODULE_CYCLE mesajındaki A → B → A zinciri için (ADR-031).
    std::vector<std::string> loadChain_;
};

#endif // SAQUT_MODULE_LOADER
