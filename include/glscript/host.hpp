#pragma once

#include "glscript/type.hpp"
#include "glscript/value.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace glscript {

// A host-side type reference. Named references are resolved relative to the
// module that owns the declaration, unless they contain an explicit "::".
struct TypeRef {
    enum class Kind {
        Builtin,
        Named,
        TypeVariable,
        Error,
    };

    Kind kind = Kind::Error;
    BuiltinType builtin = BuiltinType::Error;
    std::string name;
    std::vector<TypeRef> arguments;

    static TypeRef builtin_type(BuiltinType type);
    static TypeRef named(std::string name, std::vector<TypeRef> arguments = {});
    static TypeRef type_variable(std::string name);
    static TypeRef error_type();

    static TypeRef unit() { return builtin_type(BuiltinType::Unit); }
    static TypeRef boolean() { return builtin_type(BuiltinType::Bool); }
    static TypeRef integer() { return builtin_type(BuiltinType::Int); }
    static TypeRef floating() { return builtin_type(BuiltinType::Float); }
    static TypeRef string() { return builtin_type(BuiltinType::String); }
    static TypeRef dynamic() { return builtin_type(BuiltinType::Dynamic); }
    static TypeRef never() { return builtin_type(BuiltinType::Never); }
};

enum class HostVisibility {
    Local,
    Export,
    Pub,
    PubExport,
};

struct ParameterSchema {
    std::string name;
    TypeRef type;
};

struct FunctionSchema {
    std::string name;
    std::vector<std::string> type_parameters;
    std::vector<ParameterSchema> parameters;
    TypeRef return_type = TypeRef::unit();
    bool is_static = false;
    bool aborting = false;
    HostVisibility visibility = HostVisibility::Pub;
    std::vector<std::string> attributes;
    HostCall call; // empty when the host has no implementation
};

struct FieldSchema {
    std::string name;
    TypeRef type;
    HostVisibility visibility = HostVisibility::Pub;
};

struct OperatorSchema {
    std::string op;
    std::vector<TypeRef> operands;
    TypeRef result;
    HostCall call; // empty when the host has no implementation
};

struct EnumVariantSchema {
    std::string name;
    std::vector<TypeRef> payload;
};

struct TypeSchema {
    enum class Kind {
        Class,
        Enum,
        Effect,
    };

    std::string name;
    Kind kind = Kind::Class;
    std::vector<std::string> type_parameters;
    std::optional<TypeRef> base;
    bool global = false;
    HostVisibility visibility = HostVisibility::Pub;
    std::vector<FieldSchema> fields;
    std::vector<std::unique_ptr<FunctionSchema>> methods;
    std::vector<OperatorSchema> operators;
    std::vector<EnumVariantSchema> variants;
};

struct ModuleSchema {
    std::string name;
    std::vector<std::unique_ptr<TypeSchema>> types;
    std::vector<std::unique_ptr<FunctionSchema>> functions;
};

class FunctionBuilder {
public:
    explicit FunctionBuilder(FunctionSchema* schema) : schema_(schema) {}

    FunctionBuilder& type_parameter(std::string name) {
        schema_->type_parameters.push_back(std::move(name));
        return *this;
    }

    FunctionBuilder& parameter(std::string name, TypeRef type) {
        schema_->parameters.push_back(ParameterSchema{std::move(name), std::move(type)});
        return *this;
    }

    FunctionBuilder& returns(TypeRef type) {
        schema_->return_type = std::move(type);
        return *this;
    }

    FunctionBuilder& is_static(bool value = true) {
        schema_->is_static = value;
        return *this;
    }

    FunctionBuilder& aborting(bool value = true) {
        schema_->aborting = value;
        return *this;
    }

    FunctionBuilder& visibility(HostVisibility value) {
        schema_->visibility = value;
        return *this;
    }

    FunctionBuilder& attribute(std::string value) {
        schema_->attributes.push_back(std::move(value));
        return *this;
    }

    FunctionBuilder& call(HostCall implementation) {
        schema_->call = std::move(implementation);
        return *this;
    }

private:
    FunctionSchema* schema_;
};

class OperatorBuilder {
public:
    explicit OperatorBuilder(OperatorSchema* schema) : schema_(schema) {}

    OperatorBuilder& call(HostCall implementation) {
        schema_->call = std::move(implementation);
        return *this;
    }

private:
    OperatorSchema* schema_;
};

