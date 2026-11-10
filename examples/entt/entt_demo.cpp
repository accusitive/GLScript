#include <entt/entt.hpp>

#include "glscript/checker.hpp"
#include "glscript/diagnostic.hpp"
#include "glscript/host.hpp"

#include <iostream>
#include <string>

namespace {

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};

struct Velocity {
    float dx = 0.0f;
    float dy = 0.0f;
};

struct Health {
    float value = 100.0f;
};

struct Name {
    std::string value;
};

// The C++ side of the script-facing `game::Entity` and `World` operations.
// The typechecker only needs their signatures; these functions show the EnTT
// backing they are meant to run against.
struct ScriptWorld {
    entt::registry registry;
    entt::entity player = entt::null;
    entt::entity enemy = entt::null;

    static ScriptWorld make() {
        ScriptWorld world;

        world.player = world.registry.create();
        world.registry.emplace<Name>(world.player, Name{"player"});
        world.registry.emplace<Position>(world.player, Position{0.0f, 0.0f});
        world.registry.emplace<Velocity>(world.player, Velocity{1.0f, 0.0f});
        world.registry.emplace<Health>(world.player, Health{100.0f});

        world.enemy = world.registry.create();
        world.registry.emplace<Name>(world.enemy, Name{"goblin"});
        world.registry.emplace<Position>(world.enemy, Position{1.5f, 0.0f});
        world.registry.emplace<Velocity>(world.enemy, Velocity{0.0f, 0.0f});
        world.registry.emplace<Health>(world.enemy, Health{30.0f});

        return world;
    }

    entt::entity player_entity() const { return player; }
    entt::entity enemy_entity() const { return enemy; }

    const Name& name(entt::entity entity) const { return registry.get<Name>(entity); }
    const Position& position(entt::entity entity) const {
        return registry.get<Position>(entity);
    }
    float health(entt::entity entity) const { return registry.get<Health>(entity).value; }

    void move(entt::entity entity, float dx, float dy) {
        auto& position = registry.get<Position>(entity);
        position.x += dx;
        position.y += dy;
    }

    void damage(entt::entity entity, float amount) {
        registry.get<Health>(entity).value -= amount;
    }

    bool is_alive(entt::entity entity) const { return health(entity) > 0.0f; }

    void despawn(entt::entity entity) {
        if (registry.valid(entity)) {
            registry.destroy(entity);
        }
    }
};

void register_script_bindings(glscript::HostRegistry& host) {
    using glscript::TypeRef;

    auto game = host.module("game");

    // game::Vec2 is a plain script value backed by Position-like data.
    auto vec2 = game.class_type("Vec2");
    vec2.field("x", TypeRef::floating());
    vec2.field("y", TypeRef::floating());
    vec2.static_method("new", TypeRef::named("Vec2"))
        .parameter("x", TypeRef::floating())
        .parameter("y", TypeRef::floating());
    vec2.method("length", TypeRef::floating());
    vec2.binary_operator("-", TypeRef::named("Vec2"), TypeRef::named("Vec2"),
                         TypeRef::named("Vec2"));
    vec2.binary_operator("+", TypeRef::named("Vec2"), TypeRef::named("Vec2"),
                         TypeRef::named("Vec2"));

    // game::Entity is an opaque entt::entity handle.
    auto entity = game.class_type("Entity");
    entity.method("name", TypeRef::string());
    entity.method("position", TypeRef::named("Vec2"));
    entity.method("move", TypeRef::unit())
        .parameter("dx", TypeRef::floating())
        .parameter("dy", TypeRef::floating());
    entity.method("health", TypeRef::floating());
    entity.method("damage", TypeRef::unit())
        .parameter("amount", TypeRef::floating());
    entity.method("is_alive", TypeRef::boolean());

    // World is a global effect, so its operations are available both as
    // `World::player()` and as ambient `player()` / `println()` calls.
    auto world = game.effect("World").global();
    world.operation("player", TypeRef::named("Entity"));
    world.operation("enemy", TypeRef::named("Entity"));
    world.operation("println", TypeRef::unit())
        .parameter("message", TypeRef::string());
    world.operation("despawn", TypeRef::unit())
        .parameter("entity", TypeRef::named("Entity"));
}

void print_world(const ScriptWorld& world) {
    auto view = world.registry.view<Name, Position, Health>();
    for (auto entity : view) {
        const auto& name = view.get<Name>(entity);
        const auto& position = view.get<Position>(entity);
        const auto& health = view.get<Health>(entity);

        std::cout << "  - " << name.value << " (entity " << entt::to_integral(entity)
                  << ") position=(" << position.x << ", " << position.y
                  << ") health=" << health.value << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
#ifdef GLSCRIPT_ENTT_DEMO_SCRIPT
    std::string script_path = GLSCRIPT_ENTT_DEMO_SCRIPT;
#else
    std::string script_path = "examples/entt/entt_demo.gl";
#endif
    if (argc > 1) {
        script_path = argv[1];
    }

    auto world = ScriptWorld::make();

    std::cout << "EnTT registry (the entities the script will act on):\n";
    print_world(world);

    glscript::HostRegistry host;
    register_script_bindings(host);

    glscript::DiagnosticSink diagnostics;
    glscript::Frontend frontend(host, diagnostics);
    if (!frontend.check_file(script_path)) {
        std::cerr << diagnostics.format();
        return 1;
    }

    std::cout << "\nchecked " << script_path << " successfully (0 warning(s))\n";
    std::cout << "The script's `update()` is validated against the EnTT-backed game::Entity\n"
                 "and World bindings above. Execution is the next glscript milestone.\n";
    return 0;
}
