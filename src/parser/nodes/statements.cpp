// ============================================================================
// saQut Compiler — İfade/Deyim Düğümleri Gerçeklemesi
// ============================================================================

#include "parser/nodes/statements.hpp"
#include "parser/ast_json.hpp"

// BlockNode
BlockNode::BlockNode() { kind = ASTKind::Block; }
void BlockNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "Block" << Color::Reset << "\n";
    for (auto* child : children) child->log(indent + 1);
}
std::string BlockNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "Block");
    obj.addArray("children", [&]() {
        for (auto* child : children) obj.addItem(child->toJson(depth + 2));
    });
    obj.add("isReachable", isReachable);
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// IfStatementNode
IfStatementNode::IfStatementNode() { kind = ASTKind::IfStatement; }
void IfStatementNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "IfStatement" << Color::Reset << "\n";
    if (condition) condition->log(indent + 1);
    if (thenBranch) thenBranch->log(indent + 1);
    if (elseBranch) elseBranch->log(indent + 1);
}
std::string IfStatementNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "IfStatement");
    if (condition) obj.addRaw("condition", condition->toJson(depth + 1));
    if (thenBranch) obj.addRaw("then", thenBranch->toJson(depth + 1));
    if (elseBranch) obj.addRaw("else", elseBranch->toJson(depth + 1));
    obj.add("isReachable", isReachable);
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// WhileStatementNode
WhileStatementNode::WhileStatementNode() { kind = ASTKind::WhileStatement; }
void WhileStatementNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "WhileStatement" << Color::Reset << "\n";
    if (condition) condition->log(indent + 1);
    if (body) body->log(indent + 1);
}
std::string WhileStatementNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "WhileStatement");
    if (condition) obj.addRaw("condition", condition->toJson(depth + 1));
    if (body) obj.addRaw("body", body->toJson(depth + 1));
    obj.add("isReachable", isReachable);
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// ForStatementNode
ForStatementNode::ForStatementNode() { kind = ASTKind::ForStatement; }
void ForStatementNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "ForStatement" << Color::Reset << "\n";
    if (init) init->log(indent + 1);
    if (condition) condition->log(indent + 1);
    if (update) update->log(indent + 1);
    if (body) body->log(indent + 1);
}
std::string ForStatementNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "ForStatement");
    if (init) obj.addRaw("init", init->toJson(depth + 1));
    if (condition) obj.addRaw("condition", condition->toJson(depth + 1));
    if (update) obj.addRaw("update", update->toJson(depth + 1));
    if (body) obj.addRaw("body", body->toJson(depth + 1));
    obj.add("isReachable", isReachable);
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// DoWhileStatementNode
DoWhileStatementNode::DoWhileStatementNode() { kind = ASTKind::DoWhileStatement; }
void DoWhileStatementNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "DoWhileStatement" << Color::Reset << "\n";
    if (body) body->log(indent + 1);
    if (condition) condition->log(indent + 1);
}
std::string DoWhileStatementNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "DoWhileStatement");
    if (condition) obj.addRaw("condition", condition->toJson(depth + 1));
    if (body) obj.addRaw("body", body->toJson(depth + 1));
    obj.add("isReachable", isReachable);
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// ReturnStatementNode
ReturnStatementNode::ReturnStatementNode() { kind = ASTKind::ReturnStatement; }
void ReturnStatementNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "ReturnStatement" << Color::Reset << "\n";
    if (value) value->log(indent + 1);
}
std::string ReturnStatementNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "ReturnStatement");
    if (value) obj.addRaw("value", value->toJson(depth + 1));
    obj.add("isReachable", isReachable);
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// BreakStatementNode
BreakStatementNode::BreakStatementNode() { kind = ASTKind::BreakStatement; }
void BreakStatementNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "BreakStatement" << Color::Reset << "\n";
}
std::string BreakStatementNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "BreakStatement");
    obj.add("isReachable", isReachable);
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// ContinueStatementNode
ContinueStatementNode::ContinueStatementNode() { kind = ASTKind::ContinueStatement; }
void ContinueStatementNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "ContinueStatement" << Color::Reset << "\n";
}
std::string ContinueStatementNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "ContinueStatement");
    obj.add("isReachable", isReachable);
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// ExpressionStatementNode
ExpressionStatementNode::ExpressionStatementNode() { kind = ASTKind::ExpressionStatement; }
void ExpressionStatementNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "ExpressionStatement" << Color::Reset << "\n";
    if (expression) expression->log(indent + 1);
}
std::string ExpressionStatementNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "ExpressionStatement");
    if (expression) obj.addRaw("expression", expression->toJson(depth + 1));
    obj.add("isReachable", isReachable);
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// TryStatementNode (ADR-025)
TryStatementNode::TryStatementNode() { kind = ASTKind::TryStatement; }
void TryStatementNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "TryStatement" << Color::Reset
              << " (" << Color::SoftGri << "catch" << Color::Reset << " "
              << Color::SoftYesil << catchVar << Color::Reset << ")\n";
    if (body)    body->log(indent + 1);
    if (handler) handler->log(indent + 1);
}
std::string TryStatementNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "TryStatement");
    obj.add("catchVar", catchVar);
    if (body)    obj.addRaw("body",    body->toJson(depth + 1));
    if (handler) obj.addRaw("handler", handler->toJson(depth + 1));
    obj.add("isReachable", isReachable);
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// ThrowStatementNode (ADR-025)
ThrowStatementNode::ThrowStatementNode() { kind = ASTKind::ThrowStatement; }
void ThrowStatementNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "ThrowStatement" << Color::Reset << "\n";
    if (value) value->log(indent + 1);
}
std::string ThrowStatementNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "ThrowStatement");
    if (value) obj.addRaw("value", value->toJson(depth + 1));
    obj.add("isReachable", isReachable);
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// SwitchStatementNode (ADR-027)
SwitchStatementNode::SwitchStatementNode() { kind = ASTKind::SwitchStatement; }
void SwitchStatementNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "SwitchStatement" << Color::Reset << "\n";
    if (subject) subject->log(indent + 1);
    for (auto& c : cases) {
        if (c.isDefault)
            std::cout << jsonIndent(indent + 1) << Color::SoftMor << "default" << Color::Reset << ":\n";
        else {
            std::cout << jsonIndent(indent + 1) << Color::SoftMor << "case" << Color::Reset << " ";
            for (size_t i = 0; i < c.values.size(); i++) {
                if (i) std::cout << Color::SoftGri << ", " << Color::Reset;
                if (c.values[i]) c.values[i]->log(0);
            }
            std::cout << "\n";
        }
        for (auto* s : c.body) s->log(indent + 2);
    }
}
std::string SwitchStatementNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "SwitchStatement");
    if (subject) obj.addRaw("subject", subject->toJson(depth + 1));
    std::string casesJson = "[\n";
    std::string ind = jsonIndent(depth + 1);
    for (size_t ci = 0; ci < cases.size(); ci++) {
        auto& c = cases[ci];
        casesJson += ind + "{\n";
        casesJson += ind + "  \"isDefault\": " + (c.isDefault ? "true" : "false") + ",\n";
        casesJson += ind + "  \"values\": [";
        for (size_t i = 0; i < c.values.size(); i++) {
            if (i) casesJson += ", ";
            casesJson += c.values[i] ? c.values[i]->toJson(depth + 2) : "null";
        }
        casesJson += "],\n";
        casesJson += ind + "  \"body\": [";
        for (size_t i = 0; i < c.body.size(); i++) {
            if (i) casesJson += ", ";
            casesJson += c.body[i]->toJson(depth + 2);
        }
        casesJson += "]\n";
        casesJson += ind + "}";
        if (ci + 1 < cases.size()) casesJson += ",";
        casesJson += "\n";
    }
    casesJson += jsonIndent(depth) + "]";
    obj.addRaw("cases", casesJson);
    obj.add("isReachable", isReachable);
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// LockStatementNode (ADR-045)
LockStatementNode::LockStatementNode() { kind = ASTKind::LockStatement; }
void LockStatementNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi
              << (isUnlock ? "UnlockStatement" : "LockStatement") << Color::Reset << "\n";
    for (auto* t : targets) t->log(indent + 1);
}
std::string LockStatementNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", isUnlock ? "UnlockStatement" : "LockStatement");
    obj.addArray("targets", [&]() {
        for (auto* t : targets) obj.addItem(t->toJson(depth + 2));
    });
    obj.add("isReachable", isReachable);
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// WaitStatementNode (ADR-045)
WaitStatementNode::WaitStatementNode() { kind = ASTKind::WaitStatement; }
void WaitStatementNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "WaitStatement" << Color::Reset << "\n";
    if (condition) condition->log(indent + 1);
}
std::string WaitStatementNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "WaitStatement");
    if (condition) obj.addRaw("condition", condition->toJson(depth + 1));
    obj.add("isReachable", isReachable);
    obj.addRaw("location", loc.toJson());
    return obj.str();
}
