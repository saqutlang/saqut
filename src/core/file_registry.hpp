// ============================================================================
// saQut Compiler — FileRegistry (Kaynak Dosya Yolu Havuzu)
// ============================================================================
//
// DİZİN:   src/core/file_registry.hpp
// KATMAN:  Katman 0 — SourceLocation'ın altında, her katmandan erişilir
// BAĞIMLI: Yok (sadece standart kütüphane)
//
// AMAÇ:
//   Kaynak dosya yollarını bir kez saklar ve her birine küçük bir tam sayı
//   kimliği (fileId) verir. SourceLocation dosya yolunu string olarak DEĞİL,
//   bu kimlikle taşır.
//
// NEDEN:
//   SourceLocation her token'da ve her AST düğümünde kopyalanır. filePath bir
//   std::string olduğu sürece her token dosya yolunun tam bir kopyasını taşır;
//   yol 15 karakteri (libstdc++ SSO sınırı) aşarsa bu token başına AYRI BİR
//   HEAP TAHSİSİ demektir.
//
//   Ölçüm (examples/large.sqt — 2.3 MB, 1.059.635 token), yalnızca tokenize:
//     dosya yolu  5 karakter (SSO'ya sığar) : 4.474.725 tahsis /  801 MB
//     dosya yolu 40 karakter (SSO'yu aşar)  : 9.504.058 tahsis / 1007 MB
//   Aradaki ~5 milyon tahsis ve ~206 MB'ın tek sebebi dosya adının token
//   başına kopyalanmasıydı.
//
//   fileId ile SourceLocation 48 bayttan 16 bayta iner ve dosya yolu uzunluğu
//   bellek kullanımını hiç etkilemez.
//
// ENUMERASYON:
//   Kimlikler kayıt sırasına göre 1'den başlar; 0 "bilinmiyor"a ayrılmıştır.
//     0 → ""            (geçersiz/bilinmeyen — varsayılan SourceLocation)
//     1 → "a.sqt"
//     2 → "b/c/dd.sqt"
//     ...
//
// YAŞAM DÖNGÜSÜ:
//   Süreç ömrü boyunca yaşayan tek bir örnek (instance()). Kayıtlar asla
//   silinmez — kimlikler kalıcıdır, bir SourceLocation kopyalandığında veya
//   saklandığında hedefi hâlâ geçerlidir.
//
//   ModuleRegistry'den (core/module_registry.hpp) ayrıdır: o IRProgram içinde
//   yaşar ve modül kimliklerini tutar; bu ise SourceLocation'ın altında,
//   tokenizer'dan LSP'ye kadar her katmanda kullanılır.
//
// İŞ PARÇACIĞI GÜVENLİĞİ (ADR-045, 1-e):
//   Derleme tek iş parçacıklıdır. Koşu komutları (run/exec) derleme bitince
//   freeze() çağırır; sonrasında yeni yol kaydı debug'da assert'tir ve kayıt
//   defteri salt okunur olduğundan path() birden çok isolate'ten kilitsiz
//   okunabilir. Zaten kayıtlı bir yolun intern()'ü (salt arama) serbesttir.
//   LSP süreç içinde tekrar derlediği için dondurmaz.
// ============================================================================

#ifndef SAQUT_CORE_FILE_REGISTRY
#define SAQUT_CORE_FILE_REGISTRY

#include <cassert>
#include <string>
#include <unordered_map>
#include <vector>

class FileRegistry {
public:
    // 0 = bilinmeyen/geçersiz dosya. Varsayılan SourceLocation bu kimliği taşır.
    static constexpr int UNKNOWN_ID = 0;

    static FileRegistry& instance() {
        static FileRegistry reg;
        return reg;
    }

    // Yolu kaydeder ve kimliğini döndürür; zaten kayıtlıysa mevcut kimlik döner
    // (aynı yol her zaman aynı kimliği alır). Boş yol UNKNOWN_ID'dir.
    int intern(const std::string& path) {
        if (path.empty()) return UNKNOWN_ID;
        auto it = index_.find(path);
        if (it != index_.end()) return it->second;
        assert(!frozen_ && "FileRegistry::intern: freeze() sonrası yeni yol kaydı");
        const int id = static_cast<int>(paths_.size());
        paths_.push_back(path);
        index_.emplace(path, id);
        return id;
    }

    // Kimlik → yol. Bilinmeyen/aralık dışı kimlikte boş string döner —
    // eski davranışla uyumlu: filePath'i hiç atanmamış SourceLocation da
    // boş string veriyordu.
    const std::string& path(int id) const {
        if (id <= UNKNOWN_ID || id >= static_cast<int>(paths_.size()))
            return paths_[UNKNOWN_ID];
        return paths_[static_cast<size_t>(id)];
    }

    // Kayıtlı dosya sayısı (UNKNOWN_ID dahil değil) — teşhis/test için.
    int fileCount() const { return static_cast<int>(paths_.size()) - 1; }

    // Derleme sonu: kayıt defteri bundan sonra salt okunur (1-e).
    void freeze() { frozen_ = true; }
    bool frozen() const { return frozen_; }

private:
    FileRegistry() {
        paths_.emplace_back(); // paths_[0] = "" → UNKNOWN_ID
    }

    std::vector<std::string>                 paths_;
    std::unordered_map<std::string, int>     index_;
    bool                                     frozen_ = false;
};

#endif // SAQUT_CORE_FILE_REGISTRY
