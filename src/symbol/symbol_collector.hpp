// ============================================================================
// saQut Compiler — Sembol Toplayıcı (SymbolCollector)
// ============================================================================
//
// DİZİN:   src/symbol/symbol_collector.hpp
// KATMAN:  Faz 2 — 3 geçişli (3-pass) sembol toplama algoritması
//
// AMAÇ:
//   AST üzerinde gezerek tüm bildirimleri kaydeder, tipleri çözümler,
//   kapsam hiyerarşisini kurar ve identifier'ları tanımlarına bağlar.
//   Çok dosyalı (multi-module) derlemeyi destekler.
//
// ============================================================================

#ifndef SAQUT_SYMBOL_COLLECTOR
#define SAQUT_SYMBOL_COLLECTOR

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "symbol/symbol_table.hpp"
#include "diagnostic/diagnostic_engine.hpp"
#include "core/type.hpp"
#include "core/location.hpp"
#include "parser/ast_node.hpp"
#include "module/module_graph.hpp"
#include "core/module_registry.hpp"

class ImportDeclNode;

class SymbolCollector {
public:
    SymbolCollector(SymbolTable& t, DiagnosticEngine& d)
        : table_(t), diag_(d) {}

    // Tek dosya (geriye dönük uyumluluk): seedBuiltins → 3 geçiş → structCycles
    void collect(ASTNode* program);

    // Çok dosya: tüm modülleri 3 geçişle topla, import'ları bağla
    void collectModuleGraph(ModuleGraph& graph);

private:
    void seedBuiltins();

    // Geçiş 1a — sadece tip isimlerini kaydet (struct/enum adları, fonksiyon stubs)
    void pass1aRegisterNames(ASTNode* program, int moduleId);

    // Geçiş 1b — struct layout'larını ve fonksiyon imzalarını çözümle
    void pass1bResolveLayouts(ASTNode* program, int moduleId);

    // Import doğrulaması: export edilmiş mi? İsim var mı? Scope'a bağla.
    void validateImports(ModuleGraph& graph);

    // ADR-034 (#107): tırnaksız import (`import {sqrt} from math;`) — gömülü
    // FfiCatalog'dan çöz, global scope'a hostFnId taşıyan Symbol tanımla.
    void resolveFfiImport(ImportDeclNode* imp);

    // Geçiş 2 — fonksiyon gövdelerini gez, isimleri çöz
    void pass2Bodies(ASTNode* program, int moduleId);

    void checkStructCycles();
    void walkStmt(ASTNode* node);
    void walkExpr(ASTNode* node);

    Type typeFromName(const std::string& n, const SourceLocation& loc);

    SymbolTable&      table_;
    DiagnosticEngine& diag_;
    int               currentModuleId_ = -1;

    // struct adı → içerdiği struct-tip alan adları (cycle check için)
    std::unordered_map<std::string, std::vector<std::string>> structFields_;
    // struct adı → bildirimi (E010 tanısının alan konumunu bulmak için)
    std::unordered_map<std::string, ASTNode*> structDecls_;

    // Mevcut modülün import ettiği isimler (pass2 sınır kontrolü için)
    std::unordered_set<std::string> currentModuleImports_;

    // moduleId → o modülün import ettiği isimler kümesi
    std::unordered_map<int, std::unordered_set<std::string>> moduleImports_;
};

#endif // SAQUT_SYMBOL_COLLECTOR
