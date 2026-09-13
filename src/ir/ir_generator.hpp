// ============================================================================
// saQut IR — IRGenerator (AST → IR Dönüşümü)
//
// AST'yi (parse edilmiş kaynak kodu) Instruction listelerine çevirir.
// Her fonksiyon için bir IRFunction üretir, hepsini IRProgram'a toplar.
//
// SLOT ATAMA STRATEJİSİ:
//   - Her fonksiyon üretiminde nextSlot_ sıfırdan başlar.
//   - Parametreler 0, 1, 2, ... slotlarına sırayla atanır.
//   - Sonraki her değişken veya geçici sonuç freshSlot() ile yeni slot alır.
//   - Slotlar asla geri verilmez (basitlik öncelikli).
//   - Fonksiyon bitince nextSlot_ = slotCount.
//
// SINIRLAMALAR (fibonacci için yeterli, genel dil için TODO):
//   - Aynı isimli iki değişken farklı iç kapsamlarda olsa bile çakışır.
//     (fibonacci.sqt'de bu durum yok; gelecekte scope-aware slot atama gerekir.)
//   - Sadece int değerler desteklenir (Value.kind şu an hep Int).
// ============================================================================

#ifndef SAQUT_IR_GENERATOR
#define SAQUT_IR_GENERATOR

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>
#include "ir/ir_program.hpp"
#include "symbol/symbol_table.hpp"
#include "core/array_elem_kind.hpp"
#include "core/type.hpp"
#include "core/location.hpp"
#include "parser/ast_node.hpp"
#include "module/module_graph.hpp"

class FunctionDeclNode;  // finalizeSlotTypes imzası için (tanım .cpp'de)

class IRGenerator {
public:
    // Tek dosya — geriye dönük uyumluluk
    IRProgram generate(ASTNode* programNode, SymbolTable& symbolTable,
                       const std::string& sourceFilePath = "");

    // Çok dosya: tüm modülleri tek IRProgram'a derle
    IRProgram generateModuleGraph(ModuleGraph& graph, SymbolTable& symbolTable);

private:
    // ── Fonksiyon üretimi ─────────────────────────────────────────────────
    void generateFunction(ASTNode* functionDeclNode);

    // ── Deyim (statement) üretimi — talimat listesine yazar ──────────────
    void generateStatement(ASTNode* node);

    // ── İfade (expression) üretimi — sonucun slotunu döndürür ────────────
    // Sonuç her zaman bir slotta bulunur. Identifier zaten bir slotta,
    // hesaplamalar freshSlot() ile yeni slot alır.
    int generateExpression(ASTNode* node);

    // ── İkili operatör (binary op) için ortak yardımcı ───────────────────
    // resultNode: ifadenin KENDİ düğümü — sonuç tipi ondan okunur (byte
    // sarması için, bkz. ADR-040 Faz 4). Operandların tipinden çıkarılamaz:
    // literal taraf bağlamsal olarak byte tiplenebilir ama ifade int olabilir.
    int generateBinaryArithmetic(Opcode opcode, ASTNode* leftNode, ASTNode* rightNode,
                                 int line = 0, int col = 0, ASTNode* resultNode = nullptr);

    // ── Slot yönetimi ─────────────────────────────────────────────────────
    int  freshSlot();                           // Yeni slot numarası al (nextSlot_++)
    void registerVariable(const std::string& name, int slot); // name → slot kaydı
    int  lookupVariable(const std::string& name);             // name → slot (bulunamazsa hata)

    // ── Talimat yazma yardımcıları ────────────────────────────────────────
    // Talimatları currentFunction_->instructions'a ekler.

