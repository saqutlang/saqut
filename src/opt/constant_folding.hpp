// ============================================================================
// saQut — Sabit Katlama (Constant Folding) Pass (ADR-009)
//
// BinaryExpression(Literal, OP, Literal) → Literal
// Sadece tam sayı sabitleri katlanır; kayan nokta şimdilik dışarıda.
// Derleme zamanı sıfıra bölme: W002 uyarısı verilir, katlama yapılmaz.
// ============================================================================

#ifndef SAQUT_OPT_CONSTANT_FOLDING
#define SAQUT_OPT_CONSTANT_FOLDING

#include "core/int_arithmetic.hpp"
#include "diagnostic/diagnostic_engine.hpp"
#include "opt/optimization_pass.hpp"
#include "parser/nodes/binary_expr.hpp"
#include "parser/nodes/declarations.hpp"
#include "parser/nodes/expressions.hpp"
#include "parser/nodes/identifier.hpp"
#include "parser/nodes/literal.hpp"
#include "parser/nodes/program.hpp"
#include "parser/nodes/statements.hpp"

#include <climits>
#include <string>

class ConstantFoldingPass : public OptimizationPass {
public:
    explicit ConstantFoldingPass(DiagnosticEngine& diag)
        : diag_(diag) {}

    bool run(ASTNode* root, SymbolTable*) override {
        changed_ = false;
        fold(root);
        return changed_;
    }

    const std::string& name() const override {
        static std::string n = "constant-folding";
        return n;
    }

private:
    DiagnosticEngine& diag_;
    bool changed_ = false;

    // ── Literal değer okuma ──────────────────────────────────────────────────
    // ADR-040: constant folding int (32-bit) üzerinde çalışır. int32 aralığı
    // dışındaki tamsayı literalleri (longint) katlanmaz — IR generator onları
    // LOAD_LONG olarak üretir, 64-bit semantiği korunur.
    static bool fitsInt32(const std::string& tok, int base) {
        try {
            long long v = parseIntegerLiteral(tok, base);
            return v >= INT_MIN && v <= INT_MAX;
        } catch (...) {
            return false;
        } // taşma → int32'ye sığmaz
    }

    static bool isIntLit(ASTNode* node) {
        auto* lit = dynamic_cast<LiteralNode*>(node);
        if (!lit || lit->literalType != LiteralType::INTEGER)
            return false;
        if (lit->hasDirectValue)
            return true; // zaten int32 directIntValue
        return lit->parserToken.token && fitsInt32(lit->parserToken.token->token, lit->literalBase);
    }

    // Boolean veya integer literal — unary ! için her ikisi de geçerli.
    // int32-dışı (longint) tamsayı literalleri hariç (bkz. isIntLit).
    static bool isScalarLit(ASTNode* node) {
        auto* lit = dynamic_cast<LiteralNode*>(node);
        if (!lit)
            return false;
        if (lit->literalType == LiteralType::BOOLEAN)
            return true;
        return isIntLit(node);
    }

    static int getIntVal(LiteralNode* lit) {
        if (lit->hasDirectValue)
            return lit->directIntValue;
        if (lit->parserToken.token) {
            try {
                return static_cast<int>(
                    parseIntegerLiteral(lit->parserToken.token->token, lit->literalBase));
            } catch (...) {
                return 0;
            } // isIntLit zaten int32-dışını eledi
        }
        return 0;
    }

    // Boolean literali int'e çevir: "true"→1, "false"→0
    static int getScalarVal(LiteralNode* lit) {
        if (lit->literalType == LiteralType::BOOLEAN) {
            if (!lit->parserToken.token)
                return 0;
            return (lit->parserToken.token->token == "true") ? 1 : 0;
        }
        return getIntVal(lit);
    }

    // Unary operatör katlama
    static bool canFoldUnary(TokenType op) {
        switch (op) {
        case TokenType::BANG: // !
        case TokenType::TILDE: // ~
        case TokenType::MINUS: // - (negatif)
        case TokenType::PLUS: // + (pozitif, no-op)
            return true;
        default:
            return false;
        }
    }

    static int computeUnary(TokenType op, int v) {
        switch (op) {
        case TokenType::BANG:
            return !v ? 1 : 0;
        case TokenType::TILDE:
            return ~v;
        case TokenType::MINUS:
            // -INT_MIN taşar; VM ile aynı tanımlı sonucu vermek için wrap.
            return saqut::intmath::wrapNegI32(v);
        case TokenType::PLUS:
            return v;
        default:
            return v;
        }
    }

