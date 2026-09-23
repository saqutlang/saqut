// ============================================================================
// saQut Compiler — Bildirim Düğümleri
// ============================================================================
//
// DİZİN:   src/parser/nodes/declarations.hpp
// KATMAN:  Katman 3 — FunctionDecl, StructDecl, EnumDecl, VariableDecl, ImportDecl
//
// AMAÇ:
//   Dilin bildirim yapılarını temsil eden AST düğümleri. Her biri bir
//   program öğesini (fonksiyon, struct, enum, değişken, import) tanımlar.
//
// ============================================================================

#ifndef SAQUT_AST_DECL
#define SAQUT_AST_DECL

#include "parser/ast_node.hpp"

class VariableDeclNode; // fwd — FunctionDeclNode::params için

class FunctionDeclNode : public ASTNode {
public:
    std::string name;
    std::string returnType;
    std::vector<VariableDeclNode*> params;
    bool isExported = false;
    FunctionDeclNode();
    ~FunctionDeclNode() override;
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

class VariableDeclNode : public StatementNode {
public:
    std::string varType;
    std::string name;
    ASTNode*   initExpr = nullptr;
    bool isExported = false; // yalnızca modül seviyesi global için anlamlı (#3)
    VariableDeclNode();
    ~VariableDeclNode() override { delete initExpr; }
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

class StructDeclNode : public ASTNode {
public:
    std::string name;
    bool isExported = false;
    StructDeclNode();
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

struct EnumMember {
    std::string name;
    int         value = 0;
};

class EnumDeclNode : public ASTNode {
public:
    std::string              name;
    std::vector<EnumMember>  members;
    bool isExported = false;
    EnumDeclNode();
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

// import {name1 as local1, name2} from "file.sqt";
// `as localN` isteğe bağlıdır: kaynak adının bu modülde GÖRÜNECEĞİ yerel ad.
// Belirtilmezse yerel ad = kaynak adı (eski davranış). ADR-034 esneklik:
// `import {exists as fileExists} from fs` — global isim çarpışmalarını
// kullanıcı kendi import'unda çözer; derleyici isimlendirme dayatmaz.
class ImportDeclNode : public ASTNode {
public:
    // Çift vektör yerine tek vektör<ImportName>: her öğe kaynak adı + yerel ad.
    struct ImportName {
        std::string source;   // modülde/dosyada gerçek ad ("exists")
        std::string local;    // bu birimde görünecek ad ("fileExists"); boşsa = source
    };
    std::vector<ImportName> importedNames;   // {"exists"→"fileExists", ...}
    std::string              sourcePath;     // "math.sqt" (dosya) veya "fs" (modül)
    // ADR-034 (#107): tırnaklı kaynak = dosya yolu; tırnaksız ad = gömülü/
    // çözümlenen modül. Parser bu ayrımı işaretler ki loader doğru çözsün.
    bool                     isModuleName = false;
    // ModuleLoader'ın çözdüğü canonical mutlak yol (dosya importu). Sembol
    // toplayıcı kaynak modülü bununla TAM eşleşmeyle bulur (#252).
    std::string              resolvedPath;
    ImportDeclNode();
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

// ADR-034 (#107): gömülü host fonksiyon bildirimi. Gövdesiz; yalnız gömülü
// root.sqt'te geçerli (kullanıcı kodunda policy ile reddedilir).
//   ffi <ret> <ad>(<params>) : <HOST_ID> from <mod> [requires <cap>];
class FfiDeclNode : public ASTNode {
public:
    std::string                    name;         // "readFile"
    std::string                    returnType;   // "string"
    std::vector<VariableDeclNode*> params;       // (string path)
    std::string                    hostId;       // "FS_READFILE" (sembolik)
    std::string                    moduleName;   // "fs"
    std::string                    requiresCap;  // "fs" (boş = capability'siz)
    FfiDeclNode();
    ~FfiDeclNode() override;
    void log(int indent = 0) override;
    std::string toJson(int depth = 0) override;
};

#endif
