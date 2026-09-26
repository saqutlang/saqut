// ============================================================================
// saQut Compiler — Sembol Tablosu (SymbolTable)
// ============================================================================
//
// DİZİN:   src/symbol/symbol_table.hpp
// KATMAN:  Sembol toplama — sembol ömrü yönetimi + struct/enum layout haritaları
//
// AMAÇ:
//   Sembollerin (Symbol) ve kapsamların (Scope) ömrünü yönetir.
//   structLayouts ve enumLayouts haritaları ile tip denetleyici ve IR
//   üretecinin struct/enum düzenine erişmesini sağlar.
//
// ============================================================================

#ifndef SAQUT_SYMBOL_TABLE
#define SAQUT_SYMBOL_TABLE

#include <memory>
#include <vector>
#include <unordered_map>
#include "symbol/scope.hpp"

class SymbolTable {
public:
    SymbolTable() {
        global_  = newScope(nullptr);
        current_ = global_;
    }

    Scope* global()  { return global_; }
    Scope* current() { return current_; }

    Scope* enterScope() {
        current_ = newScope(current_);
        return current_;
    }

    void exitScope() {
        if (current_->parent) current_ = current_->parent;
    }

    // current scope'ta tanımla; duplicate → nullptr döner
    // moduleId: ModuleRegistry ID (-1 = main, 0 = __builtin__)
    Symbol* define(const std::string& name, SymbolKind k, Type t, SourceLocation loc,
                   int moduleId = -1) {
        auto s = std::make_unique<Symbol>();
        s->name         = name;
        s->kind         = k;
        s->type         = std::move(t);
        s->definitionLoc = loc;
        s->moduleId      = moduleId;
        Symbol* raw = s.get();
        if (!current_->defineLocal(raw)) return nullptr; // duplicate
        pool_.push_back(std::move(s));
        return raw;
    }

    Symbol* resolve(const std::string& n) { return current_->resolve(n); }

    void addReference(Symbol* s, SourceLocation loc) {
        if (s) s->references.push_back(loc);
    }

    std::vector<Symbol*> allSymbols() const {
        std::vector<Symbol*> result;
        result.reserve(pool_.size());
        for (const auto& s : pool_) result.push_back(s.get());
        return result;
    }

    // Struct alan düzeni: struct adı → sıralı [(alan adı, tip)] listesi
    // Sembol toplayıcı doldurur; tip denetleyici ve IR üreteci okur.
    std::unordered_map<std::string, std::vector<std::pair<std::string, Type>>> structLayouts;

    // Enum üye düzeni: enum adı → sıralı [(üye adı, int değer)] listesi
    std::unordered_map<std::string, std::vector<std::pair<std::string, int>>> enumLayouts;

    bool isEnumName(const std::string& name) const {
        return enumLayouts.count(name) > 0;
    }
    bool hasStruct(const std::string& name) const {
        return structLayouts.count(name) > 0;
    }

    int getEnumMemberValue(const std::string& enumName, const std::string& member) const {
        auto it = enumLayouts.find(enumName);
        if (it == enumLayouts.end()) return -1;
        for (auto& p : it->second)
            if (p.first == member) return p.second;
        return -1;
    }
    bool hasEnumMember(const std::string& enumName, const std::string& member) const {
        auto it = enumLayouts.find(enumName);
        if (it == enumLayouts.end()) return false;
        for (auto& p : it->second)
            if (p.first == member) return true;
        return false;
    }

    int getFieldIndex(const std::string& structName, const std::string& fieldName) const {
        auto it = structLayouts.find(structName);
        if (it == structLayouts.end()) return -1;
        for (int i = 0; i < (int)it->second.size(); i++)
            if (it->second[i].first == fieldName) return i;
        return -1;
    }

    Type getFieldType(const std::string& structName, const std::string& fieldName) const {
        auto it = structLayouts.find(structName);
        if (it == structLayouts.end()) return Type::error();
        for (auto& p : it->second)
            if (p.first == fieldName) return p.second;
        return Type::error();
    }

private:
    Scope* newScope(Scope* p) {
        scopes_.push_back(std::make_unique<Scope>(p));
        return scopes_.back().get();
    }

    std::vector<std::unique_ptr<Scope>>  scopes_;
    std::vector<std::unique_ptr<Symbol>> pool_;
    Scope* global_  = nullptr;
    Scope* current_ = nullptr;
};

#endif // SAQUT_SYMBOL_TABLE
