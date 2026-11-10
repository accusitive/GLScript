#pragma once

#include "glscript/ast.hpp"
#include "glscript/source.hpp"
#include "glscript/type.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace glscript {

// Semantic symbols produced by the checker. They outlive checking inside a
// Program so the bytecode compiler (and any other consumer) can walk the
// checked program: ASTs stay alive through ModuleInfo::unit, expression nodes
// carry their resolved Binding, and functions carry declaration back-pointers
// to their bodies.

struct TypeSymbol;
struct FunctionSymbol;

struct FieldSymbol {
    std::string name;
    TypePtr type;
    std::size_t index = 0;
    ast::Visibility visibility = ast::Visibility::Local;
    SourceSpan span;
};

struct EnumVariantSymbol {
    std::string name;
    std::vector<TypePtr> payload;
    TypePtr value_type;
    TypeSymbol* owner = nullptr;
    SourceSpan span;
};

struct OperatorInfo {
    std::string op;
    std::vector<TypePtr> operands;
    TypePtr result;
};

struct FunctionSymbol {
    std::string name;
    std::string qualified_name;
    std::vector<std::string> type_parameters;
    std::size_t owner_type_parameter_count = 0;
    std::vector<std::string> parameter_names;
    std::vector<TypePtr> parameter_types;
    TypePtr result = types::unit();
    ast::Visibility visibility = ast::Visibility::Local;
    bool is_static = true;
    bool aborting = false;
    bool is_effect_operation = false;
    bool is_host = false;
    TypeSymbol* owner = nullptr;
    SourceSpan span;
    ast::FunctionDecl* declaration = nullptr;
};

struct TypeSymbol {
    enum class Kind {
        Class,
        Enum,
        Effect,
    };

    Kind kind = Kind::Class;
    std::string name;
    std::string qualified_name;
    std::string module;
    std::vector<std::string> type_parameters;
    ast::Visibility visibility = ast::Visibility::Local;
    bool global = false;
    bool is_host = false;
    TypeSymbol* base = nullptr;
    std::vector<TypePtr> base_arguments;
    std::unordered_map<std::string, FieldSymbol> fields;
    std::unordered_map<std::string, FunctionSymbol*> methods;
    std::unordered_map<std::string, EnumVariantSymbol*> variants;
    std::vector<OperatorInfo> operators;
    std::vector<FunctionSymbol*> effect_operations;
    SourceSpan span;
    ast::ClassDecl* class_declaration = nullptr;
    ast::EnumDecl* enum_declaration = nullptr;
    ast::EffectDecl* effect_declaration = nullptr;
};

struct Symbol {
    enum class Kind {
        Variable,
        Function,
        EnumVariant,
    };

    Kind kind = Kind::Variable;
    TypePtr type;
    TypePtr receiver_type;
    FunctionSymbol* function = nullptr;
    FieldSymbol* field = nullptr;
    EnumVariantSymbol* variant = nullptr;
};

struct Scope {
    Scope* parent = nullptr;
    std::unordered_map<std::string, Symbol> values;
    std::unordered_map<std::string, TypeSymbol*> types;
};

struct ModuleInfo {
    std::string name;
    std::string path;
    std::unique_ptr<ast::ModuleUnit> unit;
    bool is_host = false;
    bool loading = false;
    bool signatures_resolved = false;
    bool bodies_checked = false;
    Scope* scope = nullptr;
    std::vector<std::unique_ptr<TypeSymbol>> owned_types;
    std::vector<std::unique_ptr<FunctionSymbol>> owned_functions;
    std::vector<std::unique_ptr<EnumVariantSymbol>> owned_variants;
    std::unordered_map<std::string, TypeSymbol*> types;
    std::unordered_map<std::string, FunctionSymbol*> functions;
};

// The checked program: everything the front end produced, retained after
// check_source() returns. The bytecode compiler is the primary consumer.
struct Program {
    std::unordered_map<std::string, std::unique_ptr<ModuleInfo>> modules;
    std::vector<std::unique_ptr<SourceFile>> sources;
    std::vector<std::unique_ptr<Scope>> scopes;
    std::vector<std::unique_ptr<FunctionSymbol>> builtin_functions;
    std::unordered_map<std::string, TypeSymbol*> types_by_qname;

    TypeSymbol* find_type(const std::string& qualified_name) const {
        const auto it = types_by_qname.find(qualified_name);
        return it != types_by_qname.end() ? it->second : nullptr;
    }

    ModuleInfo* find_module(const std::string& name) const {
        const auto it = modules.find(name);
        return it != modules.end() ? it->second.get() : nullptr;
    }
};

} // namespace glscript
