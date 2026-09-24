// ============================================================================
// saQut Compiler — Sembol Veri Yapıları (Symbol, SymbolKind)
// ============================================================================
//
// DİZİN:   src/symbol/symbol.hpp
// KATMAN:  Faz 2 — Symbol struct'ı ve SymbolKind enum'ı
//
// AMAÇ:
//   Bir ismin (değişken, fonksiyon, struct, enum, alan, parametre) tüm
//   metaverisini taşıyan Symbol yapısı. Sembol toplama, tip denetimi, IR
//   üretimi ve hata raporlamanın ortak dilidir.
//
// ============================================================================

#ifndef SAQUT_SYMBOL_SYMBOL
#define SAQUT_SYMBOL_SYMBOL

#include <string>
#include <vector>
#include "core/type.hpp"
#include "core/location.hpp"

enum class SymbolKind { Variable, Function, Parameter, Struct, Field, Enum, EnumValue };

inline const char* symbolKindName(SymbolKind k) {
    switch (k) {
        case SymbolKind::Variable:   return "variable";
        case SymbolKind::Function:   return "function";
        case SymbolKind::Parameter:  return "parameter";
        case SymbolKind::Struct:     return "struct";
        case SymbolKind::Field:      return "field";
        case SymbolKind::Enum:       return "enum";
        case SymbolKind::EnumValue:  return "enum_value";
    }
    return "?";
}

class Scope;

struct Symbol {
    std::string                 name;
    SymbolKind                  kind = SymbolKind::Variable;
    Type                        type;
    int                         moduleId = -1;
    SourceLocation              definitionLoc;
    std::vector<SourceLocation> references;
    Scope*                      scope    = nullptr;
    bool                        isBuiltin = false;
    std::vector<std::string>    paramNames; // Function: parametre isimleri (type.paramTypes ile sıra eşleşir)
    // ADR-034 (#107): FFI host fonksiyonu ise sayısal host id
    // (hostEntryIndex kHostFnBase toplamıyla, #229), değilse -1. IRGen bunu
    // CALLHOST'un sayısal dispatch'ine taşır.
    int                         hostFnId = -1;
    // FFI sembolünün ait olduğu gömülü modül adı ("math"); import çözümü için.
    std::string                 ffiModule;
    // ADR-045: `shared` global (thread'ler arası görünür; heap dışında,
    // SharedSlots tablosunda yaşar). shared tipin değil SEMBOLÜN özelliğidir.
    bool                        isShared = false;
};

#endif // SAQUT_SYMBOL_SYMBOL
