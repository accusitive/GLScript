#include "engine_bindings.hpp"

#include "glscript/runtime.hpp"
#include "glscript/symbols.hpp"

#include <iostream>

namespace glscript {

void register_engine_bindings(HostRegistry& host, PrintSink print) {
    if (print == nullptr) {
        print = [](const std::string& message) { std::cout << message << "\n"; };
    }

    // std: array<T>, Option<T>, OutOfBounds effect
    register_std(host);

    // input::Axis and input::Input
    {
        auto input = host.module("input");
        input.enum_type("Axis")
            .variant("Horizontal")
            .variant("Vertical");

        input.class_type("Input")
            .static_method("get_axis", TypeRef::floating())
            .parameter("axis", TypeRef::named("Axis"))
            .call([](Runtime& runtime, std::vector<Value>& args) {
                double value = 0.0;
                if (args.size() == 1 && args[0].is_heap() &&
                    args[0].as_heap()->kind == GcObject::Kind::Enum) {
                    value = as_enum(args[0])->variant->name == "Horizontal" ? 1.0 : 0.0;
                }
                return Value::floating(value);
            });
    }

    // math::Vector3<T>
    {
        auto math = host.module("math");
        auto vector = math.class_type("Vector3");
        vector.type_parameter("T");
        vector.field("x", TypeRef::type_variable("T"));
        vector.field("y", TypeRef::type_variable("T"));
        vector.field("z", TypeRef::type_variable("T"));

        vector.static_method("new",
                             TypeRef::named("Vector3", {TypeRef::type_variable("T")}))
            .parameter("x", TypeRef::type_variable("T"))
            .parameter("y", TypeRef::type_variable("T"))
            .parameter("z", TypeRef::type_variable("T"))
            .call([](Runtime& runtime, std::vector<Value>& args) {
                return runtime.make_object(runtime.find_type("math::Vector3"),
                                           {args[0], args[1], args[2]});
            });

        vector.binary_operator("*",
                               TypeRef::named("Vector3", {TypeRef::type_variable("T")}),
                               TypeRef::type_variable("T"),
                               TypeRef::named("Vector3", {TypeRef::type_variable("T")}))
            .call([](Runtime& runtime, std::vector<Value>& args) {
                // Operand order follows the expression; find the vector.
                const ObjectInstance* vector_value = nullptr;
                double scalar = 0.0;
                for (const auto& argument : args) {
                    if (argument.is_heap() &&
                        argument.as_heap()->kind == GcObject::Kind::Object) {
                        vector_value = as_object(argument);
                    } else {
                        scalar = as_number(argument);
                    }
                }
                return runtime.make_object(
                    runtime.find_type("math::Vector3"),
                    {Value::floating(as_number(vector_value->fields[0]) * scalar),
                     Value::floating(as_number(vector_value->fields[1]) * scalar),
                     Value::floating(as_number(vector_value->fields[2]) * scalar)});
            });

        vector.binary_operator("+",
                               TypeRef::named("Vector3", {TypeRef::type_variable("T")}),
                               TypeRef::named("Vector3", {TypeRef::type_variable("T")}),
                               TypeRef::named("Vector3", {TypeRef::type_variable("T")}))
            .call([](Runtime& runtime, std::vector<Value>& args) {
                const auto* a = as_object(args[0]);
                const auto* b = as_object(args[1]);
                return runtime.make_object(
                    runtime.find_type("math::Vector3"),
                    {Value::floating(as_number(a->fields[0]) + as_number(b->fields[0])),
                     Value::floating(as_number(a->fields[1]) + as_number(b->fields[1])),
                     Value::floating(as_number(a->fields[2]) + as_number(b->fields[2]))});
            });
    }

    // Engine-global types.
    {
        auto engine = host.module("engine");

        engine.class_type("EntityBehaviour").global();
        engine.class_type("Span").global();
        engine.class_type("Transform").global()
            .method("translate", TypeRef::unit())
            .parameter("direction",
                       TypeRef::named("math::Vector3", {TypeRef::floating()}))
            .call([](Runtime& runtime, std::vector<Value>& args) {
                (void)runtime;
                (void)args;
                return Value::unit();
            });

        engine.class_type("GameObject").global()
            .method("get_component", TypeRef::type_variable("T"))
            .type_parameter("T")
            .call([](Runtime& runtime, std::vector<Value>& args) {
                (void)args;
                return runtime.make_object(runtime.find_type("engine::Transform"), {});
            });

        // Runtime is a global effect: its operations are callable both as
        // println(...) and as Runtime::println(...). Unhandled operations
        // fall back to these thunks.
        auto runtime_builder = engine.effect("Runtime").global();
        runtime_builder.operation("log", TypeRef::unit())
            .parameter("message", TypeRef::string())
            .call([print](Runtime& runtime, std::vector<Value>& args) {
                if (args.size() == 1) {
                    print(runtime.stringify(args[0]));
                }
                return Value::unit();
            });
        runtime_builder.operation("invalid_downcast", TypeRef::unit())
            .parameter("message", TypeRef::string())
            .aborting()
            .call([print](Runtime& runtime, std::vector<Value>& args) {
                print(args.size() == 1 ? runtime.stringify(args[0]) : "invalid downcast");
                return Value::unit();
            });
        runtime_builder.operation("get_script_name", TypeRef::string())
            .call([](Runtime& runtime, std::vector<Value>& args) {
                (void)args;
                return runtime.make_string("script");
            });
        runtime_builder.operation("get_object", TypeRef::named("GameObject"))
            .call([](Runtime& runtime, std::vector<Value>& args) {
                (void)args;
                return runtime.make_object(runtime.find_type("engine::GameObject"), {});
            });
        runtime_builder.operation("println", TypeRef::unit())
            .parameter("message", TypeRef::string())
            .call([print](Runtime& runtime, std::vector<Value>& args) {
                if (args.size() == 1) {
                    print(runtime.stringify(args[0]));
                }
                return Value::unit();
            });
        runtime_builder.operation("dt", TypeRef::floating())
            .call([](Runtime& runtime, std::vector<Value>& args) {
                (void)runtime;
                (void)args;
                return Value::floating(0.016);
            });
    }
}

} // namespace glscript