class TypeBuilder {
public:
    explicit TypeBuilder(TypeSchema* schema) : schema_(schema) {}

    TypeBuilder& type_parameter(std::string name) {
        schema_->type_parameters.push_back(std::move(name));
        return *this;
    }

    TypeBuilder& extends(TypeRef base) {
        schema_->base = std::move(base);
        return *this;
    }

    TypeBuilder& global(bool value = true) {
        schema_->global = value;
        return *this;
    }

    TypeBuilder& visibility(HostVisibility value) {
        schema_->visibility = value;
        return *this;
    }

    TypeBuilder& field(std::string name, TypeRef type,
                       HostVisibility visibility = HostVisibility::Pub) {
        schema_->fields.push_back(FieldSchema{std::move(name), std::move(type), visibility});
        return *this;
    }

    FunctionBuilder method(std::string name, TypeRef result = TypeRef::unit()) {
        auto schema = std::make_unique<FunctionSchema>();
        schema->name = std::move(name);
        schema->return_type = std::move(result);
        schema->is_static = false;
        schema_->methods.push_back(std::move(schema));
        return FunctionBuilder(schema_->methods.back().get());
    }

    FunctionBuilder static_method(std::string name, TypeRef result = TypeRef::unit()) {
        auto schema = std::make_unique<FunctionSchema>();
        schema->name = std::move(name);
        schema->return_type = std::move(result);
        schema->is_static = true;
        schema_->methods.push_back(std::move(schema));
        return FunctionBuilder(schema_->methods.back().get());
    }

    FunctionBuilder operation(std::string name, TypeRef result = TypeRef::unit()) {
        auto schema = std::make_unique<FunctionSchema>();
        schema->name = std::move(name);
        schema->return_type = std::move(result);
        schema->is_static = true;
        schema_->methods.push_back(std::move(schema));
        return FunctionBuilder(schema_->methods.back().get());
    }

    OperatorBuilder binary_operator(std::string op, TypeRef left, TypeRef right,
                                    TypeRef result) {
        schema_->operators.push_back(OperatorSchema{
            std::move(op), {std::move(left), std::move(right)}, std::move(result), {}});
        return OperatorBuilder(&schema_->operators.back());
    }

    OperatorBuilder unary_operator(std::string op, TypeRef operand, TypeRef result) {
        schema_->operators.push_back(
            OperatorSchema{std::move(op), {std::move(operand)}, std::move(result), {}});
        return OperatorBuilder(&schema_->operators.back());
    }

    TypeBuilder& variant(std::string name, std::vector<TypeRef> payload = {}) {
        schema_->variants.push_back(EnumVariantSchema{std::move(name), std::move(payload)});
        return *this;
    }

private:
    TypeSchema* schema_;
};

class ModuleBuilder {
public:
    explicit ModuleBuilder(ModuleSchema* schema) : schema_(schema) {}

    TypeBuilder type(std::string name, TypeSchema::Kind kind = TypeSchema::Kind::Class) {
        auto schema = std::make_unique<TypeSchema>();
        schema->name = std::move(name);
        schema->kind = kind;
        schema_->types.push_back(std::move(schema));
        return TypeBuilder(schema_->types.back().get());
    }

    TypeBuilder class_type(std::string name) {
        return type(std::move(name), TypeSchema::Kind::Class);
    }

    TypeBuilder enum_type(std::string name) {
        return type(std::move(name), TypeSchema::Kind::Enum);
    }

    TypeBuilder effect(std::string name) {
        return type(std::move(name), TypeSchema::Kind::Effect);
    }

    FunctionBuilder function(std::string name, TypeRef result = TypeRef::unit()) {
        auto schema = std::make_unique<FunctionSchema>();
        schema->name = std::move(name);
        schema->return_type = std::move(result);
        schema_->functions.push_back(std::move(schema));
        return FunctionBuilder(schema_->functions.back().get());
    }

    ModuleSchema* schema() const { return schema_; }

private:
    ModuleSchema* schema_;
};

class HostRegistry {
public:
    ModuleBuilder module(std::string name);

    const std::vector<std::unique_ptr<ModuleSchema>>& modules() const { return modules_; }
    const ModuleSchema* find_module(std::string_view name) const;

private:
    std::vector<std::unique_ptr<ModuleSchema>> modules_;
};

} // namespace glscript
