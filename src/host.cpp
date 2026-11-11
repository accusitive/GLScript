#include "glscript/host.hpp"

#include "glscript/runtime.hpp"

#include <iostream>
#include <utility>

namespace glscript {

TypeRef TypeRef::builtin_type(BuiltinType type) {
    TypeRef reference;
    reference.kind = Kind::Builtin;
    reference.builtin = type;
    return reference;
}

TypeRef TypeRef::named(std::string name, std::vector<TypeRef> arguments) {
    TypeRef reference;
    reference.kind = Kind::Named;
    reference.name = std::move(name);
    reference.arguments = std::move(arguments);
    return reference;
}

TypeRef TypeRef::type_variable(std::string name) {
    TypeRef reference;
    reference.kind = Kind::TypeVariable;
    reference.name = std::move(name);
    return reference;
}

TypeRef TypeRef::error_type() {
    TypeRef reference;
    reference.kind = Kind::Error;
    return reference;
}

ModuleBuilder HostRegistry::module(std::string name) {
    for (auto& module : modules_) {
        if (module->name == name) {
            return ModuleBuilder(module.get());
        }
    }

    auto schema = std::make_unique<ModuleSchema>();
    schema->name = std::move(name);
    modules_.push_back(std::move(schema));
    return ModuleBuilder(modules_.back().get());
}

const ModuleSchema* HostRegistry::find_module(std::string_view name) const {
    for (const auto& module : modules_) {
        if (module->name == name) {
            return module.get();
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// std module: array<T>, Option<T>, and the OutOfBounds effect
//
// Types in this module get bare qualified names (special-cased in the
// checker) so scripts write `array<int>`, `Option::None`, and
// `handle OutOfBounds::index(...)`. Everything is `global` — no import
// needed. Embedders call register_std() alongside their own bindings.
// ---------------------------------------------------------------------------

void register_std(HostRegistry& host, PrintSink print) {
    if (print == nullptr) {
        print = [](const std::string& message) { std::cout << message << "\n"; };
    }
    auto std_module = host.module("std");

    auto array_type = std_module.class_type("array").type_parameter("T").global(true);
    array_type.method("len")
        .returns(TypeRef::integer())
        .call([](Runtime& runtime, std::vector<Value>& args) {
            (void)runtime;
            if (!args.empty() && args[0].is_heap() &&
                args[0].as_heap()->kind == GcObject::Kind::Array) {
                return Value::integer(static_cast<std::int64_t>(as_array(args[0])->elements.size()));
            }
            return Value::unit();
        });
    array_type.method("push")
        .parameter("value", TypeRef::type_variable("T"))
        .call([](Runtime& runtime, std::vector<Value>& args) {
            (void)runtime;
            if (args.size() == 2 && args[0].is_heap() &&
                args[0].as_heap()->kind == GcObject::Kind::Array) {
                as_array(args[0])->elements.push_back(args[1]);
            }
            return Value::unit();
        });

    std_module.enum_type("Option")
        .type_parameter("T")
        .global(true)
        .variant("Some", {TypeRef::type_variable("T")})
        .variant("None");

    // Unhandled out-of-bounds accesses fall back to this thunk: print the
    // details and abort (mirrors `invalid_downcast`).
    std_module.effect("OutOfBounds")
        .global(true)
        .operation("index", TypeRef::dynamic())
        .parameter("size", TypeRef::integer())
        .parameter("index", TypeRef::integer())
        .aborting()
        .call([print](Runtime& runtime, std::vector<Value>& args) {
            (void)runtime;
            if (args.size() == 2) {
                print("array index out of bounds: index " + runtime.stringify(args[1]) +
                      " (size " + runtime.stringify(args[0]) + ")");
            }
            return Value::unit();
        });
}

} // namespace glscript