    // ── Operatör hesaplama ───────────────────────────────────────────────────
    static bool canFoldOp(TokenType op) {
        switch (op) {
        case TokenType::PLUS:
        case TokenType::MINUS:
        case TokenType::STAR:
        case TokenType::SLASH:
        case TokenType::PERCENT:
        case TokenType::STAR_STAR:   // #237: üs — negatif üs çağrandan önce elenir
        case TokenType::EQUAL_EQUAL:
        case TokenType::BANG_EQUAL:
        case TokenType::LESS:
        case TokenType::GREATER:
        case TokenType::LESS_EQUAL:
        case TokenType::GREATER_EQUAL:
        case TokenType::AMPERSAND:
        case TokenType::PIPE:
        case TokenType::LSHIFT:
        case TokenType::RSHIFT:
        case TokenType::AMPERSAND_AMPERSAND:
        case TokenType::PIPE_PIPE:
            return true;
        default:
            return false;
        }
    }

    // Aritmetik core/int_arithmetic.hpp'den gelir — VM ile AYNI fonksiyonlar.
    //
    // Neden düz `l + r` yazılamaz: C++'ta signed overflow UB'dir, saQut'ta ise
    // tanımlı wrap'tir (ADR-040). Düz yazım, katlanmış ifadeyi katlanmamış
    // olandan ayırabilir — ve optimizasyon artık VARSAYILAN AÇIK olduğundan
    // bu ayrışma doğrudan kullanıcıya ulaşırdı. Aynı gerekçe INT_MIN/-1
    // (donanım tuzağı) ve kaydırma maskelemesi için de geçerlidir.
    static int computeOp(TokenType op, int l, int r) {
        switch (op) {
        case TokenType::PLUS:
            return saqut::intmath::wrapAddI32(l, r);
        case TokenType::MINUS:
            return saqut::intmath::wrapSubI32(l, r);
        case TokenType::STAR:
            return saqut::intmath::wrapMulI32(l, r);
        case TokenType::SLASH:
            return saqut::intmath::wrapDivI32(l, r);  // b == 0 çağrandan önce elenir (W002)
        case TokenType::PERCENT:
            return saqut::intmath::wrapModI32(l, r);  // b == 0 çağrandan önce elenir (W002)
        case TokenType::STAR_STAR:
            // #237: VM/JIT ile AYNI gövde — tekrarlı çarpma, libm pow() değil.
            // Negatif üs aşağıdaki guard'da elenir (W003).
            return saqut::intmath::wrapPowI32(l, r);
        case TokenType::EQUAL_EQUAL:
            return l == r ? 1 : 0;
        case TokenType::BANG_EQUAL:
            return l != r ? 1 : 0;
        case TokenType::LESS:
            return l < r ? 1 : 0;
        case TokenType::GREATER:
            return l > r ? 1 : 0;
        case TokenType::LESS_EQUAL:
            return l <= r ? 1 : 0;
        case TokenType::GREATER_EQUAL:
            return l >= r ? 1 : 0;
        case TokenType::AMPERSAND:
            return l & r;
        case TokenType::PIPE:
            return l | r;
        case TokenType::LSHIFT:
            return saqut::intmath::wrapShlI32(l, r);
        case TokenType::RSHIFT:
            return saqut::intmath::wrapShrI32(l, r);
        case TokenType::AMPERSAND_AMPERSAND:
            return (l && r) ? 1 : 0;
        case TokenType::PIPE_PIPE:
            return (l || r) ? 1 : 0;
        default:
            return 0;
        }
    }

    // ── Sentetik literal oluştur ─────────────────────────────────────────────
    static LiteralNode* makeFoldedLit(int value, const SourceLocation& loc, const Type& type) {
        auto* lit = new LiteralNode();
        lit->loc = loc;
        lit->literalType = LiteralType::INTEGER;
        lit->hasDirectValue = true;
        lit->directIntValue = value;
        lit->resolvedType = type;
        lit->isConstant = true;
        return lit;
    }

