// ============================================================================
// saQut IR — IRGenerator (AST → IR) Gerçeklemesi
// ============================================================================
//
// DİZİN:   src/ir/ir_generator.cpp
// KATMAN:  IR — AST'yi dolaşarak 3-adresli IR talimatları üretir
//
// AMAÇ:
//   AST'deki her düğüm tipi için uygun Instruction'ları emit eder.
//   Slot tahsisi, satır tablosu doldurma, backpatch yönetimi.
//
// ============================================================================

#include "ir/ir_generator.hpp"
#include "ir/ir_cfg.hpp"

#include "data/data_registry.hpp"
#include "ffi/host_registry.hpp"
#include "parser/nodes/binary_expr.hpp"
#include "parser/nodes/declarations.hpp"
#include "parser/nodes/expressions.hpp"
#include "parser/nodes/identifier.hpp"
#include "parser/nodes/literal.hpp"
#include "parser/nodes/program.hpp"
#include "parser/nodes/statements.hpp"
#include "tokenizer/token.hpp"

#include <climits>   // LLONG_MIN (#219 A2: longint en küçük değer literal'i)
#include <stdexcept>
#include <string>

// #206: forward declarations — static helper'lar kullanımdan önce bildirilir
static ArrayElemKind arrayElemKindFromTypeName(const std::string& t);
static ArrayElemKind arrayElemKindFromPrim(PrimitiveKind p);
static ArrayElemKind arrayElemKindFromType(const Type& t);

// Error struct alan sırası (ADR-025): makeError için IR tarafından bilinir
// 0=line, 1=col, 2=message, 3=trace, 4=code
static constexpr int ERROR_FIELD_COUNT = 5;

// ─────────────────────────────────────────────────────────────────────────────
// generate — Ana giriş noktası
// ─────────────────────────────────────────────────────────────────────────────

IRProgram IRGenerator::generateModuleGraph(ModuleGraph& graph, SymbolTable& symbolTable) {
    IRProgram program;
    structLayouts_ = symbolTable.structLayouts;
    enumLayouts_ = symbolTable.enumLayouts;

    // Ön-geçiş (#3 düzeltmesi): TÜM modüllerdeki global değişkenleri ve
    // fonksiyon dönüş türlerini, HERHANGİ bir fonksiyon gövdesi üretilmeden
    // önce topla. nameToGlobal_ programın tamamı için TEK düz ad alanı
    // (import yalnızca görünürlük kısıtlar, ayrı ad alanı yaratmaz — sembol
    // tablosuyla tutarlı, ADR-031/034). Bu ön-geçiş olmadan: (a) main'in
    // bulunduğu modülden FARKLI bir modülde tanımlı export edilmiş global
    // hiçbir zaman ilklendirilmiyordu (yalnızca main'i barındıran modülün
    // KENDİ globalVars'ı main'e enjekte ediliyordu — `graph.units[0]` her
    // zaman giriş dosyası/main'in modülü, bağımlılıklar SONRA yüklenir,
    // dolayısıyla export edilmiş bir global her zaman main'den SONRA
    // işlenen bir modülde kalıp hiç koşmuyordu); (b) main'den önce işlenen
    // bir modül başka (henüz işlenmemiş) modülün fonksiyonunu çağırırsa
    // dönüş türü bilinmiyordu.
    std::vector<VariableDeclNode*> allGlobalVars;
    nameToGlobal_.clear();
    globalSlotTypes_.clear();
    for (auto& unit : graph.units) {
        program.moduleRegistry.intern(unit.filePath);
        int moduleGlobalCount = 0;
        for (ASTNode* child : unit.ast->getChildren()) {
            if (child->kind == ASTKind::VariableDecl) {
                auto* vd = static_cast<VariableDeclNode*>(child);
                nameToGlobal_[vd->name] = globalCount_++;
                globalSlotTypes_[nameToGlobal_[vd->name]] = slotTypeFromTypeName(vd->varType);
                program.globalCount++;
                program.globalNames.push_back(vd->name);
                allGlobalVars.push_back(vd);
                ++moduleGlobalCount;
            } else if (child->kind == ASTKind::FunctionDecl) {
                auto* fnDecl = static_cast<FunctionDeclNode*>(child);
                funcReturnKind_[fnDecl->name] = slotTypeFromTypeName(fnDecl->returnType);
                funcReturnNullable_[fnDecl->name] = !fnDecl->returnType.empty() &&
                                                     fnDecl->returnType.back() == '?';
            }
        }
        program.moduleGlobalCounts[unit.moduleId] = moduleGlobalCount;
    }

    for (auto& unit : graph.units) {
        currentModuleId_ = unit.moduleId;

        // Fonksiyonları üret
        for (ASTNode* child : unit.ast->getChildren()) {
            if (child->kind != ASTKind::FunctionDecl)
                continue;
            auto* fnDecl = static_cast<FunctionDeclNode*>(child);
            nameToSlot_.clear();
            shadowStack_.clear();
            nextSlot_ = 0;

            IRFunction irFn(fnDecl->name, (int) fnDecl->params.size());
            irFn.moduleId = currentModuleId_;
            program.addFunction(std::move(irFn));
            currentFunction_ = program.findFunction(fnDecl->name);

            if (fnDecl->name == "main") {
                // main'i barındıran modülden bağımsız — TÜM modüllerin
                // global başlangıç ifadeleri burada, ön-geçişteki (graph.units)
                // sırayla çalıştırılır.
                for (VariableDeclNode* gv : allGlobalVars) {
                    if (gv->initExpr) {
                        int initSlot = generateExpression(gv->initExpr);
                        emitStoreGlobal(initSlot, nameToGlobal_[gv->name]);
                    } else if (!gv->varType.empty() && gv->varType.back() == '?') {
                        // ADR-021 null kapısı: init'siz nullable global null
                        // başlar (local emitLoadNull ile aynı sözleşme). Aksi
                        // halde slot Value{} (= Int 0) kalır ve `q == null`
                        // hiç atanmamış değer için sessizce false döner —
                        // null kapısının kendisi yalan söyler.
                        int initSlot = freshSlot();
                        emitLoadNull(initSlot, SourceLocation{});
                        emitStoreGlobal(initSlot, nameToGlobal_[gv->name]);
                    } else if (gv->varType == "string") {
                        // #184 ürün kararı: init'siz non-nullable global
                        // string "" başlar (Int 0 değil) — local/alan ile
                        // aynı sözleşme. `string?` yukarıda null alır.
                        int initSlot = freshSlot();
                        emitLoadString(initSlot, "", SourceLocation{});
                        emitStoreGlobal(initSlot, nameToGlobal_[gv->name]);
                    }
                }
            }

            generateFunction(child);
            currentFunction_->slotCount = nextSlot_;
            finalizeSlotTypes(currentFunction_, fnDecl);
        }
    }
    return program;
}

IRProgram IRGenerator::generate(ASTNode* programNode, SymbolTable& symbolTable,
                                const std::string& sourceFilePath) {
    IRProgram program;

    // 0. Modül adını registry'ye kaydet
    if (!sourceFilePath.empty())
        currentModuleId_ = program.moduleRegistry.intern(sourceFilePath);
    // sourceFilePath boşsa currentModuleId_ = INVALID_ID (-1) kalır

    // 1. Geçiş: struct ve enum layout haritalarını sembol tablosundan al
    structLayouts_ = symbolTable.structLayouts;
    enumLayouts_ = symbolTable.enumLayouts;

    // 1. Geçiş: modül-düzeyi VariableDecl'leri topla ve kayıt et
    // "Global" değil — bu dosyanın (modülün) kendi değişkenleri.
    std::vector<VariableDeclNode*> globalVars;
    globalSlotTypes_.clear();
    for (ASTNode* child : programNode->getChildren()) {
        if (child->kind == ASTKind::VariableDecl) {
            auto* vd = (VariableDeclNode*) child;
            nameToGlobal_[vd->name] = globalCount_++;
            globalSlotTypes_[nameToGlobal_[vd->name]] = slotTypeFromTypeName(vd->varType);
            program.globalCount++;
            program.globalNames.push_back(vd->name);
            globalVars.push_back(vd);
        }
    }
    // Tek modül: moduleId = "" ile slot sayısını kaydet
    program.moduleGlobalCounts[currentModuleId_] = program.globalCount;

    // Dilim 1.5: CALL sonuç türü için dönüş türlerini önceden topla.
    for (ASTNode* child : programNode->getChildren()) {
        if (child->kind != ASTKind::FunctionDecl)
            continue;
        auto* fnDecl = (FunctionDeclNode*) child;
        funcReturnKind_[fnDecl->name] = slotTypeFromTypeName(fnDecl->returnType);
        funcReturnNullable_[fnDecl->name] = !fnDecl->returnType.empty() &&
                                             fnDecl->returnType.back() == '?';
    }

    // 2. Geçiş: fonksiyonları üret
    for (ASTNode* child : programNode->getChildren()) {
        if (child->kind == ASTKind::FunctionDecl) {
            nameToSlot_.clear();
            shadowStack_.clear();
            nextSlot_ = 0;

            auto* fnDecl = (FunctionDeclNode*) child;
            IRFunction irFn(fnDecl->name, (int) fnDecl->params.size());
            irFn.moduleId = currentModuleId_;
            program.addFunction(std::move(irFn));
            currentFunction_ = program.findFunction(fnDecl->name);

            // main'in başında global değişkenlerin init ifadelerini üret
            if (fnDecl->name == "main") {
                for (VariableDeclNode* gv : globalVars) {
                    if (gv->initExpr) {
                        int initSlot = generateExpression(gv->initExpr);
                        emitStoreGlobal(initSlot, nameToGlobal_[gv->name]);
                    } else if (!gv->varType.empty() && gv->varType.back() == '?') {
                        // ADR-021 null kapısı: init'siz nullable global null
                        // başlar (local emitLoadNull ile aynı sözleşme).
                        int initSlot = freshSlot();
                        emitLoadNull(initSlot, SourceLocation{});
                        emitStoreGlobal(initSlot, nameToGlobal_[gv->name]);
                    } else if (gv->varType == "string") {
                        // #184 ürün kararı: init'siz non-nullable global
                        // string "" başlar (Int 0 değil). `string?` yukarıda
                        // null alır.
                        int initSlot = freshSlot();
                        emitLoadString(initSlot, "", SourceLocation{});
                        emitStoreGlobal(initSlot, nameToGlobal_[gv->name]);
                    }
                }
            }

            generateFunction(child);
            currentFunction_->slotCount = nextSlot_;
            finalizeSlotTypes(currentFunction_, fnDecl);
        }
    }

    return program;
}

// ─────────────────────────────────────────────────────────────────────────────
// generateFunction — Tek bir fonksiyonu IR'a çevirir
// ─────────────────────────────────────────────────────────────────────────────

