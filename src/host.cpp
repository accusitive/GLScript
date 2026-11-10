#include "glscript/host.hpp"

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

} // namespace glscript
