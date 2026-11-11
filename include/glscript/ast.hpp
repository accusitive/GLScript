#pragma once

#include "glscript/binding.hpp"
#include "glscript/source.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace glscript {

struct EnumVariantSymbol; // checker-owned variant symbol (symbols.hpp)

} // namespace glscript

namespace glscript::ast {

struct Node {
    SourceSpan span;

    virtual ~Node() = default;
};

enum class Visibility {
    Local,
    Export,
    Pub,
    PubExport,
};

struct Attribute {
    std::string name;
    SourceSpan span;
};

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

struct TypeExpr : Node {};
using TypeExprPtr = std::unique_ptr<TypeExpr>;

struct NamedTypeExpr : TypeExpr {
    std::vector<std::string> path;
    std::vector<TypeExprPtr> arguments;
};

struct DynamicTypeExpr : TypeExpr {};

struct ErrorTypeExpr : TypeExpr {};

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

// Every expression carries the binding the typechecker resolved for it. The
// checker decorates the tree through const references, hence `mutable`.
struct Expr : Node {
    mutable Binding binding;
};
using ExprPtr = std::unique_ptr<Expr>;

struct ErrorExpr : Expr {};

struct TemplatePart {
    bool expression = false;
    std::string text;
    ExprPtr expression_value;
};

struct IntLiteralExpr : Expr {
    std::string text;
};

struct FloatLiteralExpr : Expr {
    std::string text;
};

struct StringLiteralExpr : Expr {
    std::string value;
};

struct TemplateStringExpr : Expr {
    std::vector<TemplatePart> parts;
};

struct BoolLiteralExpr : Expr {
    bool value = false;
};

struct ArrayLiteralExpr : Expr {
    std::vector<ExprPtr> elements;
};

struct UnitLiteralExpr : Expr {};

struct WildcardExpr : Expr {};

struct IdentifierExpr : Expr {
    std::vector<std::string> path;
};

struct UnaryExpr : Expr {
    std::string op;
    ExprPtr operand;
};

struct BinaryExpr : Expr {
    std::string op;
    ExprPtr left;
    ExprPtr right;
};

struct DowncastExpr : Expr {
    TypeExprPtr target;
    ExprPtr operand;
};

struct CallExpr : Expr {
    ExprPtr callee;
    std::vector<TypeExprPtr> type_arguments;
    std::vector<ExprPtr> arguments;
};

struct MemberExpr : Expr {
    ExprPtr object;
    std::string name;
};

struct IndexExpr : Expr {
    ExprPtr object;
    ExprPtr index;
};

struct Parameter {
    std::string name;
    TypeExprPtr type;
    SourceSpan span;

    Parameter() = default;
    Parameter(Parameter&&) = default;
    Parameter& operator=(Parameter&&) = default;
    Parameter(const Parameter&) = delete;
    Parameter& operator=(const Parameter&) = delete;
};

struct LambdaExpr : Expr {
    std::vector<Parameter> parameters;
    TypeExprPtr return_type;
    StmtPtr body;
};

struct ResumeExpr : Expr {
    ExprPtr value;
    bool wildcard = false;
};

// ---------------------------------------------------------------------------
// Patterns (match arms, let destructuring)
// ---------------------------------------------------------------------------

struct Pattern : Node {};
using PatternPtr = std::unique_ptr<Pattern>;

struct ErrorPattern : Pattern {};

struct WildcardPattern : Pattern {};

// A bare name. The checker decides between a binding and a payload-less
// variant of the expected enum type (Rust's resolution rule); when it resolves
// to a variant, `variant` is set for the lowering pass.
struct BindingPattern : Pattern {
    std::string name;
    TypeExprPtr annotation;
    mutable EnumVariantSymbol* variant = nullptr;
};

struct LiteralPattern : Pattern {
    ExprPtr value; // literal expression, possibly negated
};

struct VariantPattern : Pattern {
    std::vector<std::string> path;
    std::vector<PatternPtr> payload;
    // Checker resolution, recorded for the lowering pass.
    mutable EnumVariantSymbol* variant = nullptr;
};

struct MatchArm {
    PatternPtr pattern;
    ExprPtr guard; // null when absent
    ExprPtr body;
    SourceSpan span;
};

struct MatchExpr : Expr {
    ExprPtr scrutinee;
    std::vector<MatchArm> arms;
};

// A statement block in expression position (`=> { ... }` match arm bodies).
// The checker treats it like a block whose value is its tail expression;
// lowering compiles it as a zero-argument closure and calls it immediately.
struct BlockExpr : Expr {
    StmtPtr body; // a BlockStmt
};

struct HandlerCase {
    std::string effect;
    std::string operation;
    std::vector<Parameter> parameters;
    TypeExprPtr result_type;
    StmtPtr body;
    SourceSpan span;

