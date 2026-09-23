// ============================================================================
// saQut Compiler — Modül Ad Alanı Geçişi (#246)
// ============================================================================
//
// DİZİN:   src/module/module_namespacer.hpp
// KATMAN:  ModuleLoader'dan sonra, SymbolCollector'dan önce
//
// AMAÇ:
//   Sembol tablosu, IR ve iki backend (VM/JIT) üst düzey adları (fonksiyon,
//   struct, enum, global) program çapında TEK ad alanında tutar. Bu yüzden:
//     - iki modülde export edilmemiş aynı ad E002 veriyordu,
//     - bir modülün FFI importu diğer modüllerde de görünüyordu,
//     - kaynak dosyada `import {x as y}` çalışmıyordu (y tanımsızdı).
//
//   Bu geçiş, sembol toplamadan ÖNCE AST üzerinde çalışır:
//     1. Birden fazla modülde tanımlanan bir üst düzey ad (ya da farklı
//        hedeflere bağlanan FFI import adı) giriş modülü dışındaki
//        modüllerde benzersiz bir iç ada (`ad@N`) çevrilir.
//     2. Dosya importlarındaki takma adlar hedef tanımın (son) adına
//        bağlanır; import eden dosyadaki kullanımlar yeniden yazılır.
//     3. Import edilen ad import eden dosyada da tanımlıysa E002 raporlanır
//        (konum: import eden dosya — eskiden kaynak dosyayı gösteriyordu).
//
//   Yeniden yazma kapsam bilincine sahiptir: aynı adı taşıyan yerel
//   değişken/parametre yeniden yazılmaz. Çakışma yoksa hiçbir ad değişmez;
//   tanılar ve LSP çıktısı kullanıcının yazdığı adları görür.
//
// ============================================================================

#ifndef SAQUT_MODULE_NAMESPACER
#define SAQUT_MODULE_NAMESPACER

#include "diagnostic/diagnostic_engine.hpp"
#include "module/module_graph.hpp"

void namespaceModules(ModuleGraph& graph, DiagnosticEngine& diag);

#endif // SAQUT_MODULE_NAMESPACER