void IRGenerator::generateFunction(ASTNode* functionDeclNode) {
    auto* fn = (FunctionDeclNode*) functionDeclNode;

    // Parametreler slot 0, 1, 2, ... sırasıyla alır.
    // Interpreter, CALL sırasında bu slotlara argümanları kopyalar.
    for (auto* param : fn->params) {
        int slot = freshSlot();
        registerVariable(param->name, slot);
    }

    // Fonksiyon gövdesi — children[0] her zaman BlockNode
    auto& children = fn->getChildren();
    if (!children.empty()) {
        generateStatement(children[0]);
    }

    // A void function must terminate with an explicit RETURN instruction.
    // Materialize a fresh canonical zero so a local occupying slot 0 can never
    // leak into the caller's process status.
    if (fn->returnType == "void" &&
        (currentFunction_->instructions.empty() ||
         currentFunction_->instructions.back().opcode != Opcode::RETURN)) {
        int zeroSlot = freshSlot();
        emitLoadConst(zeroSlot, 0);
        emitReturn(zeroSlot, fn->loc.line, fn->loc.column);
    }

    // Faz 5: (sourceLine) → ilk instruction IP indeksi (breakpoint eşlemesi için)
    for (int i = 0; i < (int) currentFunction_->instructions.size(); ++i) {
        int sl = currentFunction_->instructions[i].sourceLine;
        if (sl > 0 &&
            currentFunction_->lineToFirstIP.find(sl) == currentFunction_->lineToFirstIP.end())
            currentFunction_->lineToFirstIP[sl] = i;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// generateStatement — Deyim türlerine göre talimat üret
// ─────────────────────────────────────────────────────────────────────────────

void IRGenerator::generateStatement(ASTNode* node) {
    if (!node)
        return;

    // Faz 5: tüm emit noktaları için kaynak konum güncelle
    if (node->loc.isValid())
        currentLoc_ = node->loc;

    switch (node->kind) {
    // ── Blok: içindeki her deyimi sırayla üret ───────────────────────────
    case ASTKind::Block: {
        pushScope();
        for (ASTNode* child : node->getChildren()) {
            generateStatement(child);
        }
        popScope();
        break;
    }

    // ── Değişken bildirimi: int x = <ifade>  /  Point p; ────────────────
    case ASTKind::VariableDecl: {
        auto* vd = (VariableDeclNode*) node;

        if (vd->initExpr) {
            int initSlot = generateExpression(vd->initExpr);

            bool targetIsDouble = (vd->varType == "double");
            bool targetIsFloat32 = (vd->varType == "float"); // ADR-040: 32-bit
            bool targetIsLong = (vd->varType == "longint"); // ADR-040: 64-bit
            bool targetIsDecimal = (vd->varType == "decimal");
            Type srcType = Type::error();
            if (auto* e = dynamic_cast<ExpressionNode*>(vd->initExpr))
                srcType = e->resolvedType;
            bool srcIsInt = srcType.isPrimitive() && (srcType.prim == PrimitiveKind::Int ||
                                                      srcType.prim == PrimitiveKind::Byte);
            bool srcIsFloat32 = srcType.isPrimitive() && srcType.prim == PrimitiveKind::Float;
            bool srcIsDouble = srcType.isPrimitive() && srcType.prim == PrimitiveKind::Double;
            bool srcIsFloat = srcIsFloat32 || srcIsDouble;

            if (targetIsDouble && srcIsInt) {
                int conv = freshSlot();
                emitIntToFloat(conv, initSlot);
                initSlot = conv;
            } else if (targetIsDouble && srcIsFloat32) {
                int conv = freshSlot(); // float32 → double (kayıpsız)
                Instruction ins(Opcode::FLOAT32_TO_FLOAT);
                ins.dest = conv;
                ins.src = initSlot;
                currentFunction_->instructions.push_back(std::move(ins));
                initSlot = conv;
            } else if (targetIsFloat32 && srcIsInt) {
                int conv = freshSlot();
                emitIntToFloat32(conv, initSlot);
                initSlot = conv;
            } else if (targetIsLong && srcIsInt) {
                int conv = freshSlot();
                emitIntToLong(conv, initSlot);
                initSlot = conv;
            } else if (targetIsDecimal && srcIsInt) {
                int conv = freshSlot();
                emitIntToDecimal(conv, initSlot);
                initSlot = conv;
            } else if (targetIsDecimal && srcIsFloat) {
                int conv = freshSlot();
                emitFloatToDecimal(conv, initSlot);
                initSlot = conv;
            }

            // initSlot başka bir değişken tarafından zaten kullanılıyorsa kopyala
            bool slotShared = false;
            for (auto& [n, s] : nameToSlot_)
                if (s == initSlot) {
                    slotShared = true;
                    break;
                }

            if (slotShared) {
                int varSlot = freshSlot();
                registerVariable(vd->name, varSlot);
                emitLoadSlot(varSlot, initSlot);
            } else {
                registerVariable(vd->name, initSlot);
            }

        } else if (structLayouts_.count(vd->varType)) {
            // Struct değişkeni: dış struct + iç struct-tipi alanları özyinelemeli tahsis
            int varSlot = freshSlot();
            registerVariable(vd->name, varSlot);
            int fc = getStructFieldCount(vd->varType);
            SourceLocation loc = vd->loc;
            emitStructNew(varSlot, vd->varType, fc, loc);
            initNestedStructFields(varSlot, vd->varType, loc);
        } else if (vd->varType.size() > 2 && vd->varType.substr(vd->varType.size() - 2) == "[]") {
            // Array değişkeni: init ifadesi yoksa boş dizi (kapasite=0)
            int varSlot = freshSlot();
            registerVariable(vd->name, varSlot);
            std::string elemTypeName = vd->varType.substr(0, vd->varType.size() - 2);
            emitArrayNew(varSlot, 0, arrayElemKindFromTypeName(elemTypeName));
        } else {
            int varSlot = freshSlot();
            registerVariable(vd->name, varSlot);
            // ADR-021 zero-init: tipi `T?` olan ve açık başlangıç değeri
            // verilmeyen değişken null başlar.
            //
            //     int? a;                 // a == null  → true
            //     if (a == null) { ... }  // çalışır
            //
            // Aksi halde slot varsayılan Value{} (= Int 0) kalır ve `a == null`
            // sessizce false döner — nullable sözleşmesinin sessiz ihlali.
            // varType nullable'ı sonundaki '?' ile taşır (AST: "int?").
            if (!vd->varType.empty() && vd->varType.back() == '?')
                emitLoadNull(varSlot, vd->loc);
            // #184 ürün kararı: non-nullable `string` zero-init "" başlar.
            // Aksi halde Value{} (= Int 0) kalır; print "0" basar, length
            // "expected string" ile patlar. `string?` yukarıda null alır.
            else if (vd->varType == "string")
                emitLoadString(varSlot, "", vd->loc);
        }

        // Sibling VariableDecl'ler: int a, b; → children'da diğer VariableDecl'ler
        for (ASTNode* sib : node->getChildren()) {
            if (sib->kind == ASTKind::VariableDecl) {
                generateStatement(sib);
            }
        }
        break;
    }

    // ── return <ifade> ───────────────────────────────────────────────────
    case ASTKind::ReturnStatement: {
        auto* rs = (ReturnStatementNode*) node;
        int returnSlot = 0; // varsayılan: slot[0] (void fonksiyon / boş return)

        if (rs->value) {
            returnSlot = generateExpression(rs->value);
        }
        emitReturn(returnSlot, rs->loc.line, rs->loc.column);
        break;
    }

    // ── if (koşul) { ... } [else { ... }] ───────────────────────────────
    case ASTKind::IfStatement: {
        auto* ifn = (IfStatementNode*) node;

        // Koşulu hesapla
        int condSlot = generateExpression(ifn->condition);

        // "Koşul yanlışsa atla" → hedef henüz bilinmiyor, backpatch bekliyor
        int jumpToElse = emitJumpIfFalse(condSlot);

        // Then bloğu
        if (ifn->thenBranch)
            generateStatement(ifn->thenBranch);

        if (ifn->elseBranch) {
            // Then bitti, else'i atla (then içinde çalışanlar else'e girmemeli)
            int jumpOverElse = emitJumpUnconditional(-1);
            // Şimdi else'in başlangıç konumunu biliyoruz → jumpToElse'i doldur
            patchJump(jumpToElse);
            generateStatement(ifn->elseBranch);
            // Else bitti → jumpOverElse'i doldur
            patchJump(jumpOverElse);
        } else {
            // Else yok → jumpToElse doğrudan if sonrasına atlıyor
            patchJump(jumpToElse);
        }
        break;
    }

    // ── while (koşul) { gövde } ──────────────────────────────────────────
    case ASTKind::WhileStatement: {
        auto* ws = (WhileStatementNode*) node;

        int loopStart = currentInstrIndex();
        loopContextStack_.push_back({});

        int condSlot = generateExpression(ws->condition);
        int exitJump = emitJumpIfFalse(condSlot);

        if (ws->body)
            generateStatement(ws->body);

        // continue → LOOP_START (hedef baştan beri biliniyor)
        for (int idx : loopContextStack_.back().continueJumps)
            currentFunction_->instructions[idx].jumpTarget = loopStart;

        emitJumpUnconditional(loopStart);
        patchJump(exitJump); // OUT burası

        // break → OUT
        int outTarget = currentInstrIndex();
        for (int idx : loopContextStack_.back().breakJumps)
            currentFunction_->instructions[idx].jumpTarget = outTarget;

        loopContextStack_.pop_back();
        break;
    }

    // ── for (init; koşul; güncelleme) { gövde } ─────────────────────────
    //
    // IR yapısı (continue C_LABEL'a, break OUT'a atlar):
    //   [init]
    //   LOOP_START:
    //     [koşul] → JIF_FALSE OUT
    //     [gövde]
    //   C_LABEL:
    //     [güncelleme]
    //     JMP LOOP_START
    //   OUT:
    // ─────────────────────────────────────────────────────────────────────
    case ASTKind::ForStatement: {
        auto* fs = (ForStatementNode*) node;

        if (fs->init)
            generateStatement(fs->init);

        int loopStart = currentInstrIndex();
        loopContextStack_.push_back({});

        int condSlot = fs->condition ? generateExpression(fs->condition) : -1;
        int exitJump = (condSlot != -1) ? emitJumpIfFalse(condSlot) : -1;

        if (fs->body)
            generateStatement(fs->body);

        // C_LABEL: güncelleme başlangıcı — continue buraya atlar
        int cLabel = currentInstrIndex();
        for (int idx : loopContextStack_.back().continueJumps)
            currentFunction_->instructions[idx].jumpTarget = cLabel;

        if (fs->update)
            generateExpression(fs->update);

        emitJumpUnconditional(loopStart);

        if (exitJump != -1)
            patchJump(exitJump); // OUT burası

        // break → OUT
        int outTarget = currentInstrIndex();
        for (int idx : loopContextStack_.back().breakJumps)
            currentFunction_->instructions[idx].jumpTarget = outTarget;

        loopContextStack_.pop_back();
        break;
    }

    // ── do { gövde } while (koşul) ───────────────────────────────────────
    case ASTKind::DoWhileStatement: {
        auto* dw = (DoWhileStatementNode*) node;

        int loopStart = currentInstrIndex();
        loopContextStack_.push_back({});

        if (dw->body)
            generateStatement(dw->body);

        // COND_LABEL: koşul değerlendirmesi — continue buraya atlar
        int condLabel = currentInstrIndex();
        for (int idx : loopContextStack_.back().continueJumps)
            currentFunction_->instructions[idx].jumpTarget = condLabel;

        int condSlot = generateExpression(dw->condition);
        Instruction jit(Opcode::JIF_TRUE);
        jit.cond = condSlot;
        jit.jumpTarget = loopStart;
        jit.sourceLine = currentLoc_.line;
        jit.sourceCol = currentLoc_.column;
        currentFunction_->instructions.push_back(std::move(jit));

        // break → OUT (JIF_TRUE'dan sonraki konum)
        int outTarget = currentInstrIndex();
        for (int idx : loopContextStack_.back().breakJumps)
            currentFunction_->instructions[idx].jumpTarget = outTarget;

        loopContextStack_.pop_back();
        break;
    }

    // ── İfade deyimi: bir ifadeyi değerlendirip sonucu at ────────────────
    // Örnek: print(x) çağrısı, veya x = 5 ataması
    case ASTKind::ExpressionStatement: {
        auto* es = (ExpressionStatementNode*) node;
        if (es->expression) {
            generateExpression(es->expression); // sonucu kullanmıyoruz
        }
        break;
    }

    case ASTKind::BreakStatement: {
        int jumpIdx = emitJumpUnconditional(-1);
        if (!loopContextStack_.empty())
            loopContextStack_.back().breakJumps.push_back(jumpIdx);
        break;
    }
    case ASTKind::ContinueStatement: {
        int jumpIdx = emitJumpUnconditional(-1);
        // Switch bağlamını atla — continue en yakın DÖNGÜYE ait (ADR-027)
        for (int i = (int) loopContextStack_.size() - 1; i >= 0; --i) {
            if (!loopContextStack_[i].isSwitch) {
                loopContextStack_[i].continueJumps.push_back(jumpIdx);
                break;
            }
        }
        break;
    }

    // ── switch (expr) { case v1, v2: body; default: body; }  (ADR-027) ──
    //
    // IR yapısı (fallthrough yok; her case otomatik break):
    //   subjectSlot = eval(subject)
    //   case v1, v2 (OR semantiği):
    //     cmp = EQ(sub, v1); JIF_TRUE → body_start
    //     cmp = EQ(sub, v2); JIF_FALSE → next_case
    //     body_start: [stmts]; JMP → out
    //   next_case:
    //   ...
    //   default: [stmts]; JMP → out
    //   out:
    case ASTKind::SwitchStatement: {
        auto* sw = (SwitchStatementNode*) node;
        int subjectSlot = sw->subject ? generateExpression(sw->subject) : freshSlot();

        // Switch bağlamı: break → out'a atlar; continue switch'e ait değil
        loopContextStack_.push_back({true, {}, {}});

        std::vector<int> outJumps; // her case body sonundaki JMP → out (backpatch)

        for (auto& clause : sw->cases) {
            if (clause.isDefault) {
                for (auto* s : clause.body)
                    generateStatement(s);
                outJumps.push_back(emitJumpUnconditional(-1));
                continue;
            }

            // case v1, v2, ..., vN: — OR semantiği
            // v1..v(N-1): eşleşirse body_start'a JIF_TRUE (backpatch)
            // vN:         eşleşmezse next_case'e JIF_FALSE (backpatch)
            std::vector<int> bodyEntryJumps;
            int nextCaseJump = -1;

            for (size_t vi = 0; vi < clause.values.size(); vi++) {
                int valSlot = generateExpression(clause.values[vi]);
                int cmpSlot = freshSlot();
                emitBinaryOp(Opcode::EQUAL_EQUAL, cmpSlot, subjectSlot, valSlot);
                bool isLast = (vi + 1 == clause.values.size());
                if (isLast)
                    nextCaseJump = emitJumpIfFalse(cmpSlot);
                else
                    bodyEntryJumps.push_back(emitJumpIfTrue(cmpSlot));
            }

            // body_start: ara değerlerin JIF_TRUE buraya atlar
            int bodyStart = currentInstrIndex();
            for (int idx : bodyEntryJumps)
                currentFunction_->instructions[idx].jumpTarget = bodyStart;

            for (auto* s : clause.body)
                generateStatement(s);
            outJumps.push_back(emitJumpUnconditional(-1));

            // next_case: son değerin JIF_FALSE buraya atlar
            int nextCase = currentInstrIndex();
            if (nextCaseJump != -1)
                currentFunction_->instructions[nextCaseJump].jumpTarget = nextCase;
        }

        // out: tüm case JMP'leri + explicit break'ler buraya atlar
        int outTarget = currentInstrIndex();
        for (int idx : outJumps)
            currentFunction_->instructions[idx].jumpTarget = outTarget;
        for (int idx : loopContextStack_.back().breakJumps)
            currentFunction_->instructions[idx].jumpTarget = outTarget;

        loopContextStack_.pop_back();
        break;
    }

    // ── try { body } catch (Error e) { handler }  (ADR-025) ────────────
    case ASTKind::TryStatement: {
        auto* ts = (TryStatementNode*) node;

        // Catch değişkeni için slot; VM bu slota Error nesnesini yazar
        int errorSlot = freshSlot();
        if (!ts->catchVar.empty())
            registerVariable(ts->catchVar, errorSlot);

        // ENTER_TRY: catch hedefi henüz bilinmiyor (-1), sonradan patchlanır
        Instruction enterTry(Opcode::ENTER_TRY);
        enterTry.dest = errorSlot;
        enterTry.jumpTarget = -1;
        enterTry.sourceLine = currentLoc_.line;
        enterTry.sourceCol = currentLoc_.column;
        currentFunction_->instructions.push_back(std::move(enterTry));
        int enterTryIdx = (int) currentFunction_->instructions.size() - 1;

        // Try gövdesi
        if (ts->body)
            generateStatement(ts->body);

        // Normal çıkış: try frame'ini çıkar
        Instruction leaveTry(Opcode::LEAVE_TRY);
        leaveTry.sourceLine = currentLoc_.line;
        leaveTry.sourceCol = currentLoc_.column;
        currentFunction_->instructions.push_back(std::move(leaveTry));

        // Catch bloğunu atla (normal akışta)
        int jumpOverCatch = emitJumpUnconditional(-1);

        // Catch etiketi: ENTER_TRY buraya atlayacak
        int catchLabel = currentInstrIndex();
        currentFunction_->instructions[enterTryIdx].jumpTarget = catchLabel;

        // Catch gövdesi
        if (ts->handler)
            generateStatement(ts->handler);

        // Catch bitti
        patchJump(jumpOverCatch);
        break;
    }

    // ── throw <ifade>;  (ADR-025) ────────────────────────────────────────
    case ASTKind::ThrowStatement: {
        auto* th = (ThrowStatementNode*) node;
        int valSlot = th->value ? generateExpression(th->value) : freshSlot();
        Instruction ins(Opcode::THROW);
        ins.src = valSlot;
        ins.sourceLine = th->loc.line;
        ins.sourceCol = th->loc.column;
        currentFunction_->instructions.push_back(std::move(ins));
        break;
    }

    default:
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// generateExpression — İfadeyi IR'a çevirir, sonucu içeren slot'u döndürür
// ─────────────────────────────────────────────────────────────────────────────

int IRGenerator::generateExpression(ASTNode* node) {
    if (!node)
        return 0;

    // Faz 5: tüm emit noktaları için kaynak konum güncelle
    if (node->loc.isValid())
        currentLoc_ = node->loc;

    switch (node->kind) {
    // ── Sabit değer: 42, 3.14, true ... ──────────────────────────────────
    case ASTKind::Literal: {
        auto* lit = (LiteralNode*) node;
        int slot = freshSlot();

        switch (lit->literalType) {
        case LiteralType::INTEGER: {
            bool asDecimal =
                lit->resolvedType.isPrimitive() && lit->resolvedType.prim == PrimitiveKind::Decimal;
            bool asDouble =
                lit->resolvedType.isPrimitive() && lit->resolvedType.prim == PrimitiveKind::Double;
            bool asFloat32 =
                lit->resolvedType.isPrimitive() && lit->resolvedType.prim == PrimitiveKind::Float;
            bool asLong = lit->resolvedType.isLongInt();
            if (asDecimal) {
                std::string s = "0";
                if (lit->hasDirectValue)
                    s = std::to_string(lit->directIntValue);
                else if (lit->parserToken.token)
                    s = lit->parserToken.token->token;
                emitLoadDecimal(slot, DecimalValue::fromString(s));
            } else if (asDouble) {
                double val = 0.0;
                if (lit->hasDirectValue)
                    val = (double) lit->directIntValue;
                else if (lit->parserToken.token)
                    val = std::stod(lit->parserToken.token->token);
                emitLoadFloat(slot, val);
            } else if (asFloat32) {
                double val = 0.0;
                if (lit->hasDirectValue)
                    val = (double) lit->directIntValue;
                else if (lit->parserToken.token)
                    val = std::stod(lit->parserToken.token->token);
                emitLoadFloat32(slot, val);
            } else if (asLong) {
                long long val = 0;
                if (lit->hasDirectValue)
                    val = lit->directIntValue;
                else if (lit->parserToken.token) {
                    // #219 A2: burası korumasızdı — int64 aralığı dışı literal
                    // yakalanmamış std::out_of_range ile derleyiciyi çökertiyordu
                    // (kullanıcı tanı değil "terminate called" görüyordu).
                    // Aralık denetimi artık type_checker'da (E003); buraya sadece
                    // geçerli literal gelir. Tek istisna int64'ün EN KÜÇÜK değeri:
                    // `-9223372036854775808` unary '-' + `9223372036854775808`
                    // olarak parse edilir ve pozitif hali stoll'da taşar — bu
                    // literal LLONG_MIN olarak üretilir, unary '-' onu geri
                    // çevirir. Kalan catch ulaşılamaz bir backstop'tur.
                    try {
                        val = parseIntegerLiteral(lit->parserToken.token->token,
                                                  lit->literalBase);
                    } catch (...) {
                        if (lit->parserToken.token->token == "9223372036854775808")
                            val = LLONG_MIN; // -(-9223372036854775808) == kendisi
                        else
                            val = 0;
                    }
                }
                emitLoadLong(slot, val);
            } else {
                int value = 0;
                if (lit->hasDirectValue)
                    value = lit->directIntValue;
                else if (lit->parserToken.token) {
                    // #219 A1: eskiden aralık dışı literal burada sessizce 0
                    // oluyordu (`int x = 99999999999999999999;` → 0, tanı yok).
                    // Aralık denetimi artık type_checker'da (E003); bu catch
                    // ulaşılamaz bir backstop. `-2147483648` için literal
                    // 2147483648'dir ve int'e sığmaz — static_cast onu
                    // INT32_MIN'e çevirir, unary '-' geri çevirir (iki tümleyen).
                    try {
                        value = static_cast<int>(
                            parseIntegerLiteral(lit->parserToken.token->token, lit->literalBase));
                    } catch (...) {
                        value = 0;
                    }
                }
                emitLoadConst(slot, value);
            }
            break;
        }
        case LiteralType::BOOLEAN: {
            int value = 0;
            if (lit->hasDirectValue)
                value = lit->directIntValue ? 1 : 0;
            else
                value = (lit->parserToken.token && lit->parserToken.token->token == "true") ? 1 : 0;
            emitLoadConst(slot, value);
            break;
        }
        case LiteralType::STRING: {
            // StringToken::context tırnak işaretleri olmadan içeriği tutar
            std::string content;
            if (auto* st = dynamic_cast<StringToken*>(lit->lexerToken))
                content = st->context;
            else if (lit->parserToken.token) {
                // Fallback: token'ın başındaki ve sonundaki " işaretlerini sıyır
                std::string raw = lit->parserToken.token->token;
                if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
                    content = raw.substr(1, raw.size() - 2);
                else
                    content = raw;
            }
            Instruction ins(Opcode::LOAD_STRING);
            ins.dest = slot;
            ins.stringValue = std::move(content);
            ins.sourceLine = currentLoc_.line;
            ins.sourceCol = currentLoc_.column;
            ins.sourceFile = currentLoc_.filePath();
            currentFunction_->instructions.push_back(std::move(ins));
            break;
        }
        case LiteralType::FLOAT: {
            bool asDecimal =
                lit->resolvedType.isPrimitive() && lit->resolvedType.prim == PrimitiveKind::Decimal;
            // ADR-040: float bağlamı 32-bit single → LOAD_FLOAT32; double → LOAD_FLOAT
            bool asFloat32 =
                lit->resolvedType.isPrimitive() && lit->resolvedType.prim == PrimitiveKind::Float;
            if (asDecimal) {
                // String'den direkt parse — binary float üzerinden geçmez (ADR-028)
                std::string s = "0";
                if (lit->parserToken.token)
                    s = lit->parserToken.token->token;
                emitLoadDecimal(slot, DecimalValue::fromString(s));
            } else {
                double val = 0.0;
                if (lit->parserToken.token)
                    val = std::stod(lit->parserToken.token->token);
                if (asFloat32)
                    emitLoadFloat32(slot, val);
                else
                    emitLoadFloat(slot, val);
            }
            break;
        }
        case LiteralType::BOŞ:
            // null literal → ValueKind::Null (ADR-021)
            {
                Instruction ins(Opcode::LOAD_NULL);
                ins.dest = slot;
                ins.sourceLine = currentLoc_.line;
                ins.sourceCol = currentLoc_.column;
                currentFunction_->instructions.push_back(std::move(ins));
            }
            break;
        }
        return slot;
    }

    // ── Değişken ismi: n, first, second ... ──────────────────────────────
    case ASTKind::Identifier: {
        auto* id = (IdentifierNode*) node;
        std::string name = id->parserToken.token ? id->parserToken.token->token : "";

        if (isGlobal(name)) {
            int tempSlot = freshSlot();
            emitLoadGlobal(tempSlot, getGlobalIndex(name));
            return tempSlot;
        }
        return lookupVariable(name);
    }

    // ── İkili ifade: x + y, x = y, x < y ... ────────────────────────────
    case ASTKind::BinaryExpression: {
        auto* bin = (BinaryExpressionNode*) node;

        // Atama operatörleri: x = expr  ve  a[i] = expr
        if (bin->Operator == TokenType::EQUAL) {
            int rhsSlot = generateExpression(bin->Right);

            // a[i] = val → ARRAY_SET
            if (bin->Left && bin->Left->kind == ASTKind::IndexExpression) {
                auto* idx = (IndexExpressionNode*) bin->Left;
                int arrSlot = generateExpression(idx->object);
                int idxSlot = generateExpression(idx->index);
                // Eleman tipi kaynak DİZİDEN okunur (ARRAY_GET ile aynı kural).
                ArrayElemKind setElemKind = ArrayElemKind::Ref;
                if (auto* objExpr = dynamic_cast<ExpressionNode*>(idx->object))
                    if (objExpr->resolvedType.isArray() && objExpr->resolvedType.elementType)
                        setElemKind = arrayElemKindFromType(objExpr->resolvedType);
                emitArraySet(arrSlot, idxSlot, rhsSlot, bin->loc.line, bin->loc.column,
                             setElemKind);
                return rhsSlot;
            }

            // p.field = val → FIELD_SET
            if (bin->Left && bin->Left->kind == ASTKind::MemberAccess) {
                auto* ma = (MemberAccessNode*) bin->Left;
                int objSlot = generateExpression(ma->object);
                std::string structName;
                if (auto* exprObj = dynamic_cast<ExpressionNode*>(ma->object))
                    structName = exprObj->resolvedType.structName;
                int idx2 = getStructFieldIndex(structName, ma->member);
                if (idx2 >= 0)
                    emitFieldSet(objSlot, idx2, rhsSlot, bin->loc.line, bin->loc.column);
                return rhsSlot;
            }

            auto* lhsId = (IdentifierNode*) bin->Left;
            std::string varName = lhsId->parserToken.token->token;

            if (isGlobal(varName)) {
                emitStoreGlobal(rhsSlot, getGlobalIndex(varName));
                return rhsSlot;
            }

            int varSlot = lookupVariable(varName);
            if (rhsSlot != varSlot)
                emitLoadSlot(varSlot, rhsSlot);
            return varSlot;
        }

        // Birleşik atama: += -= *= /= %= &= |= ^= <<= >>=
        // x OP= y  ≡  x = x OP y
        if (bin->Operator == TokenType::PLUS_EQUAL || bin->Operator == TokenType::MINUS_EQUAL ||
            bin->Operator == TokenType::STAR_EQUAL || bin->Operator == TokenType::SLASH_EQUAL ||
            bin->Operator == TokenType::PERCENT_EQUAL ||
            bin->Operator == TokenType::AMPERSAND_EQUAL || bin->Operator == TokenType::PIPE_EQUAL ||
            bin->Operator == TokenType::CARET_EQUAL || bin->Operator == TokenType::LSHIFT_EQUAL ||
            bin->Operator == TokenType::RSHIFT_EQUAL) {
            auto* lhsId = (IdentifierNode*) bin->Left;
            std::string varName = lhsId->parserToken.token->token;
            int rhsSlot = generateExpression(bin->Right);

            Opcode arithOp = Opcode::ADD;
            if (bin->Operator == TokenType::MINUS_EQUAL)
                arithOp = Opcode::SUB;
            else if (bin->Operator == TokenType::STAR_EQUAL)
                arithOp = Opcode::MUL;
            else if (bin->Operator == TokenType::SLASH_EQUAL)
                arithOp = Opcode::DIV;
            else if (bin->Operator == TokenType::PERCENT_EQUAL)
                arithOp = Opcode::MOD;
            else if (bin->Operator == TokenType::AMPERSAND_EQUAL)
                arithOp = Opcode::BAND;
            else if (bin->Operator == TokenType::PIPE_EQUAL)
                arithOp = Opcode::BOR;
            else if (bin->Operator == TokenType::CARET_EQUAL)
                arithOp = Opcode::BXOR;
            else if (bin->Operator == TokenType::LSHIFT_EQUAL)
                arithOp = Opcode::SHL;
            else if (bin->Operator == TokenType::RSHIFT_EQUAL)
                arithOp = Opcode::SHR;

            // string += string → STRING_CONCAT (ADR-024)
            if (bin->Operator == TokenType::PLUS_EQUAL) {
                if (auto* e = dynamic_cast<ExpressionNode*>(bin->Right))
                    if (e->resolvedType.isString())
                        arithOp = Opcode::STRING_CONCAT;
            }

            int resultSlot = freshSlot();

            if (isGlobal(varName)) {
                int currentSlot = freshSlot();
                emitLoadGlobal(currentSlot, getGlobalIndex(varName));
                emitBinaryOp(arithOp, resultSlot, currentSlot, rhsSlot, bin->loc.line,
                             bin->loc.column);
                emitStoreGlobal(resultSlot, getGlobalIndex(varName));
                return resultSlot;
            }

            int varSlot = lookupVariable(varName);
            emitBinaryOp(arithOp, resultSlot, varSlot, rhsSlot, bin->loc.line, bin->loc.column);
            emitLoadSlot(varSlot, resultSlot);
            return varSlot;
        }

        // Unary prefix: Left = nullptr (ör: -x, !x)
        if (!bin->Left) {
            int operandSlot = generateExpression(bin->Right);
            int resultSlot = freshSlot();

            bool operandIsDecimal = false, operandIsFloat32 = false, operandIsDouble = false,
                 operandIsLong = false;
            if (auto* e = dynamic_cast<ExpressionNode*>(bin->Right)) {
                operandIsDecimal = e->resolvedType.isDecimal();
                operandIsFloat32 =
                    e->resolvedType.isPrimitive() && e->resolvedType.prim == PrimitiveKind::Float;
                operandIsDouble =
                    e->resolvedType.isPrimitive() && e->resolvedType.prim == PrimitiveKind::Double;
                operandIsLong = e->resolvedType.isLongInt();
            }
            auto emitUn = [&](Opcode o) {
                Instruction ins(o);
                ins.dest = resultSlot;
                ins.src = operandSlot;
                ins.sourceLine = currentLoc_.line;
                ins.sourceCol = currentLoc_.column;
                currentFunction_->instructions.push_back(std::move(ins));
            };
            if (bin->Operator == TokenType::MINUS) {
                if (operandIsDecimal)
                    emitUn(Opcode::DNEG);
                else if (operandIsFloat32)
                    emitUn(Opcode::F32NEG);
                else if (operandIsDouble)
                    emitUn(Opcode::FNEG);
                else if (operandIsLong)
                    emitUn(Opcode::LNEG);
                else {
                    int zeroSlot = freshSlot();
                    emitLoadConst(zeroSlot, 0);
                    emitBinaryOp(Opcode::SUB, resultSlot, zeroSlot, operandSlot, bin->loc.line,
                                 bin->loc.column);
                }
            } else if (bin->Operator == TokenType::BANG) {
                // !x → (x == 0): sıfırsa 1, değilse 0
                int zeroSlot = freshSlot();
                emitLoadConst(zeroSlot, 0);
                emitBinaryOp(Opcode::EQUAL_EQUAL, resultSlot, operandSlot, zeroSlot, bin->loc.line,
                             bin->loc.column);
            } else if (bin->Operator == TokenType::TILDE) {
                // ~x — bitsel değil (longint 64-bit)
                emitUn(operandIsLong ? Opcode::LBNOT : Opcode::BNOT);
            } else {
                emitLoadSlot(resultSlot, operandSlot);
            }
            return resultSlot;
        }

        // Aritmetik operatörler
        const int L = bin->loc.line, C = bin->loc.column;
        switch (bin->Operator) {
        case TokenType::PLUS:
            return generateBinaryArithmetic(Opcode::ADD, bin->Left, bin->Right, L, C, bin);
        case TokenType::MINUS:
            return generateBinaryArithmetic(Opcode::SUB, bin->Left, bin->Right, L, C, bin);
        case TokenType::STAR:
            return generateBinaryArithmetic(Opcode::MUL, bin->Left, bin->Right, L, C, bin);
        case TokenType::SLASH:
            return generateBinaryArithmetic(Opcode::DIV, bin->Left, bin->Right, L, C, bin);
        case TokenType::PERCENT:
            return generateBinaryArithmetic(Opcode::MOD, bin->Left, bin->Right, L, C, bin);
        // Karşılaştırma operatörleri
        case TokenType::LESS:
            return generateBinaryArithmetic(Opcode::LESS, bin->Left, bin->Right, L, C, bin);
        case TokenType::LESS_EQUAL:
            return generateBinaryArithmetic(Opcode::LESS_EQUAL, bin->Left, bin->Right, L, C, bin);
        case TokenType::GREATER:
            return generateBinaryArithmetic(Opcode::GREATER, bin->Left, bin->Right, L, C, bin);
        case TokenType::GREATER_EQUAL:
            return generateBinaryArithmetic(Opcode::GREATER_EQUAL, bin->Left, bin->Right, L, C, bin);
        case TokenType::EQUAL_EQUAL:
            return generateBinaryArithmetic(Opcode::EQUAL_EQUAL, bin->Left, bin->Right, L, C, bin);
        case TokenType::BANG_EQUAL:
            return generateBinaryArithmetic(Opcode::NOT_EQUAL, bin->Left, bin->Right, L, C, bin);

        // Bitsel operatörler
        case TokenType::AMPERSAND:
            return generateBinaryArithmetic(Opcode::BAND, bin->Left, bin->Right, L, C, bin);
        case TokenType::PIPE:
            return generateBinaryArithmetic(Opcode::BOR, bin->Left, bin->Right, L, C, bin);
        case TokenType::CARET:
            return generateBinaryArithmetic(Opcode::BXOR, bin->Left, bin->Right, L, C, bin);
        case TokenType::LSHIFT:
            return generateBinaryArithmetic(Opcode::SHL, bin->Left, bin->Right, L, C, bin);
        case TokenType::RSHIFT:
            return generateBinaryArithmetic(Opcode::SHR, bin->Left, bin->Right, L, C, bin);

        // Mantıksal operatörler — ADR-008: kısa devre VAR, sonuç 1/0'dır.
        //
        // İki ayrı sözleşme, ikisi de burada karşılanır:
        //
        //   1. KISA DEVRE (değerlendirme): `a` sonucu belirliyorsa `b` HİÇ
        //      çalıştırılmaz. Yan etkili sağ taraf için gözlemlenebilir
        //      (`f() && g()` → a falsy ise g çağrılmaz) ve null kontrolü
        //      kalıbının temeli (`x != null && x.alan > 0`).
        //
        //   2. SONUÇ 1/0 (değer): operandın kendisi DEĞİL, doğruluk değeri
        //      döner. `5 && 3` → 1, `0 || 33` → 1. C/Java/Go modeli.
        //      Python/JS'in "operandı döndür" davranışı statik tipli bir
        //      dilde tip belirsizliği üretir; `5 && 2` bir bool ifadesidir,
        //      sayısal bir birleştirme değil.
        //
        // Şema (&& için; || simetrik):
        //   result = 0
        //   if (a falsy) → DONE            ; kısa devre: b atlanır
        //   if (b falsy) → DONE            ; b değerlendirildi, sonuç 0 kalır
        //   result = 1
        // DONE:
        //
        // İkinci JIF'in işlevi 1/0'a indirgemedir: `b`'nin değerini
        // kopyalamak (eski `LOAD_SLOT result, slotB`) sonucu `b`'nin kendisi
        // yapardı ve sözleşme (2)'yi ihlal ederdi.
        case TokenType::AMPERSAND_AMPERSAND: {
            int slotA = generateExpression(bin->Left);
            int result = freshSlot();
            emitLoadConst(result, 0);            // varsayılan: false
            int skipB = emitJumpIfFalse(slotA);  // a falsy → b'yi atla
            int slotB = generateExpression(bin->Right);
            int bFalsy = emitJumpIfFalse(slotB); // b falsy → 0 kalsın
            emitLoadConst(result, 1);            // ikisi de truthy → 1
            patchJump(skipB);
            patchJump(bFalsy);
            return result;
        }
        case TokenType::PIPE_PIPE: {
            int slotA = generateExpression(bin->Left);
            int result = freshSlot();
            emitLoadConst(result, 1);            // varsayılan: true
            int skipB = emitJumpIfTrue(slotA);   // a truthy → b'yi atla
            int slotB = generateExpression(bin->Right);
            int bTruthy = emitJumpIfTrue(slotB); // b truthy → 1 kalsın
            emitLoadConst(result, 0);            // ikisi de falsy → 0
            patchJump(skipB);
            patchJump(bTruthy);
            return result;
        }

        default: {
            // Bilinmeyen operatör — boş slot döndür
            int slot = freshSlot();
            emitLoadConst(slot, 0);
            return slot;
        }
        }
    }

    // ── Fonksiyon çağrısı: fibonacci(n-1), print(x) ... ─────────────────
    case ASTKind::Call: {
        auto* call = (CallExpressionNode*) node;

        // Hangi fonksiyon çağrılıyor? Callee bir Identifier
        std::string fnName;
        bool isBuiltin = false;
        int ffiHostId = -1;
        bool ffiReturnsVoid = false;
        const Type* ffiReturnType = nullptr;

        if (call->callee && call->callee->kind == ASTKind::Identifier) {
            auto* calleeId = (IdentifierNode*) call->callee;
            if (calleeId->parserToken.token) {
                fnName = calleeId->parserToken.token->token;
            }
            // Builtin kontrolü: resolvedSymbol->isBuiltin
            if (calleeId->resolvedSymbol && calleeId->resolvedSymbol->isBuiltin) {
                isBuiltin = true;
            }
            // ADR-034 (#107): FFI host fonksiyonu — sayısal dispatch
            if (calleeId->resolvedSymbol && calleeId->resolvedSymbol->hostFnId >= 0) {
                ffiHostId = calleeId->resolvedSymbol->hostFnId;
                ffiReturnsVoid = calleeId->resolvedSymbol->type.returnType &&
                                 calleeId->resolvedSymbol->type.returnType->isVoid();
                ffiReturnType = calleeId->resolvedSymbol->type.returnType.get();
            }
        }

        // Her argümanı hesapla, sonuçların slot numaralarını topla
        std::vector<int> argSlots;
        for (ASTNode* arg : call->arguments) {
            argSlots.push_back(generateExpression(arg));
        }

        if (ffiHostId >= 0) {
            // CALLHOST: sayısal host id ile FFI dispatch (ADR-034, #107)
            int destSlot = ffiReturnsVoid ? -1 : freshSlot();
            Instruction ins(Opcode::CALLHOST);
            // #227: intValue artık BİRLEŞİK registry indeksidir (kHostFnBase
            // + host id). functionName yalnızca IR dump okunabilirliği için
            // kalır — dispatch ona BAKMAZ, sıcak yolda string karşılaştırması
            // yok.
            ins.functionName = "__ffi__";
            ins.intValue = kHostFnBase + ffiHostId;
            ins.dest = destSlot;
            ins.argSlots = argSlots;
            if (!ffiReturnsVoid && ffiReturnType) {
                ins.valueType = slotTypeFromType(*ffiReturnType);
                ins.valueNullable = ffiReturnType->nullable;
            }
            ins.sourceLine = call->loc.line;
            ins.sourceCol = call->loc.column;
            ins.sourceFile = call->loc.filePath();
            currentFunction_->instructions.push_back(std::move(ins));
            return destSlot;
        } else if (isBuiltin) {
            // CALLHOST: çekirdek host fonksiyonu (print). #227: artık registry'de
            // sıradan bir kayıt — intValue birleşik indeksi taşır, dispatch
            // functionName'e BAKMAZ (o yalnızca IR dump okunabilirliği için).
            Instruction ins(Opcode::CALLHOST);
            ins.functionName = fnName;
            ins.intValue = hostEntryIndex(fnName == "print" ? "CORE_PRINT" : "");
            ins.argSlots = argSlots;
            ins.sourceLine = call->loc.line;
            ins.sourceCol = call->loc.column;
            ins.sourceFile = call->loc.filePath();
            currentFunction_->instructions.push_back(std::move(ins));
            return -1; // Dönüş değeri yok
        } else {
            // CALL: saQut fonksiyonu çağır, sonucu yeni slota yaz
            int destSlot = freshSlot();
            Instruction ins(Opcode::CALL);
            ins.dest = destSlot;
            ins.functionName = fnName;
            ins.argSlots = argSlots;
            auto rn = funcReturnNullable_.find(fnName);
            ins.valueNullable = rn != funcReturnNullable_.end() && rn->second;
            ins.sourceLine = call->loc.line;
            ins.sourceCol = call->loc.column;
            ins.sourceFile = call->loc.filePath();
            currentFunction_->instructions.push_back(std::move(ins));
            return destSlot;
        }
    }

    // ── ScopeCall: E::method(args) — built-in metod ─────────────────────
    case ASTKind::ScopeCall: {
        auto* sc = (ScopeCallNode*) node;

        // Her argümanı hesapla
        std::vector<int> argSlots;
        for (ASTNode* arg : sc->arguments)
            argSlots.push_back(generateExpression(arg));

        // CALLHOST: functionName = "__builtin_method__", intValue = builtinId
        // dest slotu: void dönüşlü metod için -1
        bool returnsVoid = sc->resolvedType.isVoid();
        int destSlot = returnsVoid ? -1 : freshSlot();

        Instruction ins(Opcode::CALLHOST);
        // #227: birleşik registry indeksi (kBuiltinBase + metod id).
        ins.functionName = "__builtin_method__";
        ins.intValue = kBuiltinBase + sc->builtinId;
        ins.argSlots = std::move(argSlots);
        ins.dest = destSlot;
        if (!returnsVoid) ins.valueType = slotTypeFromType(sc->resolvedType);
        if (!returnsVoid) ins.valueNullable = sc->resolvedType.nullable;
        ins.sourceLine = sc->loc.line;
        ins.sourceCol = sc->loc.column;
        currentFunction_->instructions.push_back(std::move(ins));
        return destSlot;
    }

    // ── Postfix: i++, i-- ────────────────────────────────────────────────
    case ASTKind::Postfix: {
        auto* pf = (PostfixNode*) node;
        // Şu anki değeri döndür, sonra artır/azalt
        int operandSlot = generateExpression(pf->operand);
        int resultSlot = freshSlot(); // dönüş değeri (artırmadan önceki)
        emitLoadSlot(resultSlot, operandSlot);

        int oneSlot = freshSlot();
        emitLoadConst(oneSlot, 1);
        int newSlot = freshSlot();

        if (pf->Operator == TokenType::PLUS_PLUS) {
            emitBinaryOp(Opcode::ADD, newSlot, operandSlot, oneSlot);
        } else {
            emitBinaryOp(Opcode::SUB, newSlot, operandSlot, oneSlot);
        }

        // #??: GLOBAL postfix artırımı — `counter++` operand'ı bir global ise
        // generateExpression LOAD_GLOBAL ile değeri GEÇİCİ bir slot'a yükler;
        // emitLoadSlot(operandSlot, ...) o geçici slotu günceller ama global
        // belleğe GERİ YAZMAZ. Böylece global asla artmaz (VM'de `/5 5 5/`
        // jambonlu davranış → kullanıcı "global değişken kullanılmaz, state
        // parametreyle taşınır" diye telafi ediyordu). Basit atama (`=`) ve
        // birleşik atama (`+=`) case'lerindeki isGlobal kontrolü gibi, global
        // operand için sonucu emitStoreGlobal ile global'e geri yaz.
        if (pf->operand && pf->operand->kind == ASTKind::Identifier) {
            auto* pid = (IdentifierNode*) pf->operand;
            const std::string& pname =
                pid->parserToken.token ? pid->parserToken.token->token : "";
            if (isGlobal(pname))
                emitStoreGlobal(newSlot, getGlobalIndex(pname));
        }
        emitLoadSlot(operandSlot, newSlot); // orijinal değişkeni güncelle
        return resultSlot; // artırmadan önceki değer
    }

    // ── Üye erişimi okuma: p.x ───────────────────────────────────────────
    case ASTKind::MemberAccess: {
        auto* ma = (MemberAccessNode*) node;
        // Enum üye erişimi: Color.Red → LOAD_INT sabiti
        if (ma->object && ma->object->kind == ASTKind::Identifier) {
            auto* id = (IdentifierNode*) ma->object;
            const std::string& idName = id->parserToken.token ? id->parserToken.token->token : "";
            auto it = enumLayouts_.find(idName);
            if (it != enumLayouts_.end()) {
                int destSlot = freshSlot();
                int val = -1;
                for (auto& p : it->second)
                    if (p.first == ma->member) {
                        val = p.second;
                        break;
                    }
                emitLoadConst(destSlot, val);
                ma->resolvedType = Type::enumType(idName);
                return destSlot;
            }
        }
        int objSlot = generateExpression(ma->object);
        int destSlot = freshSlot();
        // Nesnenin struct adını resolvedType üstünden al (tip denetleyici yazdı)
        std::string structName;
        if (auto* exprObj = dynamic_cast<ExpressionNode*>(ma->object))
            structName = exprObj->resolvedType.structName;
        int idx = getStructFieldIndex(structName, ma->member);
        if (idx >= 0)
            emitFieldGet(destSlot, objSlot, idx, {},
                         slotTypeFromType(ma->resolvedType), ma->resolvedType.nullable);
        return destSlot;
    }
    case ASTKind::ArrayLiteral: {
        auto* al = (ArrayLiteralNode*) node;
        int arrSlot = freshSlot();
        ArrayElemKind ak = arrayElemKindFromType(al->resolvedType);
        emitArrayNew(arrSlot, (int) al->elements.size(), ak);
        for (int i = 0; i < (int) al->elements.size(); i++) {
            int idxSlot = freshSlot();
            emitLoadConst(idxSlot, i);
            int valSlot = generateExpression(al->elements[i]);
            emitArraySet(arrSlot, idxSlot, valSlot, 0, 0, ak);
        }
        return arrSlot;
    }

    // ── Index erişimi okuma: a[i] ─────────────────────────────────────────
    case ASTKind::IndexExpression: {
        auto* idx = (IndexExpressionNode*) node;
        int arrSlot = generateExpression(idx->object);
        int idxSlot = generateExpression(idx->index);
        int destSlot = freshSlot();
        // Eleman tipi (#206 packed temsili) talimata yazılır: JIT bunu
        // doğrudan bellek erişimi için kullanır (çağrısız `a[i]`). Kaynak
        // DİZİNİN tipidir — sonucun değil: `idx->resolvedType` eleman tipini
        // verir ama packed dizilim bilgisi dizide durur.
        ArrayElemKind elemKind = ArrayElemKind::Ref;
        if (auto* objExpr = dynamic_cast<ExpressionNode*>(idx->object))
            if (objExpr->resolvedType.isArray() && objExpr->resolvedType.elementType)
                elemKind = arrayElemKindFromType(objExpr->resolvedType);
        emitArrayGet(destSlot, arrSlot, idxSlot, idx->loc.line, idx->loc.column,
                     slotTypeFromType(idx->resolvedType), elemKind);
        return destSlot;
    }

    // ── CastExpression: expr as TargetType[?]  (ADR-026) ───────────────────
    case ASTKind::CastExpression: {
        auto* cast = (CastExpressionNode*) node;
        int srcSlot = generateExpression(cast->operand);
        int destSlot = freshSlot();
        int nullable = cast->targetNullable ? 1 : 0;

        // Kaynak ve hedef tipleri resolvedType üstünden al
        Type srcType = Type::error();
        if (auto* exn = dynamic_cast<ExpressionNode*>(cast->operand))
            srcType = exn->resolvedType;
        Type tgtType = cast->resolvedType;
        tgtType.nullable = false; // base type

        // ADR-040: float32/double ve longint ayrı izlenir.
        bool srcIsStr = srcType.isString();
        bool srcIsFloat32 = srcType.isPrimitive() && srcType.prim == PrimitiveKind::Float;
        bool srcIsDouble = srcType.isPrimitive() && srcType.prim == PrimitiveKind::Double;
        bool srcIsFloat = srcIsFloat32 || srcIsDouble;
        bool srcIsInt = srcType.isPrimitive() && srcType.prim == PrimitiveKind::Int;
        bool srcIsBool = srcType.isPrimitive() && srcType.prim == PrimitiveKind::Bool;
        bool srcIsDecimal = srcType.isDecimal();
        bool srcIsLong = srcType.isLongInt();
        bool tgtIsStr = tgtType.isString();
        bool tgtIsFloat32 = tgtType.isPrimitive() && tgtType.prim == PrimitiveKind::Float;
        bool tgtIsDouble = tgtType.isPrimitive() && tgtType.prim == PrimitiveKind::Double;
        bool tgtIsFloat = tgtIsFloat32 || tgtIsDouble;
        bool tgtIsInt = tgtType.isPrimitive() && tgtType.prim == PrimitiveKind::Int;
        bool tgtIsDecimal = tgtType.isDecimal();
        bool tgtIsLong = tgtType.isLongInt();
        bool srcIsByte = srcType.isByte();
        bool tgtIsByte = tgtType.isByte();

        // Tek-operandlı çevrim yaz + kimlik (LOAD_SLOT) kısayolu
        auto emitCastConv = [&](Opcode o) {
            Instruction ins(o);
            ins.dest = destSlot;
            ins.src = srcSlot;
            currentFunction_->instructions.push_back(std::move(ins));
        };
        auto emitIdentity = [&]() {
            Instruction nop(Opcode::LOAD_SLOT);
            nop.dest = destSlot;
            nop.src = srcSlot;
            currentFunction_->instructions.push_back(std::move(nop));
        };

        Opcode op;
        bool infallible = false;
        // ── longint dönüşümleri (ADR-040) ──
        if ((srcIsInt || srcIsByte) && tgtIsLong) {
            emitCastConv(Opcode::INT_TO_LONG);
            return destSlot; // kayıpsız genişletme
        } else if (srcIsLong && tgtIsInt) {
            op = Opcode::LONG_TO_INT_CHECKED; // fallible daralma
        } else if (srcIsLong && tgtIsLong) {
            emitIdentity();
            return destSlot;
        } else if (srcIsLong && tgtIsStr) {
            op = Opcode::CAST_LONG_TO_STR;
            infallible = true;
        } else if (srcIsStr && tgtIsLong) {
            op = Opcode::CAST_STR_TO_LONG;
        } else if (srcIsLong && tgtIsFloat) {
            // longint → float/double: int64 double'a genişler (32-bit hedefte truncate)
            emitCastConv(Opcode::INT_TO_FLOAT);
            if (tgtIsFloat32) {
                int mid = destSlot;
                destSlot = freshSlot();
                Instruction ins(Opcode::FLOAT_TO_FLOAT32);
                ins.dest = destSlot;
                ins.src = mid;
                currentFunction_->instructions.push_back(std::move(ins));
            }
            return destSlot;
        } else if (srcIsFloat && tgtIsLong) {
            op = Opcode::CAST_FLOAT_TO_LONG_CHECKED; // float→int64 (64-bit trunc)
        }
        // ── float32 ↔ double ──
        else if (srcIsFloat32 && tgtIsDouble) {
            emitCastConv(Opcode::FLOAT32_TO_FLOAT);
            return destSlot; // kayıpsız
        } else if (srcIsDouble && tgtIsFloat32) {
            emitCastConv(Opcode::FLOAT_TO_FLOAT32);
            return destSlot; // veri kaybı (E003 uyardı)
        }
        // ── byte dönüşümleri (#86) ──
        else if (srcIsInt && tgtIsByte) {
            op = Opcode::CAST_INT_TO_BYTE_CHECKED;
        } else if ((srcIsByte && tgtIsInt) || (srcIsByte && tgtIsByte)) {
            emitIdentity();
            return destSlot;
        } else if (srcIsByte && tgtIsStr) {
            op = Opcode::CAST_INT_TO_STR;
            infallible = true;
        }
        // ── float/double dönüşümleri ──
        else if (srcIsInt && tgtIsFloat32) {
            emitCastConv(Opcode::INT_TO_FLOAT32);
            return destSlot;
        } else if (srcIsInt && tgtIsDouble) {
            op = Opcode::INT_TO_FLOAT;
            infallible = true;
        } else if (srcIsInt && tgtIsDecimal) {
            op = Opcode::INT_TO_DECIMAL;
            infallible = true;
        } else if (srcIsFloat && tgtIsDecimal) {
            op = Opcode::FLOAT_TO_DECIMAL;
            infallible = true;
        } else if (srcIsDecimal && tgtIsStr) {
            op = Opcode::CAST_DECIMAL_TO_STR;
            infallible = true;
        } else if (srcIsDecimal && tgtIsFloat) {
            op = Opcode::CAST_DECIMAL_TO_FLOAT;
            infallible = true;
        } else if (srcIsDecimal && tgtIsInt) {
            op = Opcode::CAST_DECIMAL_TO_INT;
        } else if (srcIsStr && tgtIsDecimal) {
            op = Opcode::CAST_STR_TO_DECIMAL;
        } else if (srcIsFloat && tgtIsInt) {
            op = Opcode::CAST_FLOAT_TO_INT_CHECKED;
        } else if (srcIsInt && tgtIsStr) {
            op = Opcode::CAST_INT_TO_STR;
            infallible = true;
        } else if (srcIsFloat32 && tgtIsStr) {
            op = Opcode::CAST_FLOAT32_TO_STR;
            infallible = true;
        } else if (srcIsFloat && tgtIsStr) {
            op = Opcode::CAST_FLOAT_TO_STR;
            infallible = true;
        } else if (srcIsBool && tgtIsStr) {
            op = Opcode::CAST_BOOL_TO_STR;
            infallible = true;
        } else if (srcIsStr && tgtIsInt) {
            op = Opcode::CAST_STR_TO_INT;
        } else if (srcIsStr && tgtIsFloat32) {
            op = Opcode::CAST_STR_TO_FLOAT32;
        } else if (srcIsStr && tgtIsFloat) {
            op = Opcode::CAST_STR_TO_FLOAT;
        } else {
            // Aynı tip→aynı tip (int→int gibi): kimlik dönüşümü
            Instruction nop(Opcode::LOAD_SLOT);
            nop.dest = destSlot;
            nop.src = srcSlot;
            currentFunction_->instructions.push_back(std::move(nop));
            return destSlot;
        }
        Instruction ins(op);
        ins.dest = destSlot;
        ins.src = srcSlot;
        ins.left = infallible ? -1 : nullable; // -1=bayraksız; 0=throw; 1=null
        currentFunction_->instructions.push_back(std::move(ins));
        return destSlot;
    }

    default:
        // Bilinmeyen ifade türü
        return freshSlot(); // boş slot (0 değeriyle)
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// generateBinaryArithmetic — İkili op için sol+sağ üret, talimat ekle
// ─────────────────────────────────────────────────────────────────────────────

int IRGenerator::generateBinaryArithmetic(Opcode opcode, ASTNode* leftNode, ASTNode* rightNode,
                                          int line, int col, ASTNode* resultNode) {
    int leftSlot = generateExpression(leftNode);
    int rightSlot = generateExpression(rightNode);
    int destSlot = freshSlot();

    // Tip tespiti — resolvedType üstünden (tip denetleyici tarafından yazıldı).
    // ADR-040: float32 (32-bit) ile double (64-bit) ve longint (64-bit int)
    // ayrı izlenir.
    bool leftIsDecimal = false, rightIsDecimal = false;
    bool leftIsFloat32 = false, rightIsFloat32 = false; // 32-bit single
    bool leftIsDouble = false, rightIsDouble = false; // 64-bit double
    bool leftIsLong = false, rightIsLong = false; // 64-bit int
    bool leftIsString = false, rightIsString = false;
    bool leftIsByte = false, rightIsByte = false;   // ADR-040 Faz 4: tip-içi sarma
    auto classify = [](ASTNode* n, bool& isDec, bool& isF32, bool& isDbl, bool& isLong,
                       bool& isStr, bool& isByte) {
        if (auto* e = dynamic_cast<ExpressionNode*>(n)) {
            isDec = e->resolvedType.isDecimal();
            isF32 = e->resolvedType.isPrimitive() && e->resolvedType.prim == PrimitiveKind::Float;
            isDbl = e->resolvedType.isPrimitive() && e->resolvedType.prim == PrimitiveKind::Double;
            isLong = e->resolvedType.isLongInt();
            isStr = e->resolvedType.isString();
            isByte = e->resolvedType.isByte();
        }
    };
    classify(leftNode, leftIsDecimal, leftIsFloat32, leftIsDouble, leftIsLong, leftIsString,
             leftIsByte);
    classify(rightNode, rightIsDecimal, rightIsFloat32, rightIsDouble, rightIsLong, rightIsString,
             rightIsByte);

    bool leftIsFloat = leftIsFloat32 || leftIsDouble;
    bool rightIsFloat = rightIsFloat32 || rightIsDouble;

    // String birleştirme (ADR-024): + → STRING_CONCAT
    if ((leftIsString || rightIsString) && opcode == Opcode::ADD) {
        emitBinaryOp(Opcode::STRING_CONCAT, destSlot, leftSlot, rightSlot, line, col);
        return destSlot;
    }

    // Decimal aritmetik (ADR-028): en az bir operand decimal ise decimal path
    if (leftIsDecimal || rightIsDecimal) {
        if (!leftIsDecimal && leftIsFloat) {
            int conv = freshSlot();
            emitFloatToDecimal(conv, leftSlot);
            leftSlot = conv;
        } else if (!leftIsDecimal) {
            int conv = freshSlot();
            emitIntToDecimal(conv, leftSlot);
            leftSlot = conv;
        }
        if (!rightIsDecimal && rightIsFloat) {
            int conv = freshSlot();
            emitFloatToDecimal(conv, rightSlot);
            rightSlot = conv;
        } else if (!rightIsDecimal) {
            int conv = freshSlot();
            emitIntToDecimal(conv, rightSlot);
            rightSlot = conv;
        }
        Opcode decOp = opcode;
        if (opcode == Opcode::ADD)
            decOp = Opcode::DADD;
        else if (opcode == Opcode::SUB)
            decOp = Opcode::DSUB;
        else if (opcode == Opcode::MUL)
            decOp = Opcode::DMUL;
        else if (opcode == Opcode::DIV)
            decOp = Opcode::DDIV;
        else if (opcode == Opcode::MOD)
            decOp = Opcode::DMOD;
        emitBinaryOp(decOp, destSlot, leftSlot, rightSlot, line, col);
        return destSlot;
    }

    // ADR-040: longint aritmetik — herhangi bir operand longint (float/decimal
    // yok, typechecker garanti ediyor). int operand INT_TO_LONG ile genişletilir.
    if (leftIsLong || rightIsLong) {
        if (!leftIsLong) {
            int conv = freshSlot();
            emitIntToLong(conv, leftSlot);
            leftSlot = conv;
        }
        if (!rightIsLong) {
            int conv = freshSlot();
            emitIntToLong(conv, rightSlot);
            rightSlot = conv;
        }
        Opcode lop = opcode;
        switch (opcode) {
        case Opcode::ADD:
            lop = Opcode::LADD;
            break;
        case Opcode::SUB:
            lop = Opcode::LSUB;
            break;
        case Opcode::MUL:
            lop = Opcode::LMUL;
            break;
        case Opcode::DIV:
            lop = Opcode::LDIV;
            break;
        case Opcode::MOD:
            lop = Opcode::LMOD;
            break;
        case Opcode::BAND:
            lop = Opcode::LBAND;
            break;
        case Opcode::BOR:
            lop = Opcode::LBOR;
            break;
        case Opcode::BXOR:
            lop = Opcode::LBXOR;
            break;
        case Opcode::SHL:
            lop = Opcode::LSHL;
            break;
        case Opcode::SHR:
            lop = Opcode::LSHR;
            break;
        default:
            break;
        }
        emitBinaryOp(lop, destSlot, leftSlot, rightSlot, line, col);
        return destSlot;
    }

    if (leftIsFloat || rightIsFloat) {
        // ADR-040: sonucun genişliği en geniş operand. double varsa 64-bit path,
        // yoksa float32 path. int/float32 operandlar hedefe yükseltilir.
        bool resultIsDouble = leftIsDouble || rightIsDouble;
        // Tek-operandlı (src→dest) çevrim instruction'ı ekle
        auto emitConv = [&](Opcode op, int dest, int src) {
            Instruction ins(op);
            ins.dest = dest;
            ins.src = src;
            ins.sourceLine = line;
            ins.sourceCol = col;
            currentFunction_->instructions.push_back(std::move(ins));
        };
        auto toFloatTarget = [&](int slot, bool isF32, bool isDbl) -> int {
            if (resultIsDouble) {
                if (isDbl)
                    return slot; // zaten double
                int conv = freshSlot();
                if (isF32)
                    emitConv(Opcode::FLOAT32_TO_FLOAT, conv, slot);
                else
                    emitIntToFloat(conv, slot); // int → double
                return conv;
            } else {
                if (isF32)
                    return slot; // zaten float32
                int conv = freshSlot();
                emitIntToFloat32(conv, slot); // int → float32
                return conv;
            }
        };
        leftSlot = toFloatTarget(leftSlot, leftIsFloat32, leftIsDouble);
        rightSlot = toFloatTarget(rightSlot, rightIsFloat32, rightIsDouble);
        Opcode floatOp = opcode;
        if (resultIsDouble) {
            if (opcode == Opcode::ADD)
                floatOp = Opcode::FADD;
            else if (opcode == Opcode::SUB)
                floatOp = Opcode::FSUB;
            else if (opcode == Opcode::MUL)
                floatOp = Opcode::FMUL;
            else if (opcode == Opcode::DIV)
                floatOp = Opcode::FDIV;
        } else {
            if (opcode == Opcode::ADD)
                floatOp = Opcode::F32ADD;
            else if (opcode == Opcode::SUB)
                floatOp = Opcode::F32SUB;
            else if (opcode == Opcode::MUL)
                floatOp = Opcode::F32MUL;
            else if (opcode == Opcode::DIV)
                floatOp = Opcode::F32DIV;
        }
        emitBinaryOp(floatOp, destSlot, leftSlot, rightSlot, line, col);
    } else {
        emitBinaryOp(opcode, destSlot, leftSlot, rightSlot, line, col);
        // byte ⊕ byte → byte: sonucu 8 bite sar (ADR-040 Faz 4).
        //
        // Kaynak TypeChecker'ın verdiği sonuç tipidir, operandların tipi
        // değil: literal taraf bağlamsal olarak byte tiplenebilir
        // (`a + 100`), ama o ifade int'tir ve sarılmamalıdır. Kuralın tek
        // sahibi TypeChecker'dır (type_checker.cpp "byte aritmetiği" bloğu);
        // burada yalnız kararı uyguluyoruz.
        bool resultIsByte = false;
        if (auto* resultExpr = dynamic_cast<ExpressionNode*>(resultNode))
            resultIsByte = resultExpr->resolvedType.isByte();
        if (resultIsByte)
            destSlot = emitByteWrap(destSlot, line, col);
    }
    return destSlot;
}

// byte ⊕ byte sonucunu 8 bite sarar (ADR-040 Faz 4): `& 0xFF`.
//
// TypeChecker yalnız İKİ operandı da byte olan aritmetik/bitsel ifadeye byte
// tipi verir (type_checker.cpp, "byte aritmetiği" bloğu); karışık işlem int
// kalır ve buraya düşmez. Dolayısıyla bu maskeleme tam olarak tip-içi
// aritmetiği kapsar.
//
// Neden `as byte` cast'i DEĞİL: o cast aralık denetimlidir ve dış veri
// doğrulaması için ayrılmıştır (300 → hata). Tip-içi aritmetik ise sarmalıdır
// (300 → 44). İki niyet ayrı yollardan geçer.
//
// Maliyet tek bir BAND talimatı — çalışma zamanı çağrısı yoktur, VM'de ve
// JIT'te aynı şekilde tek makine talimatına iner.
int IRGenerator::emitByteWrap(int valueSlot, int line, int col) {
    int maskSlot = freshSlot();
    emitLoadConst(maskSlot, 0xFF, SourceLocation{"", line, col, 0});
    int wrappedSlot = freshSlot();
    emitBinaryOp(Opcode::BAND, wrappedSlot, valueSlot, maskSlot, line, col);
    return wrappedSlot;
}

// ─────────────────────────────────────────────────────────────────────────────
// Slot yönetimi
// ─────────────────────────────────────────────────────────────────────────────

int IRGenerator::freshSlot() {
    int s = nextSlot_++;
    // Faz 5: slotNames vektörünü büyüt
    if (currentFunction_ && s >= (int) currentFunction_->slotNames.size())
        currentFunction_->slotNames.resize(s + 1);
    return s;
}

void IRGenerator::registerVariable(const std::string& name, int slot) {
    // #108: aktif bloğun gölge kaydına ismin ÖNCEKİ durumunu (var olan slot
    // ya da yoktu) sakla — popScope() blok kapanışında bunu geri yükler.
    if (!shadowStack_.empty()) {
        auto it = nameToSlot_.find(name);
        if (it != nameToSlot_.end())
            shadowStack_.back().emplace_back(name, it->second);
        else
            shadowStack_.back().emplace_back(name, std::nullopt);
    }

    nameToSlot_[name] = slot;
    // Faz 5: slot → isim eşlemesi (DAP/debug için)
    if (currentFunction_) {
        if (slot >= (int) currentFunction_->slotNames.size())
            currentFunction_->slotNames.resize(slot + 1);
        currentFunction_->slotNames[slot] = name;
    }
}

void IRGenerator::pushScope() {
    shadowStack_.emplace_back();
}

void IRGenerator::popScope() {
    if (shadowStack_.empty())
        return;
    auto record = std::move(shadowStack_.back());
    shadowStack_.pop_back();
    // Geriye doğru uygula: aynı isim birden fazla kez bu blokta bildirilmişse
    // (örn. sibling VariableDecl'ler) en eski önceki-durum geçerli olmalı.
    for (auto it = record.rbegin(); it != record.rend(); ++it) {
        const std::string& name = it->first;
        if (it->second.has_value())
            nameToSlot_[name] = *it->second;
        else
            nameToSlot_.erase(name);
    }
}

int IRGenerator::lookupVariable(const std::string& name) {
    auto it = nameToSlot_.find(name);
    if (it == nameToSlot_.end()) {
        // Bu noktaya normalde gelinmemeli; sembol toplayıcı E001 üretmiş olur.
        // Yine de çökmemek için 0 döndür.
        return 0;
    }
    return it->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// Slot tipi hesaplama (Dilim 1.5, MIRPLAN §3)
// ─────────────────────────────────────────────────────────────────────────────

SlotType IRGenerator::slotTypeFromType(const Type& t) const {
    if (t.isArray() || t.isStruct()) return SlotType::Ref;
    return slotTypeFromTypeName(t.toString());
}

SlotType IRGenerator::slotTypeFromTypeName(const std::string& t) const {
    if (t == "double")
        return SlotType::Float; // 64-bit
    if (t == "float")
        return SlotType::Float32; // ADR-040: 32-bit
    if (t == "longint")
        return SlotType::LongInt; // ADR-040: 64-bit
    if (t == "decimal")
        return SlotType::Decimal;
    if (t == "string")
        return SlotType::Str;
    if (t == "date")
        return SlotType::Date;
    if (t == "int" || t == "bool" || t == "byte")
        return SlotType::Int;
    // Array (`int[]`) veya bilinen struct → referans.
    if (t.size() > 2 && t.substr(t.size() - 2) == "[]")
        return SlotType::Ref;
    if (structLayouts_.count(t))
        return SlotType::Ref;
    // enum → int değeri; void/bilinmeyen → Int (nötr varsayılan).
    return SlotType::Int;
}

void IRGenerator::finalizeSlotTypes(IRFunction* fn, FunctionDeclNode* decl) {
    if (fn->slotCount <= 0) {
        fn->slotTypes.clear();
        return;
    }
    fn->slotTypes.assign(static_cast<size_t>(fn->slotCount), SlotType::Int);

    // 1. Parametre slot'ları (0..paramCount-1) — bildirilen tipten.
    for (size_t i = 0; i < decl->params.size() && i < static_cast<size_t>(fn->slotCount); ++i)
        fn->slotTypes[i] = slotTypeFromTypeName(decl->params[i]->varType);

    // 2. Üreten opcode'dan türet. Slot türü sabit olduğundan (ADR-020) tek
    // yön yeterli, ama LOAD_SLOT propagasyonu geriye-atlamalarda gecikebilir
    // → küçük bir fixpoint (tavan 8) güvenli ve ucuz.
    auto kindOf = [&](int slot) -> SlotType {
        return (slot >= 0 && slot < fn->slotCount) ? fn->slotTypes[static_cast<size_t>(slot)] :
                                                     SlotType::Int;
    };
    bool changed = true;
    for (int guard = 0; changed && guard < 8; ++guard) {
        changed = false;
        for (const Instruction& ins : fn->instructions) {
            if (ins.dest < 0 || ins.dest >= fn->slotCount)
                continue;
            SlotType cur = fn->slotTypes[static_cast<size_t>(ins.dest)];
            SlotType nk = cur;
            switch (ins.opcode) {
            case Opcode::LOAD_FLOAT:
            case Opcode::FADD:
            case Opcode::FSUB:
            case Opcode::FMUL:
            case Opcode::FDIV:
            case Opcode::FNEG:
            case Opcode::INT_TO_FLOAT:
            case Opcode::FLOAT32_TO_FLOAT:
                nk = SlotType::Float;
                break;
            // ADR-040: float32 (32-bit) üreten op'lar
            case Opcode::LOAD_FLOAT32:
            case Opcode::F32ADD:
            case Opcode::F32SUB:
            case Opcode::F32MUL:
            case Opcode::F32DIV:
            case Opcode::F32NEG:
            case Opcode::INT_TO_FLOAT32:
            case Opcode::FLOAT_TO_FLOAT32:
            case Opcode::CAST_STR_TO_FLOAT32:
                nk = SlotType::Float32;
                break;
            // ADR-040: longint (64-bit) üreten op'lar
            case Opcode::LOAD_LONG:
            case Opcode::LADD:
            case Opcode::LSUB:
            case Opcode::LMUL:
            case Opcode::LDIV:
            case Opcode::LMOD:
            case Opcode::LNEG:
            case Opcode::LBAND:
            case Opcode::LBOR:
            case Opcode::LBXOR:
            case Opcode::LSHL:
            case Opcode::LSHR:
            case Opcode::LBNOT:
            case Opcode::INT_TO_LONG:
            case Opcode::CAST_STR_TO_LONG:
            case Opcode::CAST_FLOAT_TO_LONG_CHECKED:
                nk = SlotType::LongInt;
                break;
            case Opcode::STRUCT_NEW:
            case Opcode::ARRAY_NEW:
                nk = SlotType::Ref;
                break;
            case Opcode::LOAD_STRING:
            case Opcode::STRING_CONCAT:
            case Opcode::CAST_INT_TO_STR:
            case Opcode::CAST_FLOAT_TO_STR:
            case Opcode::CAST_FLOAT32_TO_STR:
            case Opcode::CAST_LONG_TO_STR:
            case Opcode::CAST_BOOL_TO_STR:
            case Opcode::CAST_DECIMAL_TO_STR:
                nk = SlotType::Str;
                break;
            case Opcode::CAST_STR_TO_FLOAT:
            case Opcode::CAST_DECIMAL_TO_FLOAT:
                nk = SlotType::Float;
                break;
            // CAST_STR_TO_INT / CAST_FLOAT_TO_INT_CHECKED /
            // CAST_INT_TO_BYTE_CHECKED / CAST_DECIMAL_TO_INT → Int (default).
            case Opcode::LOAD_DECIMAL:
            case Opcode::DADD:
            case Opcode::DSUB:
            case Opcode::DMUL:
            case Opcode::DDIV:
            case Opcode::DMOD:
            case Opcode::DNEG:
            case Opcode::INT_TO_DECIMAL:
            case Opcode::FLOAT_TO_DECIMAL:
            case Opcode::CAST_STR_TO_DECIMAL:
                nk = SlotType::Decimal;
                break;
            case Opcode::LOAD_SLOT:
                nk = kindOf(ins.src);
                break;
            case Opcode::CALL: {
                auto it = funcReturnKind_.find(ins.functionName);
                if (it != funcReturnKind_.end())
                    nk = it->second;
                break;
            }
            case Opcode::CALLHOST: {
                if (ins.valueType != SlotType::Unknown) { nk = ins.valueType; break; }
                // #227: dönüş türü registry'den gelir. Bu bilgi olmadan JIT
                // double dönen bir host fonksiyonunun sonucunu Int register'a
                // yazmaya çalışır ve MIR tip hatası verir (ölçüldü: MATH_PI).
                const HostEntry* he = hostEntryAt(ins.intValue);
                if (he) {
                    switch (he->retKind) {
                        case HostKind::Float:   nk = SlotType::Float;   break;
                        case HostKind::Float32: nk = SlotType::Float32; break;
                        case HostKind::LongInt: nk = SlotType::LongInt; break;
                        case HostKind::Date:    nk = SlotType::Date;    break;
                        case HostKind::Str:     nk = SlotType::Str;     break;
                        case HostKind::Decimal: nk = SlotType::Decimal; break;
                        case HostKind::Ref:     nk = SlotType::Ref;     break;
                        default: break;   // Int / Void / Null → Int
                    }
                }
                break;
            }
            case Opcode::ARRAY_GET:
            case Opcode::FIELD_GET:
            case Opcode::LOAD_GLOBAL:
                if (ins.valueType != SlotType::Unknown) nk = ins.valueType;
                break;
            // FIELD_GET/ARRAY_GET/LOAD_GLOBAL: sonuç türü opcode'dan
            // belli değil (eleman/alan türü gerekir). Dilim 1.5 JIT'i bu
            // opcode'ları zaten reddediyor; `--types` için Int kalır.
            // TODO(Dilim 2/3): bu opcode'lara sonuç-türü alanı ekle.
            default:
                break; // Int-üreten opcode'lar: varsayılan Int
            }
            if (nk != cur) {
                fn->slotTypes[static_cast<size_t>(ins.dest)] = nk;
                changed = true;
            }
        }
    }

    // 3. Nullable maskesi (#221). slotTypes'tan AYRI bir fixpoint çünkü farklı
    // bir soruya cevap verir: "bu slot null TUTABİLİR mi?" — değer türü değil.
    //
    // Kaynaklar:
    //   - LOAD_NULL dest'i    → doğrudan nullable
    //   - bildirilen `T?` parametre → nullable
    //   - LOAD_SLOT           → kaynaktan yayılır
    // Tek yönlü ve monoton (bir kez nullable olan geri dönmez) → fixpoint sonlu.
    fn->slotNullable.assign(static_cast<size_t>(fn->slotCount), false);

    auto markNullable = [&](int slot) -> bool {
        if (slot < 0 || slot >= fn->slotCount) return false;
        if (fn->slotNullable[static_cast<size_t>(slot)]) return false;
        fn->slotNullable[static_cast<size_t>(slot)] = true;
        return true;
    };

    for (size_t i = 0; i < decl->params.size() && i < static_cast<size_t>(fn->slotCount); ++i)
        if (decl->params[i]->varType.size() > 0 && decl->params[i]->varType.back() == '?')
            markNullable(static_cast<int>(i));

    changed = true;
    for (int guard = 0; changed && guard < 8; ++guard) {
        changed = false;
        for (const Instruction& ins : fn->instructions) {
            switch (ins.opcode) {
            case Opcode::LOAD_NULL:
                if (markNullable(ins.dest)) changed = true;
                break;
            case Opcode::CALLHOST:
                if (ins.valueNullable && markNullable(ins.dest)) changed = true;
                break;
            case Opcode::CALL:
                if (ins.valueNullable && markNullable(ins.dest)) changed = true;
                break;
            case Opcode::FIELD_GET:
                if (ins.valueNullable && markNullable(ins.dest)) changed = true;
                break;
            case Opcode::CAST_STR_TO_INT:
            case Opcode::CAST_STR_TO_FLOAT:
            case Opcode::CAST_FLOAT_TO_INT_CHECKED:
            case Opcode::CAST_INT_TO_BYTE_CHECKED:
            case Opcode::CAST_STR_TO_LONG:
            case Opcode::CAST_STR_TO_FLOAT32:
            case Opcode::CAST_FLOAT_TO_LONG_CHECKED:
            case Opcode::LONG_TO_INT_CHECKED:
            case Opcode::CAST_DECIMAL_TO_INT:
            case Opcode::CAST_STR_TO_DECIMAL:
                if (ins.left == 1 && markNullable(ins.dest)) changed = true;
                break;
            case Opcode::LOAD_SLOT:
                if (ins.src >= 0 && ins.src < fn->slotCount &&
                    fn->slotNullable[static_cast<size_t>(ins.src)])
                    if (markNullable(ins.dest)) changed = true;
                break;
            default:
                break;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Talimat yazma yardımcıları
// ─────────────────────────────────────────────────────────────────────────────

void IRGenerator::emitLoadConst(int destSlot, int value, const SourceLocation& loc) {
    Instruction ins(Opcode::LOAD_CONST);
    ins.dest = destSlot;
    ins.intValue = value;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

void IRGenerator::emitLoadSlot(int destSlot, int srcSlot, const SourceLocation& loc) {
    Instruction ins(Opcode::LOAD_SLOT);
    ins.dest = destSlot;
    ins.src = srcSlot;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

void IRGenerator::emitLoadGlobal(int destSlot, int globalIndex, const SourceLocation& loc) {
    Instruction ins(Opcode::LOAD_GLOBAL);
    ins.dest = destSlot;
    ins.intValue = globalIndex;
    auto gt = globalSlotTypes_.find(globalIndex);
    if (gt != globalSlotTypes_.end()) ins.valueType = gt->second;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

void IRGenerator::emitStoreGlobal(int srcSlot, int globalIndex, const SourceLocation& loc) {
    Instruction ins(Opcode::STORE_GLOBAL);
    ins.src = srcSlot;
    ins.intValue = globalIndex;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

void IRGenerator::emitLoadFloat(int destSlot, double value, const SourceLocation& loc) {
    Instruction ins(Opcode::LOAD_FLOAT);
    ins.dest = destSlot;
    ins.floatValue = value;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

void IRGenerator::emitIntToFloat(int destSlot, int srcSlot, const SourceLocation& loc) {
    Instruction ins(Opcode::INT_TO_FLOAT);
    ins.dest = destSlot;
    ins.src = srcSlot;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

// ADR-040: float32 sabit yükle
void IRGenerator::emitLoadFloat32(int destSlot, double value, const SourceLocation& loc) {
    Instruction ins(Opcode::LOAD_FLOAT32);
    ins.dest = destSlot;
    ins.floatValue = value;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

// ADR-040: longint (64-bit) sabit yükle
void IRGenerator::emitLoadLong(int destSlot, long long value, const SourceLocation& loc) {
    Instruction ins(Opcode::LOAD_LONG);
    ins.dest = destSlot;
    ins.int64Value = value;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

// ADR-040: int → float32 çevrimi
void IRGenerator::emitIntToFloat32(int destSlot, int srcSlot, const SourceLocation& loc) {
    Instruction ins(Opcode::INT_TO_FLOAT32);
    ins.dest = destSlot;
    ins.src = srcSlot;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

// ADR-040: int → longint çevrimi (kayıpsız genişletme)
void IRGenerator::emitIntToLong(int destSlot, int srcSlot, const SourceLocation& loc) {
    Instruction ins(Opcode::INT_TO_LONG);
    ins.dest = destSlot;
    ins.src = srcSlot;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

void IRGenerator::emitLoadDecimal(int destSlot, const DecimalValue& value,
                                  const SourceLocation& loc) {
    Instruction ins(Opcode::LOAD_DECIMAL);
    ins.dest = destSlot;
    ins.decimalValue = value;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

void IRGenerator::emitIntToDecimal(int destSlot, int srcSlot, const SourceLocation& loc) {
    Instruction ins(Opcode::INT_TO_DECIMAL);
    ins.dest = destSlot;
    ins.src = srcSlot;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

void IRGenerator::emitFloatToDecimal(int destSlot, int srcSlot, const SourceLocation& loc) {
    Instruction ins(Opcode::FLOAT_TO_DECIMAL);
    ins.dest = destSlot;
    ins.src = srcSlot;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

// ADR-021: nullable slot'un zero-init değeri. `T?` tipli bir şeye açık değer
// verilmediğinde slot Value{} (= Int 0) kalmamalı, LOAD_NULL ile null olmalı.
void IRGenerator::emitLoadNull(int destSlot, const SourceLocation& loc) {
    Instruction ins(Opcode::LOAD_NULL);
    ins.dest = destSlot;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

void IRGenerator::emitLoadString(int destSlot, std::string value,
                                 const SourceLocation& loc) {
    Instruction ins(Opcode::LOAD_STRING);
    ins.dest = destSlot;
    ins.stringValue = std::move(value);
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

void IRGenerator::emitStructNew(int destSlot, const std::string& structType, int fieldCount,
                                const SourceLocation& loc) {
    Instruction ins(Opcode::STRUCT_NEW);
    ins.dest = destSlot;
    ins.intValue = fieldCount;
    ins.functionName = structType;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    // Alan adlarını struct layout'tan al — 218: IRFunction metadata'ya taşı
    auto it = structLayouts_.find(structType);
    if (it != structLayouts_.end()) {
        std::vector<std::string> names;
        std::vector<bool>        nullableMask;
        for (const auto& kv : it->second) {
            names.push_back(kv.first);
            // ADR-021: `T? alan` zero-init'te null başlamalı. Bu bilgi burada
            // yakalanmazsa IR→VM sınırında silinir ve VM bütün alanları
            // Value{} (= Int 0) ile doldurur.
            nullableMask.push_back(kv.second.nullable);
        }
        currentFunction_->structFieldNames[structType]    = std::move(names);
        currentFunction_->structFieldNullable[structType] = std::move(nullableMask);
    }
    currentFunction_->instructions.push_back(std::move(ins));
}

void IRGenerator::emitFieldGet(int destSlot, int objSlot, int fieldIdx,
                                const SourceLocation& loc, SlotType valueType,
                                bool valueNullable) {
    Instruction ins(Opcode::FIELD_GET);
    ins.dest = destSlot;
    ins.src = objSlot;
    ins.intValue = fieldIdx;
    ins.valueType = valueType;
    ins.valueNullable = valueNullable;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

void IRGenerator::emitFieldSet(int objSlot, int fieldIdx, int valSlot, int line, int col) {
    Instruction ins(Opcode::FIELD_SET);
    ins.dest = objSlot;
    ins.intValue = fieldIdx;
    ins.right = valSlot;
    ins.sourceLine = (line == 0 && col == 0 && currentLoc_.isValid()) ? currentLoc_.line : line;
    ins.sourceCol = (line == 0 && col == 0 && currentLoc_.isValid()) ? currentLoc_.column : col;
    currentFunction_->instructions.push_back(std::move(ins));
}

void IRGenerator::emitArrayNew(int destSlot, int capacity, ArrayElemKind k,
                               const SourceLocation& loc) {
    Instruction ins(Opcode::ARRAY_NEW);
    ins.dest = destSlot;
    ins.intValue = capacity;
    ins.arrayElemKind = k;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

// #206: tip-adı string'inden ArrayElemKind çıkar (ör. "byte" → Byte)
static ArrayElemKind arrayElemKindFromTypeName(const std::string& t) {
    if (t == "byte")     return ArrayElemKind::Byte;
    if (t == "int" || t == "bool" || t == "char")
                         return ArrayElemKind::Int;
    if (t == "longint" || t == "date")
                         return ArrayElemKind::LongInt;
    if (t == "float")    return ArrayElemKind::Float32;
    if (t == "double")   return ArrayElemKind::Float64;
    if (t == "decimal")  return ArrayElemKind::Decimal;
    // string, struct, enum, array → Ref
    return ArrayElemKind::Ref;
}

// #206: Type::PrimitiveKind'den ArrayElemKind çıkar
static ArrayElemKind arrayElemKindFromPrim(PrimitiveKind p) {
    switch (p) {
        case PrimitiveKind::Byte:    return ArrayElemKind::Byte;
        case PrimitiveKind::Int:
        case PrimitiveKind::Bool:
        case PrimitiveKind::Char:    return ArrayElemKind::Int;
        case PrimitiveKind::LongInt:
        case PrimitiveKind::Date:    return ArrayElemKind::LongInt;
        case PrimitiveKind::Float:   return ArrayElemKind::Float32;
        case PrimitiveKind::Double:  return ArrayElemKind::Float64;
        case PrimitiveKind::Decimal: return ArrayElemKind::Decimal;
        default:                     return ArrayElemKind::Ref;
    }
}

// #206: Type'tan ArrayElemKind çıkar (elementType alanına bakar)
static ArrayElemKind arrayElemKindFromType(const Type& t) {
    if (!t.isArray() || !t.elementType) return ArrayElemKind::Ref;
    return arrayElemKindFromPrim(t.elementType->prim);
}

void IRGenerator::emitArrayGet(int destSlot, int arrSlot, int idxSlot, int line, int col,
                                SlotType valueType, ArrayElemKind elemKind) {
    Instruction ins(Opcode::ARRAY_GET);
    ins.dest = destSlot;
    ins.left = arrSlot;
    ins.right = idxSlot;
    ins.valueType = valueType;
    // #206: packed eleman tipi. ARRAY_NEW'de zaten taşınıyordu; ARRAY_GET'te
    // eksikti ve JIT doğrudan bellek erişimi için onu okuyor — eksik olduğu
    // sürece her eleman tipi Ref görünüyor ve trampoline düşüyordu.
    ins.arrayElemKind = elemKind;
    ins.sourceLine = (line == 0 && col == 0 && currentLoc_.isValid()) ? currentLoc_.line : line;
    ins.sourceCol = (line == 0 && col == 0 && currentLoc_.isValid()) ? currentLoc_.column : col;
    currentFunction_->instructions.push_back(std::move(ins));
}

void IRGenerator::emitArraySet(int arrSlot, int idxSlot, int valSlot, int line, int col,
                                ArrayElemKind elemKind) {
    Instruction ins(Opcode::ARRAY_SET);
    ins.dest = arrSlot;
    ins.left = idxSlot;
    ins.right = valSlot;
    // #206: packed eleman tipi — ARRAY_GET ile aynı gerekçe. Eksik olduğu
    // sürece JIT her eleman tipini Ref görüp trampoline düşüyordu.
    ins.arrayElemKind = elemKind;
    ins.sourceLine = (line == 0 && col == 0 && currentLoc_.isValid()) ? currentLoc_.line : line;
    ins.sourceCol = (line == 0 && col == 0 && currentLoc_.isValid()) ? currentLoc_.column : col;
    currentFunction_->instructions.push_back(std::move(ins));
}

void IRGenerator::emitArrayLen(int destSlot, int arrSlot, const SourceLocation& loc) {
    Instruction ins(Opcode::ARRAY_LEN);
    ins.dest = destSlot;
    ins.src = arrSlot;
    auto el = effectiveLoc(loc);
    ins.sourceLine = el.line;
    ins.sourceCol = el.column;
    ins.sourceFile = el.filePath();
    currentFunction_->instructions.push_back(std::move(ins));
}

// Zero-init'te iç struct alanlarını örnekler.
//
// NULLABLE ALAN → null (örneklenmez):
//   `T? alan` "bu alan olmayabilir" demektir (ADR-021); zero-init'in doğru
//   başlangıç değeri boş bir T örneği değil, null'dır. Bu aynı zamanda
//   ÖZYİNELEMELİ struct'ları mümkün kılar:
//
//       struct Node { int val; Node? next; }
//       Node n;                    // next = null → zincir burada biter
//
//   Eskiden nullable alan da koşulsuz örnekleniyordu; her `next` yeni bir
//   `Node` doğuruyor, o da kendi `next`'ini doğuruyordu → sonsuz özyineleme,
//   stack tükenmesi, SIGSEGV (`saqut ir` ve `saqut run` exit 139). `check`
//   aşaması bunu kabul ediyordu, yani hata yalnız IR üretiminde patlıyordu.
//
// NON-NULLABLE İÇ STRUCT → örneklenir (davranış değişmedi):
//   `Inner inner;` alanı null olamaz, zero-init onu üretmek zorundadır.
//   Bu yol kendi kendine sonlanır: non-nullable bir alan kendi tipini
//   doğrudan ya da dolaylı içeremez (aksi halde sonsuz boyutlu bir değer
//   olurdu) — o yüzden ayrıca derinlik sayacı gerekmez. Yine de yanlış bir
//   layout'a karşı savunma olarak ziyaret zinciri takip edilir.
//
// NON-NULLABLE ARRAY ALANI → boş dizi örneklenir (#184/#226): local
// `T[] a;` bildirimi boş diziyle başlar; struct alanı da aynı semantiği
// izler. Aksi halde alan Value{} (= Int 0) kalır ve `s.items.push(x)`
// check'ten geçip runtime'da "push — expected array" ile patlar —
// sessiz-derleme/çatırdayan-çalışma asimetrisi tam olarak buydu.
void IRGenerator::initNestedStructFields(int destSlot, const std::string& structType,
                                         const SourceLocation& loc,
                                         std::vector<std::string>* activeChain) {
    auto it = structLayouts_.find(structType);
    if (it == structLayouts_.end())
        return;

    // Savunma: non-nullable bir döngü (A içinde B, B içinde A) semantic
    // katmanda reddedilmeli. Buraya kadar geldiyse IR'ı çökertmek yerine
    // zinciri kes — sessiz yanlış değer değil, eksik init; ve derleyici
    // ayakta kalır.
    std::vector<std::string> localChain;
    if (!activeChain) activeChain = &localChain;
    for (const auto& seen : *activeChain)
        if (seen == structType) return;
    activeChain->push_back(structType);

    for (int i = 0; i < (int) it->second.size(); i++) {
        const auto& [fieldName, fieldType] = it->second[i];
        if (fieldType.nullable)     // `T? alan` → null kalır, örneklenmez
            continue;
        if (fieldType.isString()) {
            // #184 ürün kararı: non-nullable string alanı "" ile başlar.
            // Aksi halde alan Value{} (= Int 0) kalır ve s.name.length()
            // "expected string" ile patlar; local `string s;` ile aynı
            // sözleşme (b9f5c62'nin array dalıyla aynı desen).
            int strSlot = freshSlot();
            emitLoadString(strSlot, "", loc);
            emitFieldSet(destSlot, i, strSlot, loc.line, loc.column);
            continue;
        }
        if (fieldType.isArray()) {
            // #184/#226: non-nullable array alanı boş diziyle başlar —
            // local `T[] a;` ile aynı sözleşme. elemKind layout Type'ından
            // çözülür (arrayElemKindFromType), capacity=0.
            int arrSlot = freshSlot();
            emitArrayNew(arrSlot, 0, arrayElemKindFromType(fieldType), loc);
            emitFieldSet(destSlot, i, arrSlot, loc.line, loc.column);
            continue;
        }
        if (!fieldType.isStruct() || fieldType.structName.empty())
            continue;
        if (!structLayouts_.count(fieldType.structName))
            continue;
        int innerSlot = freshSlot();
        int innerFc = getStructFieldCount(fieldType.structName);
        emitStructNew(innerSlot, fieldType.structName, innerFc, loc);
        initNestedStructFields(innerSlot, fieldType.structName, loc, activeChain);
        emitFieldSet(destSlot, i, innerSlot, loc.line, loc.column);
    }

    activeChain->pop_back();
}

int IRGenerator::getStructFieldIndex(const std::string& structType,
                                     const std::string& fieldName) const {
    auto it = structLayouts_.find(structType);
    if (it == structLayouts_.end())
        return -1;
    for (int i = 0; i < (int) it->second.size(); i++)
        if (it->second[i].first == fieldName)
            return i;
    return -1;
}

int IRGenerator::getStructFieldCount(const std::string& structType) const {
    auto it = structLayouts_.find(structType);
    if (it == structLayouts_.end())
        return 0;
    return (int) it->second.size();
}

bool IRGenerator::isGlobal(const std::string& name) const {
    return nameToGlobal_.count(name) > 0;
}

int IRGenerator::getGlobalIndex(const std::string& name) const {
    auto it = nameToGlobal_.find(name);
    return (it != nameToGlobal_.end()) ? it->second : -1;
}

void IRGenerator::emitBinaryOp(Opcode op, int destSlot, int leftSlot, int rightSlot, int line,
                               int col) {
    Instruction ins(op);
    ins.dest = destSlot;
    ins.left = leftSlot;
    ins.right = rightSlot;
    ins.sourceLine = (line == 0 && col == 0 && currentLoc_.isValid()) ? currentLoc_.line : line;
    ins.sourceCol = (line == 0 && col == 0 && currentLoc_.isValid()) ? currentLoc_.column : col;
    currentFunction_->instructions.push_back(std::move(ins));
}

void IRGenerator::emitReturn(int srcSlot, int line, int col) {
    Instruction ins(Opcode::RETURN);
    ins.src = srcSlot;
    ins.sourceLine = (line == 0 && col == 0 && currentLoc_.isValid()) ? currentLoc_.line : line;
    ins.sourceCol = (line == 0 && col == 0 && currentLoc_.isValid()) ? currentLoc_.column : col;
    currentFunction_->instructions.push_back(std::move(ins));
}

int IRGenerator::emitJumpUnconditional(int targetInstrIndex) {
    Instruction ins(Opcode::JMP);
    ins.jumpTarget = targetInstrIndex;
    currentFunction_->instructions.push_back(std::move(ins));
    return (int) currentFunction_->instructions.size() - 1;
}

int IRGenerator::emitJumpIfFalse(int condSlot) {
    Instruction ins(Opcode::JIF_FALSE);
    ins.cond = condSlot;
    ins.jumpTarget = -1; // henüz bilinmiyor — patchJump() bekliyor
    currentFunction_->instructions.push_back(std::move(ins));
    return (int) currentFunction_->instructions.size() - 1;
}

int IRGenerator::emitJumpIfTrue(int condSlot) {
    Instruction ins(Opcode::JIF_TRUE);
    ins.cond = condSlot;
    ins.jumpTarget = -1;
    currentFunction_->instructions.push_back(std::move(ins));
    return (int) currentFunction_->instructions.size() - 1;
}

void IRGenerator::patchJump(int instrIndex) {
    // instrIndex'teki JMP veya JIF_FALSE'un hedefini şu anki konuma doldur
    currentFunction_->instructions[instrIndex].jumpTarget = currentInstrIndex();
}

int IRGenerator::currentInstrIndex() const {
    return (int) currentFunction_->instructions.size();
}