    HandlerCase() = default;
    HandlerCase(HandlerCase&&) = default;
    HandlerCase& operator=(HandlerCase&&) = default;
    HandlerCase(const HandlerCase&) = delete;
    HandlerCase& operator=(const HandlerCase&) = delete;
};

struct HandleExpr : Expr {
    ExprPtr body;
    std::vector<HandlerCase> cases;
};

struct IfExpr : Expr {
    ExprPtr condition;
    ExprPtr then_expr;
    ExprPtr else_expr;
};

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

struct Stmt : Node {};
using StmtPtr = std::unique_ptr<Stmt>;

struct BlockStmt : Stmt {
    std::vector<StmtPtr> statements;
};

struct EmptyStmt : Stmt {};

struct ExprStmt : Stmt {
    ExprPtr expression;
};

struct VarDeclStmt : Stmt {
    TypeExprPtr type;
    std::string name;
    // `let` destructuring form: a pattern whose bindings take the value.
    // Null for the legacy `type name` form.
    PatternPtr pattern;
    ExprPtr initializer;
};

struct AssignStmt : Stmt {
    ExprPtr target;
    ExprPtr value;
};

struct ReturnStmt : Stmt {
    ExprPtr value;
};

struct IfStmt : Stmt {
    ExprPtr condition;
    StmtPtr then_branch;
    StmtPtr else_branch;
};

struct WhileStmt : Stmt {
    ExprPtr condition;
    StmtPtr body;
};

struct BreakStmt : Stmt {};

struct ContinueStmt : Stmt {};

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

struct Decl : Node {
    Visibility visibility = Visibility::Local;
    std::vector<Attribute> attributes;

    bool exported_to_host() const {
        return visibility == Visibility::Export || visibility == Visibility::PubExport;
    }
    bool importable() const {
        return visibility == Visibility::Pub || visibility == Visibility::PubExport;
    }
};

struct ImportItem {
    std::string name;
    SourceSpan span;
};

struct ImportDecl : Decl {
    std::vector<std::string> module_path;
    bool wildcard = false;
    std::vector<ImportItem> items;
};

struct FieldDecl : Decl {
    TypeExprPtr type;
    std::string name;
    ExprPtr initializer;
};

struct FunctionDecl : Decl {
    std::string name;
    std::vector<std::string> type_parameters;
    std::vector<Parameter> parameters;
    TypeExprPtr return_type;
    StmtPtr body;
    bool is_effect_operation = false;
};

struct EnumVariantDecl {
    std::string name;
    std::vector<TypeExprPtr> payload;
    SourceSpan span;

    EnumVariantDecl() = default;
    EnumVariantDecl(EnumVariantDecl&&) = default;
    EnumVariantDecl& operator=(EnumVariantDecl&&) = default;
    EnumVariantDecl(const EnumVariantDecl&) = delete;
    EnumVariantDecl& operator=(const EnumVariantDecl&) = delete;
};

struct ClassDecl : Decl {
    std::string name;
    std::vector<std::string> type_parameters;
    std::optional<std::string> base;
    std::vector<std::unique_ptr<FieldDecl>> fields;
    std::vector<std::unique_ptr<FunctionDecl>> methods;
};

struct EnumDecl : Decl {
    std::string name;
    std::vector<std::string> type_parameters;
    std::vector<EnumVariantDecl> variants;
};

struct EffectDecl : Decl {
    std::string name;
    std::vector<std::unique_ptr<FunctionDecl>> operations;
};

struct HandlerDecl : Decl {
    std::string effect;
    ExprPtr handled_expression;
    std::vector<HandlerCase> cases;
};

using DeclPtr = std::unique_ptr<Decl>;

struct ModuleUnit {
    std::string name;
    std::string path;
    std::vector<DeclPtr> declarations;
};

} // namespace glscript::ast