    void emitLoadConst(int destSlot, int value,
                       const SourceLocation& loc = {});
    void emitLoadFloat(int destSlot, double value,
                       const SourceLocation& loc = {});
    void emitIntToFloat(int destSlot, int srcSlot,
                        const SourceLocation& loc = {});
    void emitLoadFloat32(int destSlot, double value,
                         const SourceLocation& loc = {});
    void emitLoadLong(int destSlot, long long value,
                      const SourceLocation& loc = {});
    void emitIntToFloat32(int destSlot, int srcSlot,
                          const SourceLocation& loc = {});
    void emitIntToLong(int destSlot, int srcSlot,
                       const SourceLocation& loc = {});
    void emitLoadDecimal(int destSlot, const DecimalValue& value,
                         const SourceLocation& loc = {});
    void emitIntToDecimal(int destSlot, int srcSlot,
                          const SourceLocation& loc = {});
    void emitFloatToDecimal(int destSlot, int srcSlot,
                            const SourceLocation& loc = {});
    void emitLoadSlot(int destSlot, int srcSlot,
                      const SourceLocation& loc = {});
    void emitLoadGlobal(int destSlot, int globalIndex,
                        const SourceLocation& loc = {});
    void emitStoreGlobal(int srcSlot, int globalIndex,
                         const SourceLocation& loc = {});
    // ADR-021 zero-init: `T?` slot'u Int(0) değil null başlar.
    void emitLoadNull(int destSlot, const SourceLocation& loc = {});
    // #184 kararı: non-nullable `string` zero-init "" başlar (Int 0 değil).
    void emitLoadString(int destSlot, std::string value,
                        const SourceLocation& loc = {});
    void emitStructNew(int destSlot, const std::string& structType,
                       int fieldCount, const SourceLocation& loc = {});
    void emitFieldGet(int destSlot, int objSlot, int fieldIdx,
                      const SourceLocation& loc = {},
                      SlotType valueType = SlotType::Unknown,
                      bool valueNullable = false);
    void emitFieldSet(int objSlot, int fieldIdx, int valSlot,
                      int line = 0, int col = 0);
    // #206: arrayElemKind parametresi eklendi — packed primitive array'ler için
    void emitArrayNew(int destSlot, int capacity, ArrayElemKind k,
                      const SourceLocation& loc = {});
    void emitArrayGet(int destSlot, int arrSlot, int idxSlot,
                      int line = 0, int col = 0,
                      SlotType valueType = SlotType::Unknown,
                      ArrayElemKind elemKind = ArrayElemKind::Ref);
    void emitArraySet(int arrSlot, int idxSlot, int valSlot,
                      int line = 0, int col = 0,
                      ArrayElemKind elemKind = ArrayElemKind::Ref);
    void emitArrayLen(int destSlot, int arrSlot,
                      const SourceLocation& loc = {});
    void emitBinaryOp(Opcode op, int destSlot, int leftSlot, int rightSlot,
                      int line = 0, int col = 0);
    // byte ⊕ byte sonucunu 8 bite sarar (& 0xFF) — ADR-040 Faz 4.
    // Gerekçe ve tip-içi/tipler-arası ayrımı gerçeklemede (ir_generator.cpp).
    int  emitByteWrap(int valueSlot, int line = 0, int col = 0);
    void emitReturn(int srcSlot, int line = 0, int col = 0);
    // Koşulsuz atlama yazar; instruction indeksini döndürür (backpatch için).
    // Hedef bilinmiyorsa -1 geçilir, patchJump() ile doldurulur.
    int  emitJumpUnconditional(int targetInstrIndex);

    // JIF_FALSE talimatını -1 hedefle yazar, instruction indeksini döndürür.
    // Döndürülen indeks ileride patchJump() ile doldurulur (backpatch).
    int  emitJumpIfFalse(int condSlot);

    // JIF_TRUE talimatını -1 hedefle yazar, instruction indeksini döndürür.
    int  emitJumpIfTrue(int condSlot);

    // Daha önce -1 hedefle yazılan jump'ın hedefini şu anki pozisyona doldur.
    void patchJump(int instrIndex);

    // Şu an kaç talimat üretildi? (jump hedefi belirlemek için)
    int currentInstrIndex() const;

    // ── Döngü bağlamı yığını — break/continue hedefleri ─────────────────
    // Her döngüye girerken bir giriş push'lanır, çıkınca pop'lanır.
    // İç içe döngülerde en üstteki giriş en içteki döngüye aittir.
    struct LoopContext {
        bool             isSwitch = false;  // true → switch; continue buraya ait değil
        std::vector<int> breakJumps;    // patch bekleyen break JMP indeksleri
        std::vector<int> continueJumps; // patch bekleyen continue JMP indeksleri (switch'te boş)
    };
    std::vector<LoopContext> loopContextStack_;

