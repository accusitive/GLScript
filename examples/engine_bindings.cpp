#include "engine_bindings.hpp"

namespace glscript {

void register_engine_bindings(HostRegistry& host) {
    // input::Axis and input::Input
    {
        auto input = host.module("input");
        input.enum_type("Axis")
            .variant("Horizontal")
            .variant("Vertical");

        input.class_type("Input")
            .static_method("get_axis", TypeRef::floating())
            .parameter("axis", TypeRef::named("Axis"));
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
            .parameter("z", TypeRef::type_variable("T"));

        vector.binary_operator("*",
                               TypeRef::named("Vector3", {TypeRef::type_variable("T")}),
                               TypeRef::type_variable("T"),
                               TypeRef::named("Vector3", {TypeRef::type_variable("T")}));
        vector.binary_operator("+",
                               TypeRef::named("Vector3", {TypeRef::type_variable("T")}),
                               TypeRef::named("Vector3", {TypeRef::type_variable("T")}),
                               TypeRef::named("Vector3", {TypeRef::type_variable("T")}));
    }

    // Engine-global types.
    {
        auto engine = host.module("engine");

        engine.class_type("EntityBehaviour").global();
        engine.class_type("Span").global();
        engine.class_type("Transform").global()
            .method("translate", TypeRef::unit())
            .parameter("direction",
                       TypeRef::named("math::Vector3", {TypeRef::floating()}));

        engine.class_type("GameObject").global()
            .method("get_component", TypeRef::type_variable("T"))
            .type_parameter("T");

        // Runtime is a global effect: its operations are callable both as
        // println(...) and as Runtime::println(...).
        auto runtime = engine.effect("Runtime").global();
        runtime.operation("log", TypeRef::unit())
            .parameter("message", TypeRef::string());
        runtime.operation("invalid_downcast", TypeRef::unit())
            .parameter("message", TypeRef::string())
            .aborting();
        runtime.operation("get_script_name", TypeRef::string());
        runtime.operation("get_object", TypeRef::named("GameObject"));
        runtime.operation("println", TypeRef::unit())
            .parameter("message", TypeRef::string());
        runtime.operation("dt", TypeRef::floating());
    }
}

} // namespace glscript
