// ============================================================================
// saQut Compiler — SymbolCollector Gerçeklemesi
// ============================================================================
//
// DİZİN:   src/symbol/symbol_collector.cpp
// KATMAN:  Faz 2 — 3 geçişli sembol toplama
//
// AMAÇ:
//   AST üzerinde 3 geçiş (pass1a, pass1b, pass2) ile sembolleri toplar,
//   tipleri çözümler ve identifier'ları bağlar. çok dosyalı derleme için
//   collectModuleGraph() giriş noktası.
//
// ============================================================================

#include "symbol/symbol_collector.hpp"
#include <filesystem>
#include <functional>
#include "parser/nodes/program.hpp"
#include "parser/nodes/declarations.hpp"
#include "parser/nodes/statements.hpp"
#include "parser/nodes/expressions.hpp"
#include "parser/nodes/binary_expr.hpp"
#include "parser/nodes/identifier.hpp"
#include "ffi/ffi_catalog.hpp"
#include "ffi/host_registry.hpp"
#include "core/module_registry.hpp"
#include "module/module_namespacer.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// collect — tek dosya (geriye dönük uyumluluk)
// ─────────────────────────────────────────────────────────────────────────────

void SymbolCollector::collect(ASTNode* program) {
    if (!program) return;
    seedBuiltins();
    pass1aRegisterNames(program, currentModuleId_);
    pass1bResolveLayouts(program, currentModuleId_);
    checkStructCycles();
    pass2Bodies(program, currentModuleId_);
}

// ─────────────────────────────────────────────────────────────────────────────
// collectModuleGraph — çok dosya, 3 geçişli
// ─────────────────────────────────────────────────────────────────────────────

