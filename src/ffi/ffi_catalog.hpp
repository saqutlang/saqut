// ============================================================================
// saQut FFI — Katalog (Gömülü root.sqt'in Bir Kez Parse Edilmiş Hali)
// ============================================================================
//
// DİZİN:   src/ffi/ffi_catalog.hpp
// KATMAN:  FFI — gömülü modüllerin sembol kataloğu
//
// AMAÇ (ADR-034 #5, #107):
//   Gömülü root.sqt SÜREÇ ÖMRÜNDE BİR KEZ parse edilir (Meyers singleton).
//   FfiDeclNode'lar salt-okunur bildirimlerdir; tüm derlemeler güvenle paylaşır.
//   module → (isim → FfiDeclNode*) indeksiyle O(1) çözüm. Dosya-tabanlı modül
//   makinesinden bağımsız; SymbolCollector import'ları buradan bağlar.
//
// İŞ PARÇACIĞI GÜVENLİĞİ (ADR-045, 1-e): kurucudan sonra hiçbir mutatör
// yoktur (instance() const döner); ilk erişim C++11 "magic static" ile
// thread-safe'tir. Ayrı bir freeze() gerekmez — tip zaten dondurulmuştur.
//
// ============================================================================

#ifndef SAQUT_FFI_CATALOG
#define SAQUT_FFI_CATALOG

#include <string>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include "ffi/root_sqt.hpp"
#include "parser/nodes/declarations.hpp"
#include "tokenizer/tokenizer.hpp"
#include "parser/parser.hpp"
#include "diagnostic/diagnostic_engine.hpp"

class FfiCatalog {
public:
    // Süreç ömrü boyunca tek örnek; ilk çağrıda root.sqt parse edilir.
    static const FfiCatalog& instance() {
        static const FfiCatalog cat;
        return cat;
    }

    // module gömülü bir modül mü?
    bool hasModule(const std::string& module) const {
        return byModule_.count(module) > 0;
    }

    // module::name FFI bildirimini bul; yoksa nullptr.
    const FfiDeclNode* lookup(const std::string& module,
                              const std::string& name) const {
        auto m = byModule_.find(module);
        if (m == byModule_.end()) return nullptr;
        auto n = m->second.find(name);
        return n != m->second.end() ? n->second : nullptr;
    }

    std::vector<std::string> modules() const {
        std::vector<std::string> result;
        for (const auto& [module, _] : byModule_) result.push_back(module);
        std::sort(result.begin(), result.end());
        return result;
    }

    std::vector<std::string> names(const std::string& module) const {
        std::vector<std::string> result;
        auto it = byModule_.find(module);
        if (it == byModule_.end()) return result;
        for (const auto& [name, _] : it->second) result.push_back(name);
        std::sort(result.begin(), result.end());
        return result;
    }

private:
    FfiCatalog() {
        Tokenizer tokenizer;
        tokens_ = tokenizer.scan(kEmbeddedRootSqt, "<builtin:root.sqt>");
        DiagnosticEngine diag; // gömülü kaynak doğru varsayılır; tanılar yutulur
        Parser parser(&diag);
        ast_ = parser.parse(tokens_);
        if (!ast_) return;
        for (ASTNode* child : ast_->getChildren()) {
            if (child->kind != ASTKind::FfiDecl) continue;
            auto* d = static_cast<FfiDeclNode*>(child);
            byModule_[d->moduleName][d->name] = d;
        }
    }
    // Kopyalama kapalı — singleton.
    FfiCatalog(const FfiCatalog&)            = delete;
    FfiCatalog& operator=(const FfiCatalog&) = delete;

    ASTNode*            ast_ = nullptr;      // root.sqt AST (süreç ömrü)
    std::vector<Token*> tokens_;            // ast_ ile birlikte yaşar
    std::unordered_map<std::string,
        std::unordered_map<std::string, const FfiDeclNode*>> byModule_;
};

#endif // SAQUT_FFI_CATALOG