    // ── Ana ziyaretçi: bottom-up, pointer-by-reference ───────────────────────
    // Dönüş değeri: aynı node veya yerine geçen yeni node.
    // Folding yapıldıysa orijinal node silinir, yeni node döndürülür.
    ASTNode* fold(ASTNode* node) {
        if (!node)
            return nullptr;

        switch (node->kind) {
        // ── İfadeler ────────────────────────────────────────────────────────
        case ASTKind::BinaryExpression: {
            auto* bin = static_cast<BinaryExpressionNode*>(node);

            // Alt ağaçları önce kat
            ASTNode* newLeft = fold(bin->Left);
            if (newLeft != bin->Left) {
                bin->Left = newLeft;
                if (newLeft)
                    newLeft->parent = bin;
            }

            ASTNode* newRight = fold(bin->Right);
            if (newRight != bin->Right) {
                bin->Right = newRight;
                if (newRight)
                    newRight->parent = bin;
            }

            // Katlama int32 semantiğiyle hesaplar. Tip denetleyici ifadeyi
            // longint tiplediyse (longint bağlamındaki literaller, #262)
            // katlamak sonucu DEĞİŞTİRİR: `longint l = 1000000 * 1000000;`
            // katlanınca -727379968, katlanmayınca 10^12 veriyordu (ADR-038:
            // optimizasyon gözlenen sonucu değiştiremez). Bu ifadeler IR'ye
            // bırakılır; LMUL/LADD 64-bit hesaplar.
            auto isLongTyped = [](ASTNode* n) {
                auto* e = dynamic_cast<ExpressionNode*>(n);
                return e && e->resolvedType.isLongInt();
            };
            if (bin->resolvedType.isLongInt() || isLongTyped(bin->Left) || isLongTyped(bin->Right))
                return bin;

            // ── Unary dal: Left==nullptr → !x, ~x, -x, +x ──────────────────
            if (bin->Left == nullptr) {
                if (!isScalarLit(bin->Right))
                    return bin;
                if (!canFoldUnary(bin->Operator))
                    return bin;

                auto* rlit = static_cast<LiteralNode*>(bin->Right);
                int rv = getScalarVal(rlit);
                int result = computeUnary(bin->Operator, rv);

                LiteralNode* lit = makeFoldedLit(result, bin->loc, bin->resolvedType);
                delete bin;
                changed_ = true;
                return lit;
            }

            // ── Binary dal: iki taraf da tam sayı sabiti ────────────────────
            if (!isIntLit(bin->Left) || !isIntLit(bin->Right))
                return bin;
            if (!canFoldOp(bin->Operator))
                return bin;

            auto* llit = static_cast<LiteralNode*>(bin->Left);
            auto* rlit = static_cast<LiteralNode*>(bin->Right);
            int lv = getIntVal(llit);
            int rv = getIntVal(rlit);

            // #237: derleme zamanı negatif üs — katlama atlanır, runtime
            // hatası (E_POWNEG) VM/JIT'te üretilir. Burada sessizce yanlış
            // bir sabit üretmek en kötü seçenek olurdu.
            if (bin->Operator == TokenType::STAR_STAR && rv < 0)
                return bin; // fold etme

            // W002: Derleme zamanı sıfıra bölme
            if ((bin->Operator == TokenType::SLASH || bin->Operator == TokenType::PERCENT) &&
                rv == 0) {
                Diagnostic d;
                d.level = DiagLevel::Warning;
                d.code = "W002";
                d.loc = bin->loc;
                d.message = "compile-time division by zero — folding skipped";
                diag_.report(d);
                return bin; // fold etme
            }

            int result = computeOp(bin->Operator, lv, rv);
            LiteralNode* lit = makeFoldedLit(result, bin->loc, bin->resolvedType);

            delete bin;
            changed_ = true;
            return lit;
        }

        case ASTKind::Postfix: {
            auto* pf = static_cast<PostfixNode*>(node);
            ASTNode* newOp = fold(pf->operand);
            if (newOp != pf->operand) {
                pf->operand = newOp;
                if (newOp)
                    newOp->parent = pf;
            }
            return pf;
        }

        case ASTKind::Call: {
            auto* call = static_cast<CallExpressionNode*>(node);
            if (call->callee) {
                ASTNode* nc = fold(call->callee);
                if (nc != call->callee) {
                    call->callee = nc;
                    if (nc)
                        nc->parent = call;
                }
            }
            for (auto*& arg : call->arguments) {
                ASTNode* na = fold(arg);
                if (na != arg) {
                    arg = na;
                    if (na)
                        na->parent = call;
                }
            }
            return call;
        }

        case ASTKind::MemberAccess: {
            auto* ma = static_cast<MemberAccessNode*>(node);
            ASTNode* no = fold(ma->object);
            if (no != ma->object) {
                ma->object = no;
                if (no)
                    no->parent = ma;
            }
            return ma;
        }

        case ASTKind::IndexExpression: {
            auto* ix = static_cast<IndexExpressionNode*>(node);
            ASTNode* no = fold(ix->object);
            if (no != ix->object) {
                ix->object = no;
                if (no)
                    no->parent = ix;
            }
            ASTNode* ni = fold(ix->index);
            if (ni != ix->index) {
                ix->index = ni;
                if (ni)
                    ni->parent = ix;
            }
            return ix;
        }

        // Literal ve Identifier değişmez
        case ASTKind::Literal:
        case ASTKind::Identifier:
            return node;

        // ── Deyimler (çocuklara iner) ────────────────────────────────────────
        case ASTKind::ExpressionStatement: {
            auto* es = static_cast<ExpressionStatementNode*>(node);
            if (es->expression) {
                ASTNode* ne = fold(es->expression);
                if (ne != es->expression) {
                    es->expression = ne;
                    if (ne)
                        ne->parent = es;
                }
            }
            return es;
        }

        case ASTKind::ReturnStatement: {
            auto* rs = static_cast<ReturnStatementNode*>(node);
            if (rs->value) {
                ASTNode* nv = fold(rs->value);
                if (nv != rs->value) {
                    rs->value = nv;
                    if (nv)
                        nv->parent = rs;
                }
            }
            return rs;
        }

        case ASTKind::VariableDecl: {
            auto* vd = static_cast<VariableDeclNode*>(node);
            if (vd->initExpr) {
                ASTNode* ni = fold(vd->initExpr);
                if (ni != vd->initExpr) {
                    vd->initExpr = ni;
                    if (ni)
                        ni->parent = vd;
                }
            }
            for (auto*& ch : vd->getChildren()) {
                ASTNode* nc = fold(ch);
                if (nc != ch) {
                    ch = nc;
                    if (nc)
                        nc->parent = vd;
                }
            }
            return vd;
        }

        case ASTKind::IfStatement: {
            auto* ifs = static_cast<IfStatementNode*>(node);
            if (ifs->condition) {
                ASTNode* nc = fold(ifs->condition);
                if (nc != ifs->condition) {
                    ifs->condition = nc;
                    if (nc)
                        nc->parent = ifs;
                }
            }
            if (ifs->thenBranch)
                fold(ifs->thenBranch);
            if (ifs->elseBranch)
                fold(ifs->elseBranch);
            return ifs;
        }

        case ASTKind::WhileStatement: {
            auto* ws = static_cast<WhileStatementNode*>(node);
            if (ws->condition) {
                ASTNode* nc = fold(ws->condition);
                if (nc != ws->condition) {
                    ws->condition = nc;
                    if (nc)
                        nc->parent = ws;
                }
            }
            if (ws->body)
                fold(ws->body);
            return ws;
        }

        case ASTKind::ForStatement: {
            auto* fs = static_cast<ForStatementNode*>(node);
            if (fs->init) {
                ASTNode* n = fold(fs->init);
                if (n != fs->init) {
                    fs->init = n;
                    if (n)
                        n->parent = fs;
                }
            }
            if (fs->condition) {
                ASTNode* n = fold(fs->condition);
                if (n != fs->condition) {
                    fs->condition = n;
                    if (n)
                        n->parent = fs;
                }
            }
            if (fs->update) {
                ASTNode* n = fold(fs->update);
                if (n != fs->update) {
                    fs->update = n;
                    if (n)
                        n->parent = fs;
                }
            }
            if (fs->body)
                fold(fs->body);
            return fs;
        }

        case ASTKind::DoWhileStatement: {
            auto* dw = static_cast<DoWhileStatementNode*>(node);
            if (dw->body)
                fold(dw->body);
            if (dw->condition) {
                ASTNode* nc = fold(dw->condition);
                if (nc != dw->condition) {
                    dw->condition = nc;
                    if (nc)
                        nc->parent = dw;
                }
            }
            return dw;
        }

        // ── Blok ve üst düzey ────────────────────────────────────────────────
        case ASTKind::Block: {
            auto* blk = static_cast<BlockNode*>(node);
            for (auto*& ch : blk->getChildren()) {
                ASTNode* nc = fold(ch);
                if (nc != ch) {
                    ch = nc;
                    if (nc)
                        nc->parent = blk;
                }
            }
            return blk;
        }

        case ASTKind::FunctionDecl: {
            auto* fn = static_cast<FunctionDeclNode*>(node);
            for (auto*& ch : fn->getChildren()) {
                ASTNode* nc = fold(ch);
                if (nc != ch) {
                    ch = nc;
                    if (nc)
                        nc->parent = fn;
                }
            }
            return fn;
        }

        case ASTKind::Program: {
            auto* prog = static_cast<ProgramNode*>(node);
            for (auto*& ch : prog->getChildren()) {
                ASTNode* nc = fold(ch);
                if (nc != ch) {
                    ch = nc;
                    if (nc)
                        nc->parent = prog;
                }
            }
            return prog;
        }

        default:
            // Bilinmeyen veya işlenmeyen node — olduğu gibi bırak
            for (auto*& ch : node->getChildren()) {
                ASTNode* nc = fold(ch);
                if (nc != ch) {
                    ch = nc;
                    if (nc)
                        nc->parent = node;
                }
            }
            return node;
        }
    }
};

#endif // SAQUT_OPT_CONSTANT_FOLDING
