#pragma once

#include "glscript/type.hpp"

namespace glscript {

struct TypeSymbol;
struct FunctionSymbol;
struct FieldSymbol;
struct EnumVariantSymbol;
struct OperatorInfo;

// What the typechecker resolved a given AST expression to. Bindings are
// recorded onto expression nodes during checking and are what the bytecode
// compiler consumes to translate names into symbols without re-resolving
// scopes.
enum class BindingKind : unsigned char {
    None,         // unresolved (error recovery) or not meaningful
    Variable,     // a local, parameter, or imported value; `field` is set when
                  // the name refers to an instance field via the receiver
    Function,     // a callable: script function, host function, effect
                  // operation, or enum constructor; `receiver_type` is set
                  // when the symbol carries an implicit receiver
    EnumVariant,  // a payload-less enum variant used as a value
    Field,        // a member field access (`expr.field`)
    StaticMethod, // a member expression naming a static method as a value
    Intrinsic,    // an intrinsic binary/unary operation (numeric, string,
                  // comparison, logical)
    Operator,     // an overloaded operator; `op` is the resolved overload
    Downcast,     // `downcast::<T>`; `type` is the target type
    ArrayLiteral, // `[a, b, c]`; `type` is `array<T>`/`array<dynamic>`
    Index,        // `arr[i]` on an `array<T>`; `type` is the element type
};

struct Binding {
    BindingKind kind = BindingKind::None;
    TypePtr type;
    FunctionSymbol* function = nullptr;
    TypeSymbol* owner = nullptr;
    FieldSymbol* field = nullptr;
    EnumVariantSymbol* variant = nullptr;
    OperatorInfo* op = nullptr;
    TypePtr receiver_type;
};

} // namespace glscript
