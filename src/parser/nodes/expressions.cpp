// ============================================================================
// saQut Compiler — Genişletilmiş İfade Düğümleri Gerçeklemesi
// ============================================================================

#include "parser/nodes/expressions.hpp"
#include "parser/ast_json.hpp"

// ScopeCallNode — built-in metod çağrısı: E::method(args) / recv.method(args)
ScopeCallNode::ScopeCallNode() { kind = ASTKind::ScopeCall; }
void ScopeCallNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "ScopeCall" << Color::Reset
              << " " << Color::SoftPembe << (dotCall ? "<recv>" : leftTypeName) << Color::Reset
              << Color::SoftGri << (dotCall ? "." : "::") << Color::Reset
              << Color::SoftYesil << methodName << Color::Reset << "\n";
    for (auto* arg : arguments) arg->log(indent + 1);
}
std::string ScopeCallNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind",       "ScopeCall");
    obj.add("leftType",   leftTypeName);
    obj.add("method",     methodName);
    obj.add("builtinId",  builtinId);
    if (dotCall) obj.add("dotCall", true); // ADR-033: UFCS — eski AST JSON'ları değişmez
    obj.addArray("arguments", [&]() {
        for (auto* arg : arguments) obj.addItem(arg->toJson(depth + 2));
    });
    obj.addRaw("resolvedType", resolvedTypeJson());
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// PostfixNode
PostfixNode::PostfixNode() { kind = ASTKind::Postfix; }
void PostfixNode::log(int indent) {
    // #237: önek ve sonek AYNI düğümdür ama farklı değer döndürür; dışarıya
    // ayırt edilebilir görünmeli (cam kutu ilkesi).
    std::cout << jsonIndent(indent) << Color::SoftMavi
              << (isPrefix ? "Prefix" : "Postfix") << Color::Reset
              << " (" << Color::SoftMor
              << (OPERATOR_MAP_REV.count(Operator) ? OPERATOR_MAP_REV.at(Operator) : "?")
              << Color::Reset << ")\n";
    if (operand) operand->log(indent + 1);
}
std::string PostfixNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "Postfix");
    obj.add("operator", std::string(OPERATOR_MAP_REV.count(Operator) ? OPERATOR_MAP_REV.at(Operator) : "?"));
    obj.add("fix", std::string(isPrefix ? "prefix" : "postfix"));   // #237
    if (operand) obj.addRaw("operand", operand->toJson(depth + 1));
    obj.addRaw("resolvedType", resolvedTypeJson());
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// CallExpressionNode
CallExpressionNode::CallExpressionNode() { kind = ASTKind::Call; }
void CallExpressionNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "Call" << Color::Reset << "\n";
    if (callee) callee->log(indent + 1);
    for (auto* arg : arguments) arg->log(indent + 1);
}
std::string CallExpressionNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "Call");
    if (callee) obj.addRaw("callee", callee->toJson(depth + 1));
    obj.addArray("arguments", [&]() {
        for (auto* arg : arguments) obj.addItem(arg->toJson(depth + 2));
    });
    obj.addRaw("resolvedType", resolvedTypeJson());
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// MemberAccessNode
MemberAccessNode::MemberAccessNode() { kind = ASTKind::MemberAccess; }
void MemberAccessNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "MemberAccess" << Color::Reset
              << " (" << Color::SoftGri << (arrow ? "->" : ".") << Color::Reset
              << Color::SoftYesil << member << Color::Reset << ")\n";
    if (object) object->log(indent + 1);
}
std::string MemberAccessNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "MemberAccess");
    obj.add("member", member);
    obj.add("arrow", arrow);
    if (object) obj.addRaw("object", object->toJson(depth + 1));
    obj.addRaw("resolvedType", resolvedTypeJson());
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// ArrayLiteralNode
ArrayLiteralNode::ArrayLiteralNode() { kind = ASTKind::ArrayLiteral; }
void ArrayLiteralNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "ArrayLiteral" << Color::Reset
              << " [" << Color::SoftTuruncu << elements.size() << Color::Reset
              << Color::SoftGri << " eleman" << Color::Reset << "]\n";
    for (auto* e : elements) e->log(indent + 1);
}
std::string ArrayLiteralNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "ArrayLiteral");
    obj.addArray("elements", [&]() {
        for (auto* e : elements) obj.addItem(e->toJson(depth + 2));
    });
    obj.addRaw("resolvedType", resolvedTypeJson());
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// CastExpressionNode (ADR-026)
CastExpressionNode::CastExpressionNode() { kind = ASTKind::CastExpression; }
void CastExpressionNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "CastExpression" << Color::Reset
              << " " << Color::SoftGri << "as" << Color::Reset << " "
              << Color::SoftPembe << targetTypeName << Color::Reset
              << (targetNullable ? std::string(Color::SoftTuruncu) + "?" + Color::Reset : "") << "\n";
    if (operand) operand->log(indent + 1);
}
std::string CastExpressionNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "CastExpression");
    obj.add("targetType", targetTypeName + (targetNullable ? "?" : ""));
    if (operand) obj.addRaw("operand", operand->toJson(depth + 1));
    obj.addRaw("resolvedType", resolvedTypeJson());
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// IndexExpressionNode
IndexExpressionNode::IndexExpressionNode() { kind = ASTKind::IndexExpression; }
void IndexExpressionNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "IndexExpression" << Color::Reset << "\n";
    if (object) object->log(indent + 1);
    if (index) index->log(indent + 1);
}
std::string IndexExpressionNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "IndexExpression");
    if (object) obj.addRaw("object", object->toJson(depth + 1));
    if (index) obj.addRaw("index", index->toJson(depth + 1));
    obj.addRaw("resolvedType", resolvedTypeJson());
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// ThreadExprNode (ADR-045)
ThreadExprNode::ThreadExprNode() { kind = ASTKind::ThreadExpr; }
void ThreadExprNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << "ThreadExpr" << Color::Reset << "\n";
    if (body) body->log(indent + 1);
}
std::string ThreadExprNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", "ThreadExpr");
    obj.addArray("captures", [&]() {
        for (auto& c : captures) obj.addItem("\"" + c + "\"");
    });
    if (body) obj.addRaw("body", body->toJson(depth + 1));
    obj.addRaw("resolvedType", resolvedTypeJson());
    obj.addRaw("location", loc.toJson());
    return obj.str();
}

// CollectionNewNode (ADR-045)
CollectionNewNode::CollectionNewNode() { kind = ASTKind::CollectionNew; }
void CollectionNewNode::log(int indent) {
    std::cout << jsonIndent(indent) << Color::SoftMavi << (isPool ? "Pool" : "List")
              << Color::Reset << "(" << Color::SoftPembe << elemTypeName << Color::Reset << ")\n";
}
std::string CollectionNewNode::toJson(int depth) {
    JsonObject obj(depth);
    obj.add("kind", isPool ? "PoolNew" : "ListNew");
    obj.add("elementType", elemTypeName);
    obj.addRaw("resolvedType", resolvedTypeJson());
    obj.addRaw("location", loc.toJson());
    return obj.str();
}