    // ── Per-function üretim durumu ────────────────────────────────────────
    IRFunction* currentFunction_  = nullptr;               // şu an üretilen fonksiyon
    int         nextSlot_         = 0;                     // sıradaki boş slot numarası
    int         currentModuleId_  = ModuleRegistry::INVALID_ID; // registry ID

    // Faz 5: şu an üretilen düğümün kaynak konumu — tüm emit'ler buradan
    // sourceLine/sourceCol alır. Her generateStatement/generateExpression
    // girişinde node->loc ile güncellenir.
    SourceLocation currentLoc_;

    // Faz 5: explicitLoc geçerli değilse currentLoc_ döndürür.
    SourceLocation effectiveLoc(const SourceLocation& explicitLoc) const {
        return explicitLoc.isValid() ? explicitLoc : currentLoc_;
    }

    // Değişken ismi → slot numarası (lokal).
    std::unordered_map<std::string, int> nameToSlot_;

    // #108: blok-scope shadowing düzeltmesi. Her Block girişinde bir kayıt
    // push'lanır; registerVariable üstüne yazacağı ismin ÖNCEKİ durumunu
    // (var olan slot ya da nullopt = yoktu) en üstteki kayda ekler. Block
    // çıkışında kayıt geriye doğru uygulanıp nameToSlot_ eski haline getirilir.
    std::vector<std::vector<std::pair<std::string, std::optional<int>>>> shadowStack_;
    void pushScope();
    void popScope();

    // Global değişken ismi → global index
    std::unordered_map<std::string, int> nameToGlobal_;
    std::unordered_map<int, SlotType> globalSlotTypes_;
    int                                  globalCount_ = 0;

    // Struct alan düzeni: struct adı → sıralı [(alan adı, Type)] listesi
    // Sembol tablosundan generate() başında kopyalanır.
    std::unordered_map<std::string, std::vector<std::pair<std::string, Type>>> structLayouts_;

    // Enum üye düzeni: enum adı → sıralı [(üye adı, int değer)] listesi
    std::unordered_map<std::string, std::vector<std::pair<std::string, int>>> enumLayouts_;

    int getStructFieldIndex(const std::string& structType, const std::string& fieldName) const;
    int getStructFieldCount(const std::string& structType) const;

    // VarDecl anında struct-tipli alanları özyinelemeli olarak tahsis edip bağlar.
    // (ADR-020 nested struct kuralı: iç struct'lar STRUCT_NEW zinciriyle oluşturulur)
    // Nullable alanlar (`T? x`) null kalır — örneklenmez; özyinelemeli
    // struct'lar (`struct Node { Node? next; }`) bu sayede sonlanır.
    // activeChain iç kullanım: ziyaret edilen struct zinciri (döngü savunması);
    // dış çağrılar nullptr geçer.
    void initNestedStructFields(int destSlot, const std::string& structType,
                                const SourceLocation& loc,
                                std::vector<std::string>* activeChain = nullptr);

    bool isGlobal(const std::string& name) const;
    int  getGlobalIndex(const std::string& name) const;

    // ── Slot tipi hesaplama (Dilim 1.5, MIRPLAN §3) ──────────────────────
    // Fonksiyon adı → dönüş türü. Gövdeler üretilmeden önce tüm FunctionDecl'
    // lerin dönüş tipinden doldurulur (CALL sonuç slot'unun türü için).
    std::unordered_map<std::string, SlotType> funcReturnKind_;
    std::unordered_map<std::string, bool> funcReturnNullable_;

    // Bir tip adını ("float"/"int"/struct/array...) SlotType'a eşler.
    SlotType slotTypeFromTypeName(const std::string& typeName) const;
    SlotType slotTypeFromType(const Type& t) const;

    // Fonksiyon gövdesi bittikten sonra (slotCount kesinleştiğinde) çağrılır:
    // slotTypes'ı doldurur — parametreler bildirilen tipten, geri kalan
    // slotlar üreten opcode'dan (fixpoint tarama; LOAD_SLOT propagasyonu,
    // CALL dönüş türü). Ekstra semantik analiz DEĞİL — opcode başına sonuç
    // türü statik (VM'in ValueKind mantığının derleme-zamanı karşılığı).
    void finalizeSlotTypes(IRFunction* fn, FunctionDeclNode* decl);
};

#endif // SAQUT_IR_GENERATOR
