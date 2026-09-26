// ============================================================================
// saQut Compiler — Kapsam (Scope) Sınıfı
// ============================================================================
//
// DİZİN:   src/symbol/scope.hpp
// KATMAN:  Sembol toplama — kapsam hiyerarşisi ve lexical scoping
//
// AMAÇ:
//   Sembol kapsamını temsil eder. parent işaretçisi ile iç içe bloklar
//   için kapsam ağacı oluşturur. resolve() parent zincirini tırmanarak
//   lexical scoping uygular.
//
// ============================================================================

#ifndef SAQUT_SYMBOL_SCOPE
#define SAQUT_SYMBOL_SCOPE

#include <unordered_map>
#include <vector>
#include "symbol/symbol.hpp"

class Scope {
public:
    Scope* parent = nullptr;
    explicit Scope(Scope* p = nullptr) : parent(p) {}

    // Bu scope'a ekle. Aynı adda varsa nullptr (duplicate → çağıran E002 verir).
    Symbol* defineLocal(Symbol* s) {
        if (table.count(s->name)) return nullptr;
        table[s->name] = s;
        order.push_back(s);
        s->scope = this;
        return s;
    }

    Symbol* lookupLocal(const std::string& n) {
        auto it = table.find(n);
        return it == table.end() ? nullptr : it->second;
    }

    Symbol* resolve(const std::string& n) {
        for (Scope* s = this; s; s = s->parent)
            if (auto* r = s->lookupLocal(n)) return r;
        return nullptr;
    }

    std::unordered_map<std::string, Symbol*> table; // non-owning
    std::vector<Symbol*> order;                      // ekleme sırası
};

#endif // SAQUT_SYMBOL_SCOPE
