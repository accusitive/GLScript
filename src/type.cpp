#include "glscript/type.hpp"

#include <sstream>
#include <utility>

namespace glscript {
namespace {

const char* builtin_name(BuiltinType type) {
    switch (type) {
    case BuiltinType::Unit:
        return "unit";
    case BuiltinType::Bool:
        return "bool";
    case BuiltinType::Int:
        return "int";
    case BuiltinType::Float:
        return "float";
    case BuiltinType::String:
        return "String";
    case BuiltinType::Dynamic:
        return "dynamic";
    case BuiltinType::Never:
        return "never";
    case BuiltinType::Error:
        return "<error>";
    }
    return "<error>";
}

} // namespace

Type::Type(Kind kind_value, BuiltinType builtin_value, std::string name_value,
           std::vector<TypePtr> arguments_value, TypePtr result_value)
    : kind(kind_value),
      builtin(builtin_value),
      name(std::move(name_value)),
      arguments(std::move(arguments_value)),
      result(std::move(result_value)) {}

std::string Type::to_string() const {
    switch (kind) {
    case Kind::Builtin:
        return builtin_name(builtin);
    case Kind::Named: {
        std::string output = name;
        if (!arguments.empty()) {
            output += '<';
            for (std::size_t i = 0; i < arguments.size(); ++i) {
                if (i > 0) {
                    output += ", ";
                }
                output += arguments[i] != nullptr ? arguments[i]->to_string() : "<null>";
            }
            output += '>';
        }
        return output;
    }
    case Kind::TypeParameter:
        return name;
    case Kind::Function: {
        std::string output = "(";
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            if (i > 0) {
                output += ", ";
            }
            output += arguments[i] != nullptr ? arguments[i]->to_string() : "<null>";
        }
        output += ") -> ";
        output += result != nullptr ? result->to_string() : "unit";
        return output;
    }
    }
    return "<error>";
}

TypePtr make_builtin_type(BuiltinType type) {
    return std::make_shared<Type>(Type::Kind::Builtin, type, std::string(), std::vector<TypePtr>(),
                                  nullptr);
}

TypePtr make_named_type(std::string name, std::vector<TypePtr> arguments) {
    return std::make_shared<Type>(Type::Kind::Named, BuiltinType::Error, std::move(name),
                                  std::move(arguments), nullptr);
}

TypePtr make_type_parameter(std::string name) {
    return std::make_shared<Type>(Type::Kind::TypeParameter, BuiltinType::Error, std::move(name),
                                  std::vector<TypePtr>(), nullptr);
}

TypePtr make_function_type(std::vector<TypePtr> parameters, TypePtr result) {
    return std::make_shared<Type>(Type::Kind::Function, BuiltinType::Error, std::string(),
                                  std::move(parameters), std::move(result));
}

bool types_equal(const TypePtr& left, const TypePtr& right) {
    if (left == right) {
        return true;
    }
    if (left == nullptr || right == nullptr) {
        return false;
    }
    if (left->kind != right->kind) {
        return false;
    }

    switch (left->kind) {
    case Type::Kind::Builtin:
        return left->builtin == right->builtin;
    case Type::Kind::TypeParameter:
        return left->name == right->name;
    case Type::Kind::Named:
        if (left->name != right->name || left->arguments.size() != right->arguments.size()) {
            return false;
        }
        for (std::size_t i = 0; i < left->arguments.size(); ++i) {
            if (!types_equal(left->arguments[i], right->arguments[i])) {
                return false;
            }
        }
        return true;
    case Type::Kind::Function:
        if (left->arguments.size() != right->arguments.size()) {
            return false;
        }
        for (std::size_t i = 0; i < left->arguments.size(); ++i) {
            if (!types_equal(left->arguments[i], right->arguments[i])) {
                return false;
            }
        }
        return types_equal(left->result, right->result);
    }

    return false;
}

bool is_error_type(const TypePtr& type) {
    return type != nullptr && type->is_error();
}

bool is_dynamic_type(const TypePtr& type) {
    return type != nullptr && type->is_dynamic();
}

namespace types {

TypePtr unit() {
    return make_builtin_type(BuiltinType::Unit);
}

TypePtr boolean() {
    return make_builtin_type(BuiltinType::Bool);
}

TypePtr integer() {
    return make_builtin_type(BuiltinType::Int);
}

TypePtr floating() {
    return make_builtin_type(BuiltinType::Float);
}

TypePtr string() {
    return make_builtin_type(BuiltinType::String);
}

TypePtr dynamic() {
    return make_builtin_type(BuiltinType::Dynamic);
}

TypePtr never() {
    return make_builtin_type(BuiltinType::Never);
}

TypePtr error() {
    return make_builtin_type(BuiltinType::Error);
}

} // namespace types

} // namespace glscript