void SymbolCollector::collectModuleGraph(ModuleGraph& graph) {
    // #246: modül başına ad alanı — çakışan üst düzey adlar ve import takma
    // adları sembol toplamadan önce AST'de çözülür (module_namespacer.hpp).
    namespaceModules(graph, diag_);

    seedBuiltins();

    // Geçiş 1a: tüm modüllerde sadece tip isimlerini kaydet
    for (auto& unit : graph.units)
        pass1aRegisterNames(unit.ast, unit.moduleId);

    // Geçiş 1b: tüm modüllerde struct layout + fonksiyon imzaları
    for (auto& unit : graph.units)
        pass1bResolveLayouts(unit.ast, unit.moduleId);

    checkStructCycles();

    // Import doğrulaması: export edilmiş mi? İsim scope'a bağlansın.
    validateImports(graph);

    // Geçiş 2: tüm modüllerde fonksiyon gövdeleri
    for (auto& unit : graph.units) {
        currentModuleId_      = unit.moduleId;
        currentModuleImports_ = moduleImports_[unit.moduleId];
        pass2Bodies(unit.ast, unit.moduleId);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// seedBuiltins — global scope'a yerleşik fonksiyonları ekle
// ─────────────────────────────────────────────────────────────────────────────

void SymbolCollector::seedBuiltins() {
    // TODO(#89 builtin kataloğu): geçici; ileride gerçek katalog gelecek.
    Symbol* s = table_.define("print", SymbolKind::Function,
                               Type::function(Type::Void(), {}),
                               SourceLocation{}, 0 /* BUILTIN_ID */);
    if (s) s->isBuiltin = true;

    // ADR-025: Error builtin struct — try/catch için
    // Alan sırası VM makeErrorValue() ile eşleşmeli: [line, col, message, trace, code]
    table_.structLayouts["Error"] = {
        {"line",    Type::Int()},
        {"col",     Type::Int()},
        {"message", Type::String()},
        {"trace",   Type::String()},
        {"code",    Type::String()}
    };
    Symbol* errSym = table_.define("Error", SymbolKind::Struct,
                                   Type::structType("Error"), {}, 0 /* BUILTIN_ID */);
    if (errSym) errSym->isBuiltin = true;
    structFields_["Error"]; // cycle checker'a tanıt
}

// ─────────────────────────────────────────────────────────────────────────────
// typeFromName — tip adından Type üret; bilinmiyorsa E007
// ─────────────────────────────────────────────────────────────────────────────

Type SymbolCollector::typeFromName(const std::string& n, const SourceLocation& loc) {
    // "Node?" gibi nullable struct: Type::fromName struct'ı bilmez, burada çözüyoruz.
    if (!n.empty() && n.back() == '?') {
        Type base = typeFromName(n.substr(0, n.size() - 1), loc);
        if (!base.isError()) return base.asNullable();
        return Type::error();
    }
    Type t = Type::fromName(n);
    if (!t.isError()) return t;
    if (structFields_.count(n)) return Type::structType(n);
    if (table_.isEnumName(n)) return Type::enumType(n);
    // "Point[]", "Color[]" vb. — struct veya enum array tipi
    if (n.size() > 2 && n.substr(n.size() - 2) == "[]") {
        Type elem = typeFromName(n.substr(0, n.size() - 2), loc);
        if (!elem.isError()) return Type::array(elem);
        return Type::error(); // taban tip zaten E007 raporladı
    }
    diag_.report("E007", loc, "unknown type: '" + n + "'",
        "known types: int, float, double, decimal, bool, string. if using a struct, define it first: `struct " + n + " { ... }`, or an enum: `enum " + n + " { ... }`");
    return Type::error();
}

// ─────────────────────────────────────────────────────────────────────────────
// pass1aRegisterNames — sadece tip isimlerini kaydet, alan/imza çözümleme yok.
// Tüm modüllerde çalıştıktan sonra pass1bResolveLayouts çağrılır; bu sayede
// çapraz modül tip referansları (A.sqt struct'ı B.sqt struct'ını içeriyor)
// sorunsuz çözümlenir.
// ─────────────────────────────────────────────────────────────────────────────

void SymbolCollector::pass1aRegisterNames(ASTNode* program, int moduleId) {
    for (ASTNode* child : program->getChildren()) {
        switch (child->kind) {

        case ASTKind::StructDecl: {
            auto* st = static_cast<StructDeclNode*>(child);
            Symbol* s = table_.define(st->name, SymbolKind::Struct,
                                      Type::structType(st->name), st->loc, moduleId);
            if (!s) break; // çakışma — pass1b'de hata üretilecek
            structFields_[st->name]; // cycle checker için boş giriş aç
            structDecls_[st->name] = st;
            break;
        }

        case ASTKind::EnumDecl: {
            auto* en = static_cast<EnumDeclNode*>(child);
            Symbol* s = table_.define(en->name, SymbolKind::Enum,
                                      Type::enumType(en->name), en->loc, moduleId);
            if (!s) break;
            // Enum üyeleri sadece int — diğer tiplere bağımlılık yok, burada doldur.
            auto& layout = table_.enumLayouts[en->name];
            for (auto& m : en->members)
                layout.push_back({m.name, m.value});
            break;
        }

        case ASTKind::FfiDecl: {
            // #229: requires grameri parse edilir (ADR-043 enforcement yok);
            // kullanılırsa DERLEME ZAMANI uyarısı — capability'ler kaldırıldı,
            // gereksinim yok sayılır. Runtime uyarısı yok (stdout kirlenmez).
            auto* fd = static_cast<FfiDeclNode*>(child);
            if (!fd->requiresCap.empty()) {
                diag_.report("W007", fd->loc,
                             "requires '" + fd->requiresCap +
                                 "' is ignored — capabilities were removed (ADR-043)",
                             "remove 'requires " + fd->requiresCap + "' from the declaration");
            }
            break;
        }

        case ASTKind::FunctionDecl: {
            // Stub: placeholder tip — pass1b'de gerçek imzayla güncellenecek.
            // define sadece ismin varlığını tescillemek için çağrılır.
            auto* fn = static_cast<FunctionDeclNode*>(child);
            if (!table_.define(fn->name, SymbolKind::Function,
                               Type::function(Type::Void(), {}), fn->loc, moduleId)) {
                // Çakışma BURADA raporlanmalı: pass1b `existing` bulduğunda
                // sembolü günceller ve hata vermez, yani ikinci tanım sessizce
                // birincinin yerine geçerdi. IR'de aynı isimde iki fonksiyon
                // oluşur (functionOrder'da iki kayıt, functions map'inde bir
                // tane) ve VM tutarsız duruma düşüp çökerdi.
                Symbol* ex = table_.resolve(fn->name);
                std::string hint = ex
                    ? "'" + fn->name + "' first defined at " + ex->definitionLoc.toString()
                    : "choose a different name";
                diag_.report("E002", fn->loc,
                             "'" + fn->name + "' already defined in this scope", hint);
            }
            break;
        }

        case ASTKind::VariableDecl: {
            // Modül-düzeyi değişkenler — tip adı primitive olabilir, güvenle kaydet.
            auto* vd = static_cast<VariableDeclNode*>(child);
            table_.define(vd->name, SymbolKind::Variable,
                          Type::fromName(vd->varType), vd->loc, moduleId);
            for (ASTNode* sib : vd->getChildren()) {
                if (sib->kind == ASTKind::VariableDecl) {
                    auto* sv = static_cast<VariableDeclNode*>(sib);
                    table_.define(sv->name, SymbolKind::Variable,
                                  Type::fromName(sv->varType), sv->loc, moduleId);
                }
            }
            break;
        }

        default:
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// pass1bResolveLayouts — struct alanlarını ve fonksiyon imzalarını çözümle.
// Tüm modüllerin pass1a'sı bittikten sonra çalışır; bu sayede çapraz modül
// tip isimleri (BStruct, Color enum vb.) zaten bilinmektedir.
// ─────────────────────────────────────────────────────────────────────────────

void SymbolCollector::pass1bResolveLayouts(ASTNode* program, int moduleId) {
    for (ASTNode* child : program->getChildren()) {
        switch (child->kind) {

        case ASTKind::FunctionDecl: {
            auto* fn = static_cast<FunctionDeclNode*>(child);
            std::vector<Type> paramTypes;
            for (auto* p : fn->params)
                paramTypes.push_back(typeFromName(p->varType, p->loc));
            Type retType = typeFromName(fn->returnType, fn->loc);

            // pass1a'da stub olarak tanımlandı; şimdi doğru tipini set et.
            // resolve() ile bul, sembolü güncelle (redefine yerine güncelleme).
            std::vector<std::string> paramNames;
            for (auto* p : fn->params)
                paramNames.push_back(p->name);

            Symbol* existing = table_.resolve(fn->name);
            if (existing && existing->kind == SymbolKind::Function) {
                existing->type       = Type::function(retType, paramTypes);
                existing->paramNames = paramNames;
            } else if (!existing) {
                // pass1a'da çakışma nedeniyle eklenmemişti — şimdi dene.
                Symbol* s = table_.define(fn->name, SymbolKind::Function,
                                          Type::function(retType, paramTypes),
                                          fn->loc, moduleId);
                if (s) s->paramNames = paramNames;
                if (!s) {
                    Symbol* ex_ = table_.resolve(fn->name);
                    std::string h_ = ex_ ? "'" + fn->name + "' first defined at " + ex_->definitionLoc.toString() : "choose a different name";
                    diag_.report("E002", fn->loc, "'" + fn->name + "' already defined in this scope", h_);
                }
            }
            break;
        }

        case ASTKind::StructDecl: {
            auto* st = static_cast<StructDeclNode*>(child);
            // Struct sembolü pass1a'da tanımlandı; layout'u şimdi doldur.
            for (ASTNode* fieldNode : st->getChildren()) {
                if (fieldNode->kind != ASTKind::VariableDecl) continue;
                auto* vd = static_cast<VariableDeclNode*>(fieldNode);
                Type ft = typeFromName(vd->varType, vd->loc);
                table_.structLayouts[st->name].push_back({vd->name, ft});
            }
            break;
        }

        case ASTKind::VariableDecl: {
            // Modül-düzeyi değişkenlerin struct/enum tiplerini düzelt.
            auto* vd = static_cast<VariableDeclNode*>(child);
            Symbol* s = table_.resolve(vd->name);
            if (s && s->type.isError())
                s->type = typeFromName(vd->varType, vd->loc);
            break;
        }

        default:
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// validateImports — import bildirimleri doğrula; semboller scope'a bağlansın
// ─────────────────────────────────────────────────────────────────────────────

void SymbolCollector::validateImports(ModuleGraph& graph) {
    // moduleId → filePath haritası (hızlı kaynak modül bulma için)
    std::unordered_map<int, std::string> idToPath;
    std::unordered_map<std::string, int> pathToId;
    for (auto& unit : graph.units) {
        idToPath[unit.moduleId] = unit.filePath;
        pathToId[unit.filePath] = unit.moduleId;
    }

    for (auto& unit : graph.units) {
        for (ASTNode* child : unit.ast->getChildren()) {
            if (child->kind != ASTKind::ImportDecl) continue;
            auto* imp = static_cast<ImportDeclNode*>(child);

            // ADR-034 (#107): tırnaksız import → gömülü FFI modülü, dosya
            // graph'ında aranmaz — FfiCatalog'a yönlendir.
            if (imp->isModuleName) {
                resolveFfiImport(imp);
                // moduleImports_: BU BİRİMDE görünen (yerel) adlar — rename
                // import sonrası kaynak ad değil, local kullanılır.
                for (const auto& n : imp->importedNames)
                    moduleImports_[unit.moduleId].insert(
                        n.local.empty() ? n.source : n.local);
                continue;
            }

            // #252: kaynak modül TAM canonical yol eşleşmesiyle bulunur.
            // Eskiden yol SONEKİ karşılaştırılıyordu: `"lib.sqt"` importu
            // grafikte önce gelen `mylib.sqt`'ye, `"sub/lib.sqt"` ise
            // `lib.sqt`'ye bağlanabiliyordu. Loader'ın çözdüğü yol yoksa
            // (graf loader dışında kurulduysa) aynı kuralla burada çözülür.
            std::string wantPath = imp->resolvedPath;
            if (wantPath.empty()) {
                std::error_code ec;
                std::filesystem::path p =
                    std::filesystem::path(unit.filePath).parent_path() / imp->sourcePath;
                wantPath = std::filesystem::weakly_canonical(p, ec).string();
                if (ec) wantPath = p.lexically_normal().string();
            }
            int sourceModuleId = -2; // -2 = bulunamadı
            for (auto& u : graph.units) {
                if (u.filePath == wantPath) {
                    sourceModuleId = u.moduleId;
                    break;
                }
            }

            if (sourceModuleId == -2) {
                // Loader zaten E_MODULE_NOT_FOUND üretmiştir — sessiz geç.
                continue;
            }

            // Her import edilen isim için doğrula. Rename import: `name` yerine
            // `n.source` (kaynak modülde export'u aranan ad), ekleme n.source
            // ürünü yerel ad (n.local).
            for (const auto& n : imp->importedNames) {
                const std::string& name = n.source;
                const std::string  localName = n.local.empty() ? n.source : n.local;
                Symbol* sym = table_.resolve(name);

                if (!sym) {
                    diag_.report("E_IMPORT_UNKNOWN", imp->loc,
                        "'" + name + "' not found in module '" + imp->sourcePath + "'");
                    continue;
                }

                if (sym->moduleId != sourceModuleId) {
                    diag_.report("E_IMPORT_UNKNOWN", imp->loc,
                        "'" + name + "' not found in module '" + imp->sourcePath + "'");
                    continue;
                }

                // isExported kontrolü: AST'den bak
                bool exported = false;
                for (ASTNode* src : graph.units[0].ast->getChildren()) {
                    // Doğru modülü bul ve export bayrağını kontrol et
                    (void)src; // aşağıda gerçek kontrol
                    break;
                }
                // Sembol export bayrağını Symbol'e taşıyoruz — şimdilik
                // sadece AST'den okuma yapabiliriz.
                for (auto& u : graph.units) {
                    if (u.moduleId != sourceModuleId) continue;
                    for (ASTNode* decl : u.ast->getChildren()) {
                        if (decl->kind == ASTKind::FunctionDecl) {
                            auto* fn = static_cast<FunctionDeclNode*>(decl);
                            if (fn->name == name) { exported = fn->isExported; break; }
                        } else if (decl->kind == ASTKind::StructDecl) {
                            auto* st = static_cast<StructDeclNode*>(decl);
                            if (st->name == name) { exported = st->isExported; break; }
                        } else if (decl->kind == ASTKind::EnumDecl) {
                            auto* en = static_cast<EnumDeclNode*>(decl);
                            if (en->name == name) { exported = en->isExported; break; }
                        } else if (decl->kind == ASTKind::VariableDecl) {
                            // #3: global değişken export'u — kardeş bildirimler
                            // (`export int a, b;`) de children'da VariableDecl olarak durur.
                            auto* vd = static_cast<VariableDeclNode*>(decl);
                            if (vd->name == name) { exported = vd->isExported; break; }
                            bool foundInSibling = false;
                            for (ASTNode* sibling : vd->getChildren()) {
                                if (auto* svd = dynamic_cast<VariableDeclNode*>(sibling)) {
                                    if (svd->name == name) {
                                        exported = svd->isExported;
                                        foundInSibling = true;
                                        break;
                                    }
                                }
                            }
                            if (foundInSibling) break;
                        }
                    }
                    break;
                }

                if (!exported) {
                    diag_.report("E_IMPORT_NOT_EXPORTED", imp->loc,
                        "'" + name + "' is defined in '" + imp->sourcePath + "' but not exported");
                    continue;
                }

                // Başarılı: bu ismi import eden modülün erişim listesine ekle
                // (yerel ad — rename import ile source farklı olabilir).
                moduleImports_[unit.moduleId].insert(localName);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveFfiImport — `import {sqrt} from math;` (ADR-034, #107)
// FfiCatalog'dan bildirimi bul, global scope'a hostFnId taşıyan Symbol tanımla.
// Sembol yalnızca import edildiğinde var olur → gating burada doğal sağlanır.
// ─────────────────────────────────────────────────────────────────────────────

void SymbolCollector::resolveFfiImport(ImportDeclNode* imp) {
    const FfiCatalog& catalog = FfiCatalog::instance();

    if (!catalog.hasModule(imp->sourcePath)) {
        diag_.report("E_IMPORT_UNKNOWN", imp->loc,
            "unknown module '" + imp->sourcePath + "'",
            "known embedded modules: math, core, fs, sys, date, process, stdin, stdout, stderr, path, utf8, os, terminal");
        return;
    }

    for (const auto& en : imp->importedNames) {
        // Rename import: kaynak ad katalogda aranır, yerel ad (boşsa kaynak)
        // bu birimde görünecek semboldür.
        const std::string& source = en.source;
        std::string        local  = en.local.empty() ? en.source : en.local;
        const FfiDeclNode* decl = catalog.lookup(imp->sourcePath, source);
        if (!decl) {
            diag_.report("E_IMPORT_UNKNOWN", imp->loc,
                "'" + source + "' not found in module '" + imp->sourcePath + "'");
            continue;
        }

        int32_t hostId = hostEntryIndex(decl->hostId);
        if (hostId < 0) {
            // root.sqt ↔ host_functions.cpp drift — geliştirici hatası.
            diag_.report("E_IMPORT_UNKNOWN", imp->loc,
                "internal: FFI host id '" + decl->hostId + "' has no registered implementation");
            continue;
        }

        if (Symbol* existing = table_.resolve(local)) {
            // Aynı host fonksiyonunun tekrar importu zararsızdır. Başka bir
            // şeye (yerleşik `print`, kullanıcı fonksiyonu, başka bir host
            // fonksiyonu) bağlı bir adı gölgelemek ise sessizce yok
            // sayılıyordu: `import {abs as print} from math;` sonrası
            // `print` hâlâ yerleşikti (#246).
            if (existing->hostFnId != hostId || existing->isBuiltin)
                diag_.report("E002", imp->loc,
                    "cannot import '" + source + "' as '" + local + "': the name '" + local +
                        "' is already defined",
                    "choose another local name: `import { " + source + " as other } from " +
                        imp->sourcePath + ";`");
            continue;
        }

        std::vector<Type> paramTypes;
        std::vector<std::string> paramNames;
        for (auto* p : decl->params) {
            paramTypes.push_back(typeFromName(p->varType, p->loc));
            paramNames.push_back(p->name);
        }
        Type retType = typeFromName(decl->returnType, decl->loc);

        Symbol* s = table_.define(local, SymbolKind::Function,
                                  Type::function(retType, paramTypes),
                                  decl->loc, ModuleRegistry::BUILTIN_ID);
        if (!s) continue;
        s->paramNames  = paramNames;
        s->hostFnId    = hostId;
        s->ffiModule   = imp->sourcePath;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// pass2Bodies — fonksiyon gövdelerini gez; isim çözümle + referans topla
// (moduleId parametresi eklendi; iç mantık aynı)
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// checkStructCycles — E010 döngüsel struct (by-value çevrim → sonsuz boyut)
// ─────────────────────────────────────────────────────────────────────────────

void SymbolCollector::checkStructCycles() {
    // ADR-020: struct alanları referanstır, bellek düzeninde sonsuz boyut
    // yoktur. Ama non-nullable bir struct alanı sahibiyle birlikte ÖRNEKLENİR
    // (initNestedStructFields) ve null olamaz: `struct Node { Node next; }`
    // sonsuz bir örnekleme zinciri ister. IR zinciri keserek alanı null
    // bırakıyordu — tip "non-null" derken değer null'dı ve erişim çalışma
    // zamanında düşüyordu (#255). Döngü non-nullable alanlardan geçiyorsa
    // E010: en az bir halka `T?` olmalı (bağlı liste/ağaç böyle kurulur).
    enum class Mark { None, Active, Done };
    std::unordered_map<std::string, Mark> mark;
    std::vector<std::string> stack;

    auto fieldLoc = [&](const std::string& owner, const std::string& field) {
        auto it = structDecls_.find(owner);
        if (it != structDecls_.end()) {
            for (ASTNode* ch : it->second->getChildren())
                if (auto* vd = dynamic_cast<VariableDeclNode*>(ch))
                    if (vd->name == field)
                        return vd->loc;
            return it->second->loc;
        }
        return SourceLocation{};
    };

    std::function<void(const std::string&)> visit = [&](const std::string& name) {
        mark[name] = Mark::Active;
        stack.push_back(name);
        auto lay = table_.structLayouts.find(name);
        if (lay != table_.structLayouts.end()) {
            for (const auto& [fieldName, fieldType] : lay->second) {
                if (!fieldType.isStruct() || fieldType.nullable)
                    continue;   // dizi alanı boş dizi, `T?` null başlar — döngü kırılır
                const std::string& target = fieldType.structName;
                Mark m = mark.count(target) ? mark[target] : Mark::None;
                if (m == Mark::Active) {
                    std::string chain;
                    bool on = false;
                    for (const auto& s : stack) {
                        if (s == target) on = true;
                        if (on) chain += s + " → ";
                    }
                    chain += target;
                    diag_.report("E010", fieldLoc(name, fieldName),
                        "struct '" + target + "' contains itself through non-nullable fields (" +
                            chain + ")",
                        "make the field nullable so the chain can end: `" + target + "? " +
                            fieldName + ";`");
                } else if (m == Mark::None) {
                    visit(target);
                }
            }
        }
        stack.pop_back();
        mark[name] = Mark::Done;
    };

    for (const auto& [name, layout] : table_.structLayouts) {
        (void)layout;
        if (!mark.count(name) || mark[name] == Mark::None)
            visit(name);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// pass2Bodies — fonksiyon gövdelerini gez; isim çözümle + referans topla
// ─────────────────────────────────────────────────────────────────────────────

void SymbolCollector::pass2Bodies(ASTNode* program, int moduleId) {
    currentModuleId_ = moduleId;
    for (ASTNode* child : program->getChildren()) {
        switch (child->kind) {

        case ASTKind::FunctionDecl: {
            auto* fn = (FunctionDeclNode*)child;
            table_.enterScope();
            // parametreleri tanımla
            for (auto* p : fn->params) {
                Symbol* s = table_.define(p->name, SymbolKind::Parameter,
                                         typeFromName(p->varType, p->loc), p->loc);
                if (!s) {
                    Symbol* ex_ = table_.resolve(p->name);
                    std::string h_ = ex_ ? "'" + p->name + "' first defined at " + ex_->definitionLoc.toString() + " — choose a different name" : "choose a different name";
                    diag_.report("E002", p->loc, "parameter '" + p->name + "' already defined", h_);
                }
            }
            // walk body (children[0] = BlockNode)
            auto& ch = fn->getChildren();
            if (!ch.empty()) walkStmt(ch[0]);
            table_.exitScope();
            break;
        }

        case ASTKind::VariableDecl: {
            // global değişken başlatıcısı (declare-before-use)
            auto* vd = (VariableDeclNode*)child;
            if (vd->initExpr) walkExpr(vd->initExpr);
            // TODO(faz2): global init sırası kontrolü (fibonacci'de global var yok)
            for (ASTNode* sib : vd->getChildren()) {
                if (sib->kind == ASTKind::VariableDecl) {
                    auto* sv = (VariableDeclNode*)sib;
                    if (sv->initExpr) walkExpr(sv->initExpr);
                }
            }
            break;
        }

        case ASTKind::StructDecl:
        case ASTKind::EnumDecl:
            break; // pass2'de gövde gezme gerekmez

        default:
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// walkStmt — ifade bloğunu gez
// ─────────────────────────────────────────────────────────────────────────────

void SymbolCollector::walkStmt(ASTNode* node) {
    if (!node) return;

    switch (node->kind) {

    case ASTKind::Block: {
        table_.enterScope();
        for (ASTNode* child : node->getChildren()) walkStmt(child);
        table_.exitScope();
        break;
    }

    case ASTKind::VariableDecl: {
        auto* vd = (VariableDeclNode*)node;
        // First walk initializer (prevent self-reference)
        if (vd->initExpr) walkExpr(vd->initExpr);
        // Then define
        Symbol* s = table_.define(vd->name, SymbolKind::Variable,
                                  typeFromName(vd->varType, vd->loc), vd->loc);
        if (!s) {
            Symbol* ex_ = table_.resolve(vd->name);
            std::string h_ = ex_ ? "'" + vd->name + "' first defined at " + ex_->definitionLoc.toString() + " — choose a different name" : "choose a different name";
            diag_.report("E002", vd->loc, "'" + vd->name + "' already defined in this scope", h_);
        }
        // Sibling VariableDecl's (int a, b;) — TODO: not in fibonacci
        for (ASTNode* sib : vd->getChildren()) {
            if (sib->kind == ASTKind::VariableDecl) {
                auto* sv = (VariableDeclNode*)sib;
                if (sv->initExpr) walkExpr(sv->initExpr);
                Symbol* ss = table_.define(sv->name, SymbolKind::Variable,
                                          typeFromName(sv->varType, sv->loc), sv->loc);
                if (!ss) {
                    Symbol* ex_ = table_.resolve(sv->name);
                    std::string h_ = ex_ ? "'" + sv->name + "' first defined at " + ex_->definitionLoc.toString() + " — choose a different name" : "choose a different name";
                    diag_.report("E002", sv->loc, "'" + sv->name + "' already defined in this scope", h_);
                }
            }
        }
        break;
    }

    case ASTKind::IfStatement: {
        auto* ifn = (IfStatementNode*)node;
        if (ifn->condition)  walkExpr(ifn->condition);
        if (ifn->thenBranch) walkStmt(ifn->thenBranch);
        if (ifn->elseBranch) walkStmt(ifn->elseBranch);
        break;
    }

    case ASTKind::WhileStatement: {
        auto* ws = (WhileStatementNode*)node;
        if (ws->condition) walkExpr(ws->condition);
        if (ws->body)      walkStmt(ws->body);
        break;
    }

    case ASTKind::ForStatement: {
        auto* fs = (ForStatementNode*)node;
        table_.enterScope(); // init değişkeni döngüye ait
        if (fs->init) {
            if (fs->init->kind == ASTKind::VariableDecl) walkStmt(fs->init);
            else walkExpr(fs->init);
        }
        if (fs->condition) walkExpr(fs->condition);
        if (fs->update)    walkExpr(fs->update);
        if (fs->body)      walkStmt(fs->body);
        table_.exitScope();
        break;
    }

    case ASTKind::DoWhileStatement: {
        auto* dw = (DoWhileStatementNode*)node;
        if (dw->body)      walkStmt(dw->body);
        if (dw->condition) walkExpr(dw->condition);
        break;
    }

    case ASTKind::ReturnStatement: {
        auto* rs = (ReturnStatementNode*)node;
        if (rs->value) walkExpr(rs->value);
        break;
    }

    case ASTKind::ExpressionStatement: {
        auto* es = (ExpressionStatementNode*)node;
        if (es->expression) walkExpr(es->expression);
        break;
    }

    case ASTKind::BreakStatement:
    case ASTKind::ContinueStatement:
        break; // yaprak

    // ADR-025: try { body } catch (Error e) { handler }
    case ASTKind::TryStatement: {
        auto* ts = (TryStatementNode*)node;
        if (ts->body) walkStmt(ts->body);
        // catch değişkeni catch bloğu kapsamında görünür
        if (ts->handler) {
            table_.enterScope();
            if (!ts->catchVar.empty())
                table_.define(ts->catchVar, SymbolKind::Variable,
                              Type::structType("Error"), {});
            walkStmt(ts->handler);
            table_.exitScope();
        }
        break;
    }

    // ADR-025: throw <ifade>;
    case ASTKind::ThrowStatement: {
        auto* th = (ThrowStatementNode*)node;
        if (th->value) walkExpr(th->value);
        break;
    }

    // ADR-027: switch (expr) { case v: ... default: ... }
    case ASTKind::SwitchStatement: {
        auto* sw = (SwitchStatementNode*)node;
        if (sw->subject) walkExpr(sw->subject);
        for (auto& clause : sw->cases) {
            for (auto* val : clause.values) walkExpr(val);
            for (auto* s   : clause.body)   walkStmt(s);
        }
        break;
    }

    default:
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// walkExpr — ifade ağacında isimleri çöz
// ─────────────────────────────────────────────────────────────────────────────

void SymbolCollector::walkExpr(ASTNode* node) {
    if (!node) return;

    switch (node->kind) {

    case ASTKind::Identifier: {
        auto* id = (IdentifierNode*)node;
        if (!id->parserToken.token) break;
        std::string name = id->parserToken.token->token;
        Symbol* s = table_.resolve(name);
        if (s) {
            id->resolvedSymbol = s;
            table_.addReference(s, id->loc);

            // Modül sınır kontrolü: başka modülden gelen sembol import edilmeli.
            // Builtin'ler (moduleId=0) ve aynı modül muaf.
            if (s->moduleId != currentModuleId_ &&
                s->moduleId != ModuleRegistry::BUILTIN_ID &&
                s->moduleId != ModuleRegistry::INVALID_ID &&
                currentModuleId_ != ModuleRegistry::INVALID_ID &&
                currentModuleImports_.find(name) == currentModuleImports_.end()) {
                diag_.report("E_SYMBOL_NOT_IMPORTED", id->loc,
                    "'" + name + "' is from another module and must be imported explicitly");
            }
            // #246: FFI sembolü yalnız onu import eden modülde görünür.
            // Eskiden bir modülün `import {sqrt} from math;` satırı programın
            // TÜM modüllerinde `sqrt`'ü kullanılabilir yapıyordu.
            else if (s->hostFnId >= 0 && !s->isBuiltin &&
                     currentModuleId_ != ModuleRegistry::INVALID_ID &&
                     currentModuleImports_.find(name) == currentModuleImports_.end()) {
                diag_.report("E_SYMBOL_NOT_IMPORTED", id->loc,
                    "'" + name + "' comes from the '" + s->ffiModule +
                        "' module and must be imported in this file",
                    "add `import { " + name + " } from " + s->ffiModule + ";`");
            }
        } else {
            std::vector<std::string> cands_;
            for (auto* sym_ : table_.allSymbols()) cands_.push_back(sym_->name);
            std::string sug_ = suggestName(name, cands_);
            std::string h_ = sug_.empty()
                ? "define it before use: `int " + name + " = 0;` (set type and value)"
                : "did you mean: `" + sug_ + "`?";
            diag_.report("E001", id->loc, "'" + name + "' is not defined", h_, (int)name.size());
        }
        break;
    }

    case ASTKind::BinaryExpression: {
        auto* bin = (BinaryExpressionNode*)node;
        if (bin->Left)  walkExpr(bin->Left);
        if (bin->Right) walkExpr(bin->Right);
        break;
    }

    case ASTKind::Call: {
        auto* call = (CallExpressionNode*)node;
        if (call->callee) walkExpr(call->callee);
        for (ASTNode* arg : call->arguments) walkExpr(arg);
        break;
    }

    case ASTKind::Postfix: {
        auto* pf = (PostfixNode*)node;
        if (pf->operand) walkExpr(pf->operand);
        break;
    }

    case ASTKind::MemberAccess: {
        auto* ma = (MemberAccessNode*)node;
        if (ma->object) walkExpr(ma->object); // member çözümü Faz 3 → TODO
        break;
    }

    case ASTKind::IndexExpression: {
        auto* ie = (IndexExpressionNode*)node;
        if (ie->object) walkExpr(ie->object);
        if (ie->index)  walkExpr(ie->index);
        break;
    }

    case ASTKind::ArrayLiteral: {
        auto* al = (ArrayLiteralNode*)node;
        for (auto* e : al->elements) walkExpr(e);
        break;
    }

    case ASTKind::ScopeCall: {
        auto* sc = (ScopeCallNode*)node;
        for (ASTNode* arg : sc->arguments) walkExpr(arg);
        break;
    }

    case ASTKind::CastExpression: {  // ADR-026
        auto* cast = (CastExpressionNode*)node;
        if (cast->operand) walkExpr(cast->operand);
        break;
    }

    case ASTKind::Literal:
        break; // yaprak

    default:
        break;
    }
}
