#pragma once

#include <memory>
#include <string>
#include <vector>

namespace glscript {

enum class BuiltinType {
    Unit,
    Bool,
    Int,
    Float,
    String,
    Dynamic,
    Never,
    Error,
};

class Type;
using TypePtr = std::shared_ptr<const Type>;

class Type {
public:
    enum class Kind {
        Builtin,
        Named,
        TypeParameter,
        Function,
    };

    Kind kind = Kind::Builtin;
    BuiltinType builtin = BuiltinType::Error;
    std::string name;
    std::vector<TypePtr> arguments;
    TypePtr result;

    Type() = default;
    Type(Kind kind, BuiltinType builtin, std::string name, std::vector<TypePtr> arguments,
         TypePtr result);

    bool is_builtin(BuiltinType type) const {
        return kind == Kind::Builtin && builtin == type;
    }
    bool is_error() const { return is_builtin(BuiltinType::Error); }
    bool is_dynamic() const { return is_builtin(BuiltinType::Dynamic); }
    bool is_unit() const { return is_builtin(BuiltinType::Unit); }
    bool is_named() const { return kind == Kind::Named; }

    std::string to_string() const;
};

TypePtr make_builtin_type(BuiltinType type);
TypePtr make_named_type(std::string name, std::vector<TypePtr> arguments = {});
TypePtr make_type_parameter(std::string name);
TypePtr make_function_type(std::vector<TypePtr> parameters, TypePtr result);

bool types_equal(const TypePtr& left, const TypePtr& right);
bool is_error_type(const TypePtr& type);
bool is_dynamic_type(const TypePtr& type);

namespace types {

TypePtr unit();
TypePtr boolean();
TypePtr integer();
TypePtr floating();
TypePtr string();
TypePtr dynamic();
TypePtr never();
TypePtr error();

} // namespace types

} // namespace glscript
