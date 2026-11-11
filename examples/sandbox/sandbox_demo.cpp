// The sandbox engine: glscript behaviours embedded Unity-C# style.
//
// The C++ side owns the EnTT registry, the frame loop, collisions, and
// rendering (raylib). Every entity carries one behaviour instance of a
// script class from sandbox.gl; the engine:
//
//   - acts as the "prefab": it constructs instances and fills the
//     configured fields (Runtime::set_field),
//   - keeps instances alive across collections (Runtime::retain),
//   - drives the lifecycle by dispatching on the instance's runtime class
//     (Runtime::call_method): on_start, update(dt), on_collision(other),
//   - exposes engine services to the script as effect operations (axis,
//     player, spawn, despawn, score, flash).
//
// Run windowed:            ./sandbox_demo
// Run headless (for ctest): ./sandbox_demo --headless [frames]

#include <entt/entt.hpp>

#include "engine_bindings.hpp"

#include "glscript/bytecode.hpp"
#include "glscript/checker.hpp"
#include "glscript/diagnostic.hpp"
#include "glscript/host.hpp"
#include "glscript/runtime.hpp"
#include "glscript/symbols.hpp"
#include "glscript/value.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef GLSCRIPT_HAVE_RAYLIB
#include "raylib.h"
#endif

namespace {

constexpr int kWindowWidth = 800;
constexpr int kWindowHeight = 450;
constexpr float kDt = 1.0f / 60.0f;

enum class Kind {
    Player,
    Pickup,
    Chaser,
    Bullet,
};

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};

struct Velocity {
    float dx = 0.0f;
    float dy = 0.0f;
};

struct Body {
    float radius = 8.0f;
    Kind kind = Kind::Player;
};

struct Name {
    std::string value;
};

struct InputState {
    float horizontal = 0.0f;
    float vertical = 0.0f;
    float fire = 0.0f;
};

// A prefab: which script class to instantiate, the entity's body shape and
// motion, and the per-instance field overrides (the "inspector" values).
struct Prefab {
    std::string class_name;
    Kind kind = Kind::Player;
    float radius = 8.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    std::vector<std::pair<std::string, glscript::Value>> overrides;
};

struct AttachedBehaviour {
    entt::entity owner = entt::null;
    glscript::Value instance;
};

struct Sandbox {
    entt::registry registry;
    std::unordered_map<std::string, Prefab> prefabs;
    std::vector<AttachedBehaviour> behaviours;
    entt::entity player = entt::null;
    int entity_count = 0;
    int score = 0;
    float flash_timer = 0.0f;
    InputState input;

    // Deferred so host thunks never mutate what the frame loop iterates.
    struct PendingAttach {
        entt::entity owner = entt::null;
        std::string prefab;
    };
    std::vector<PendingAttach> pending_attaches;
    std::vector<entt::entity> pending_despawns;
};

glscript::Value make_entity_value(glscript::Runtime& runtime, entt::entity entity) {
    const auto id = static_cast<std::uintptr_t>(entt::to_integral(entity));
    return runtime.make_object(runtime.find_type("game::Entity"), {},
                               reinterpret_cast<void*>(id));
}

entt::entity entity_of(const glscript::Value& value) {
    return static_cast<entt::entity>(
        reinterpret_cast<std::uintptr_t>(as_object(value)->host));
}

// Vec2 helpers (shared shape with the gravity demo's bindings).
bool is_vec2(const glscript::Value& value) {
    return value.is_heap() && value.as_heap()->kind == glscript::GcObject::Kind::Object &&
           as_object(value)->type->name == "Vec2";
}

double vec_field(const glscript::Value& value, std::size_t index) {
    return as_number(as_object(value)->fields[index]);
}

glscript::Value make_vec2(glscript::Runtime& runtime, double x, double y) {
    return runtime.make_object(runtime.find_type("game::Vec2"),
                               {glscript::Value::floating(x), glscript::Value::floating(y)});
}

// Field values for a fresh behaviour instance: the prefab's overrides win,
// everything else gets a type default. Declaration order == slot order.
std::vector<glscript::Value> initial_fields(glscript::Runtime& runtime,
                                            const glscript::TypeSymbol& type,
                                            const Prefab& prefab) {
    std::vector<std::pair<std::size_t, const glscript::FieldSymbol*>> ordered;
    for (const auto& [name, field] : type.fields) {
        (void)name;
        ordered.emplace_back(field.index, &field);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<glscript::Value> values;
    values.reserve(ordered.size());
    for (const auto& [index, field] : ordered) {
        (void)index;
        const glscript::Value* override = nullptr;
        for (const auto& [name, value] : prefab.overrides) {
            if (name == field->name) {
                override = &value;
            }
        }
        if (override != nullptr) {
            values.push_back(*override);
            continue;
        }
        if (field->type->kind == glscript::Type::Kind::Builtin) {
            switch (field->type->builtin) {
            case glscript::BuiltinType::Int:
                values.push_back(glscript::Value::integer(0));
                continue;
            case glscript::BuiltinType::Float:
                values.push_back(glscript::Value::floating(0.0));
                continue;
            case glscript::BuiltinType::Bool:
                values.push_back(glscript::Value::boolean(false));
                continue;
            case glscript::BuiltinType::String:
                values.push_back(runtime.make_string(""));
                continue;
            default:
                break;
            }
        }
        values.push_back(glscript::Value::unit());
    }
    return values;
}

entt::entity spawn_entity(Sandbox& sandbox, const Prefab& prefab, float x, float y) {
    const auto entity = sandbox.registry.create();
    sandbox.registry.emplace<Name>(entity,
                                   Name{"prefab:" + prefab.class_name});
    sandbox.registry.emplace<Position>(entity, Position{x, y});
    sandbox.registry.emplace<Velocity>(entity, Velocity{prefab.vx, prefab.vy});
    sandbox.registry.emplace<Body>(entity, Body{prefab.radius, prefab.kind});
    ++sandbox.entity_count;
    return entity;
}

// Constructs the script class instance for an entity and attaches it. The
// instance is retained: the host holds it, so it must outlive GC cycles.
void attach_behaviour(glscript::Runtime& runtime, Sandbox& sandbox,
                      const glscript::Program& program, const std::string& module_name,
                      entt::entity owner, const Prefab& prefab) {
    auto* module = program.find_module(module_name);
    auto* type = module != nullptr && module->types.contains(prefab.class_name)
                     ? module->types.at(prefab.class_name)
                     : nullptr;
    if (type == nullptr) {
        std::cerr << "sandbox: script class '" << prefab.class_name << "' not found\n";
        return;
    }
    auto instance = runtime.make_object(type, initial_fields(runtime, *type, prefab));
    runtime.set_field(instance, "entity", make_entity_value(runtime, owner));
    runtime.retain(instance);
    sandbox.behaviours.push_back(AttachedBehaviour{owner, std::move(instance)});
}

void register_bindings(glscript::HostRegistry& host, Sandbox& sandbox) {
    using namespace glscript;

    auto game = host.module("game");
    game.class_type("Entity").global();
    auto vec2 = game.class_type("Vec2");
    vec2.field("x", TypeRef::floating());
    vec2.field("y", TypeRef::floating());
    vec2.static_method("new", TypeRef::named("Vec2"))
        .parameter("x", TypeRef::floating())
        .parameter("y", TypeRef::floating())
        .call([](Runtime& runtime, std::vector<Value>& args) {
            if (args.size() != 2) {
                return Value::unit();
            }
            return make_vec2(runtime, as_number(args[0]), as_number(args[1]));
        });
    vec2.method("length", TypeRef::floating())
        .call([](Runtime& runtime, std::vector<Value>& args) {
            (void)runtime;
            if (args.empty() || !is_vec2(args[0])) {
                return Value::unit();
            }
            const double x = vec_field(args[0], 0);
            const double y = vec_field(args[0], 1);
            return Value::floating(std::sqrt(x * x + y * y));
        });
    vec2.binary_operator("+", TypeRef::named("Vec2"), TypeRef::named("Vec2"),
                         TypeRef::named("Vec2"))
        .call([](Runtime& runtime, std::vector<Value>& args) {
            if (args.size() != 2 || !is_vec2(args[0]) || !is_vec2(args[1])) {
                return Value::unit();
            }
            return make_vec2(runtime, vec_field(args[0], 0) + vec_field(args[1], 0),
                             vec_field(args[0], 1) + vec_field(args[1], 1));
        });
    vec2.binary_operator("-", TypeRef::named("Vec2"), TypeRef::named("Vec2"),
                         TypeRef::named("Vec2"))
        .call([](Runtime& runtime, std::vector<Value>& args) {
            if (args.size() != 2 || !is_vec2(args[0]) || !is_vec2(args[1])) {
                return Value::unit();
            }
            return make_vec2(runtime, vec_field(args[0], 0) - vec_field(args[1], 0),
                             vec_field(args[0], 1) - vec_field(args[1], 1));
        });
    vec2.binary_operator("*", TypeRef::named("Vec2"), TypeRef::floating(),
                         TypeRef::named("Vec2"))
        .call([](Runtime& runtime, std::vector<Value>& args) {
            if (args.size() != 2) {
                return Value::unit();
            }
            const Value* vector = nullptr;
            double scalar = 0.0;
            for (const auto& argument : args) {
                if (is_vec2(argument)) {
                    vector = &argument;
                } else {
                    scalar = as_number(argument);
                }
            }
            if (vector == nullptr) {
                return Value::unit();
            }
            return make_vec2(runtime, vec_field(*vector, 0) * scalar,
                             vec_field(*vector, 1) * scalar);
        });

    auto entity = game.class_type("Entity");
    entity.method("position", TypeRef::named("Vec2"))
        .call([&sandbox](Runtime& runtime, std::vector<Value>& args) {
            if (args.empty() || !sandbox.registry.valid(entity_of(args[0]))) {
                return Value::unit();
            }
            const auto& position = sandbox.registry.get<Position>(entity_of(args[0]));
            return make_vec2(runtime, position.x, position.y);
        });
    entity.method("set_position", TypeRef::unit())
        .parameter("value", TypeRef::named("Vec2"))
        .call([&sandbox](Runtime& runtime, std::vector<Value>& args) {
            (void)runtime;
            if (args.size() != 2 || !is_vec2(args[1]) ||
                !sandbox.registry.valid(entity_of(args[0]))) {
                return Value::unit();
            }
            auto& position = sandbox.registry.get<Position>(entity_of(args[0]));
            position.x = static_cast<float>(vec_field(args[1], 0));
            position.y = static_cast<float>(vec_field(args[1], 1));
            return Value::unit();
        });

    // Script -> engine services, exposed as global effect operations.
    auto input = host.module("app").effect("Input").global();
    input.operation("axis", TypeRef::floating())
        .parameter("name", TypeRef::string())
        .call([&sandbox](Runtime& runtime, std::vector<Value>& args) {
            (void)runtime;
            if (args.size() != 1) {
                return Value::floating(0.0);
            }
            const auto name = runtime.stringify(args[0]);
            if (name == "horizontal") {
                return Value::floating(sandbox.input.horizontal);
            }
            if (name == "vertical") {
                return Value::floating(sandbox.input.vertical);
            }
            if (name == "fire") {
                return Value::floating(sandbox.input.fire);
            }
            return Value::floating(0.0);
        });

    auto world = host.module("app").effect("World").global();
    world.operation("player", TypeRef::named("Entity"))
        .call([&sandbox](Runtime& runtime, std::vector<Value>& args) {
            (void)args;
            return make_entity_value(runtime, sandbox.player);
        });
    world.operation("spawn", TypeRef::named("Entity"))
        .parameter("prefab", TypeRef::string())
        .parameter("x", TypeRef::floating())
        .parameter("y", TypeRef::floating())
        .call([&sandbox](Runtime& runtime, std::vector<Value>& args) {
            if (args.size() != 3) {
                return Value::unit();
            }
            const auto it = sandbox.prefabs.find(runtime.stringify(args[0]));
            if (it == sandbox.prefabs.end()) {
                return Value::unit();
            }
            const auto entity = spawn_entity(sandbox, it->second,
                                             static_cast<float>(as_number(args[1])),
                                             static_cast<float>(as_number(args[2])));
            sandbox.pending_attaches.push_back({entity, it->first});
            return make_entity_value(runtime, entity);
        });
    world.operation("despawn", TypeRef::unit())
        .parameter("entity", TypeRef::named("Entity"))
        .call([&sandbox](Runtime& runtime, std::vector<Value>& args) {
            (void)runtime;
            if (args.size() == 1 && args[0].is_heap() &&
                args[0].as_heap()->kind == GcObject::Kind::Object) {
                sandbox.pending_despawns.push_back(entity_of(args[0]));
            }
            return Value::unit();
        });
    world.operation("score", TypeRef::integer())
        .call([&sandbox](Runtime& runtime, std::vector<Value>& args) {
            (void)runtime;
            (void)args;
            return Value::integer(sandbox.score);
        });
    world.operation("add_score", TypeRef::unit())
        .parameter("amount", TypeRef::integer())
        .call([&sandbox](Runtime& runtime, std::vector<Value>& args) {
            (void)runtime;
            if (args.size() == 1) {
                sandbox.score += static_cast<int>(as_number(args[0]));
            }
            return Value::unit();
        });
    world.operation("flash", TypeRef::unit())
        .call([&sandbox](Runtime& runtime, std::vector<Value>& args) {
            (void)runtime;
            (void)args;
            sandbox.flash_timer = 0.15f;
            return Value::unit();
        });
}

AttachedBehaviour* find_behaviour(Sandbox& sandbox, entt::entity owner) {
    for (auto& behaviour : sandbox.behaviours) {
        if (behaviour.owner == owner) {
            return &behaviour;
        }
    }
    return nullptr;
}

bool has_method(const glscript::Value& instance, const std::string& name) {
    return as_object(instance)->type->methods.contains(name);
}

// One frame: integrate, update behaviours, resolve collisions, then apply
// the deferred spawns and despawns. call_method is only ever called from
// here — never from inside a host thunk — because execute() is not
// re-entrant.
bool update_frame(glscript::Runtime& runtime, Sandbox& sandbox,
                  const glscript::Program& program, const std::string& module_name,
                  float dt, glscript::DiagnosticSink& diagnostics) {
    // Host-side integration for entities with velocity.
    for (auto [entity, position, velocity] :
         sandbox.registry.view<Position, Velocity>().each()) {
        (void)entity;
        position.x += velocity.dx * dt;
        position.y += velocity.dy * dt;
    }

    // Script lifecycle: update(dt) on every attached behaviour.
    for (auto& behaviour : sandbox.behaviours) {
        if (!sandbox.registry.valid(behaviour.owner)) {
            continue;
        }
        runtime.call_method(behaviour.instance, "update",
                            {glscript::Value::floating(dt)});
        if (diagnostics.has_errors()) {
            std::cerr << diagnostics.format();
            return false;
        }
    }

    // Collision routing: overlapping bodies whose kinds interact get
    // on_collision(other) on both behaviours.
    const auto bodies = sandbox.registry.view<Position, Body>();
    std::vector<entt::entity> hits;
    for (auto a : bodies) {
        for (auto b : bodies) {
            if (entt::to_integral(a) >= entt::to_integral(b)) {
                continue;
            }
            const auto& body_a = bodies.get<Body>(a);
            const auto& body_b = bodies.get<Body>(b);
            Kind low = body_a.kind, high = body_b.kind;
            if (static_cast<int>(low) > static_cast<int>(high)) {
                std::swap(low, high);
            }
            const bool routed =
                (low == Kind::Player && high == Kind::Pickup) ||
                (low == Kind::Player && high == Kind::Chaser) ||
                (low == Kind::Pickup && high == Kind::Bullet) ||
                (low == Kind::Chaser && high == Kind::Bullet);
            if (!routed) {
                continue;
            }
            const auto& pa = bodies.get<Position>(a);
            const auto& pb = bodies.get<Position>(b);
            const float dx = pa.x - pb.x;
            const float dy = pa.y - pb.y;
            const float radius = body_a.radius + body_b.radius;
            if (dx * dx + dy * dy <= radius * radius) {
                hits.push_back(a);
                hits.push_back(b);
            }
        }
    }
    for (std::size_t i = 0; i + 1 < hits.size(); i += 2) {
        for (const auto owner : {hits[i], hits[i + 1]}) {
            auto* behaviour = find_behaviour(sandbox, owner);
            if (behaviour == nullptr || !has_method(behaviour->instance, "on_collision")) {
                continue;
            }
            const entt::entity other = owner == hits[i] ? hits[i + 1] : hits[i];
            runtime.call_method(behaviour->instance, "on_collision",
                                {make_entity_value(runtime, other)});
            if (diagnostics.has_errors()) {
                std::cerr << diagnostics.format();
                return false;
            }
        }
    }

    // Deferred spawns: attach the behaviour now, outside any script call.
    for (const auto& pending : sandbox.pending_attaches) {
        const auto it = sandbox.prefabs.find(pending.prefab);
        if (it == sandbox.prefabs.end()) {
            continue;
        }
        attach_behaviour(runtime, sandbox, program, module_name, pending.owner,
                         it->second);
        auto* behaviour = find_behaviour(sandbox, pending.owner);
        if (behaviour != nullptr && has_method(behaviour->instance, "on_start")) {
            runtime.call_method(behaviour->instance, "on_start");
        }
    }
    sandbox.pending_attaches.clear();

    // Deferred despawns.
    for (const auto entity : sandbox.pending_despawns) {
        if (sandbox.registry.valid(entity)) {
            sandbox.registry.destroy(entity);
            --sandbox.entity_count;
        }
        std::erase_if(sandbox.behaviours,
                      [&](const AttachedBehaviour& behaviour) {
                          return behaviour.owner == entity;
                      });
    }
    sandbox.pending_despawns.clear();

    if (sandbox.flash_timer > 0.0f) {
        sandbox.flash_timer -= dt;
    }
    return true;
}

#ifdef GLSCRIPT_HAVE_RAYLIB
int run_windowed(glscript::Runtime& runtime, Sandbox& sandbox,
                 const glscript::Program& program, const std::string& module_name,
                 glscript::DiagnosticSink& diagnostics) {
    InitWindow(kWindowWidth, kWindowHeight,
               "glscript sandbox - Unity-style script behaviours");
    SetTargetFPS(60);

    int frame = 0;
    while (!WindowShouldClose()) {
        // Keyboard -> InputState (the script reads it via the axis() op).
        sandbox.input.horizontal =
            (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT) ? 1.0f : 0.0f) -
            (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT) ? 1.0f : 0.0f);
        sandbox.input.vertical =
            (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN) ? 1.0f : 0.0f) -
            (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP) ? 1.0f : 0.0f);
        sandbox.input.fire = IsKeyDown(KEY_SPACE) ? 1.0f : 0.0f;

        float dt = GetFrameTime();
        if (dt > kDt) {
            dt = kDt;
        }
        if (!update_frame(runtime, sandbox, program, module_name, dt, diagnostics)) {
            break;
        }
        ++frame;

        BeginDrawing();
        ClearBackground(Color{16, 18, 26, 255});
        DrawRectangleLinesEx(Rectangle{20, 36, kWindowWidth - 40.0f,
                                       kWindowHeight - 76.0f},
                             2.0f, Color{70, 76, 92, 255});

        const auto view = sandbox.registry.view<Position, Body>();
        for (auto entity : view) {
            const auto& position = view.get<Position>(entity);
            const auto& body = view.get<Body>(entity);
            switch (body.kind) {
            case Kind::Player:
                DrawCircleV({position.x, position.y}, body.radius,
                            Color{235, 238, 245, 255});
                break;
            case Kind::Pickup: {
                float spin = 0.0f;
                if (auto* behaviour = find_behaviour(sandbox, entity);
                    behaviour != nullptr) {
                    spin = static_cast<float>(
                        as_number(runtime.field(behaviour->instance, "spin")));
                }
                // Script state drives the rendering: spin comes from the
                // behaviour instance's field.
                const Vector2 center{position.x, position.y};
                const float angle = spin;
                for (int k = 0; k < 2; ++k) {
                    const float a = angle + k * 1.5707963f;
                    DrawLineEx({center.x - cosf(a) * body.radius,
                                center.y - sinf(a) * body.radius},
                               {center.x + cosf(a) * body.radius,
                                center.y + sinf(a) * body.radius},
                               3.0f, Color{244, 208, 92, 255});
                }
                break;
            }
            case Kind::Chaser:
                DrawCircleV({position.x, position.y}, body.radius,
                            Color{226, 88, 88, 255});
                break;
            case Kind::Bullet:
                DrawCircleV({position.x, position.y}, body.radius,
                            Color{120, 200, 255, 255});
                break;
            }
        }

        if (sandbox.flash_timer > 0.0f) {
            DrawRectangle(0, 0, kWindowWidth, kWindowHeight,
                          Color{255, 120, 120,
                                static_cast<unsigned char>(sandbox.flash_timer /
                                                           0.15f * 60)});
        }

        DrawText("glscript sandbox - behaviours are script classes; the engine drives on_start/update/on_collision",
                 12, 10, 15, Color{226, 229, 238, 255});
        DrawText(TextFormat("score: %d    entities: %zu    behaviours: %zu    fps: %d",
                            sandbox.score,
                            static_cast<std::size_t>(sandbox.entity_count),
                            sandbox.behaviours.size(), GetFPS()),
                 12, 30, 13, Color{150, 155, 170, 255});
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
#endif // GLSCRIPT_HAVE_RAYLIB

// Deterministic headless run: a scripted input timeline collects all three
// pickups with bullets fired on the move.
int run_headless(glscript::Runtime& runtime, Sandbox& sandbox,
                 const glscript::Program& program, const std::string& module_name,
                 int frames, glscript::DiagnosticSink& diagnostics) {
    int fired = 0;
    for (int frame = 0; frame < frames; ++frame) {
        sandbox.input.horizontal = frame < 130 ? 1.0f : 0.0f;
        sandbox.input.vertical = 0.0f;
        sandbox.input.fire = (frame == 20 || frame == 60 || frame == 100) ? 1.0f : 0.0f;
        if (sandbox.input.fire > 0.0f) {
            ++fired;
        }
        if (!update_frame(runtime, sandbox, program, module_name, kDt, diagnostics)) {
            return 1;
        }
    }

    std::cout << "headless sandbox: " << fired << " shots fired, score " << sandbox.score
              << ", " << static_cast<std::size_t>(sandbox.entity_count) << " entities, "
              << sandbox.behaviours.size() << " behaviours\n";
    if (sandbox.score < 3) {
        std::cerr << "headless sandbox: expected to collect all 3 pickups\n";
        return 1;
    }
    std::cout << "all pickups collected: scripted physics, spawning and\n"
                 "collision lifecycle ran end to end\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
#ifdef GLSCRIPT_SANDBOX_SCRIPT
    std::string script_path = GLSCRIPT_SANDBOX_SCRIPT;
#else
    std::string script_path = "examples/sandbox/sandbox.gl";
#endif

    bool headless = false;
    int frames = 300;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--headless") {
            headless = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                frames = std::atoi(argv[++i]);
            }
            continue;
        }
        script_path = argument;
    }

    const std::string module_name =
        std::filesystem::path(script_path).stem().string();

    Sandbox sandbox;
    sandbox.prefabs["pickup"] = Prefab{
        "PickupBehaviour", Kind::Pickup, 10.0f, 0.0f, 0.0f,
        {{"spin_speed", glscript::Value::floating(3.0)}}};
    sandbox.prefabs["bullet"] = Prefab{
        "BulletBehaviour", Kind::Bullet, 5.0f, 0.0f, -420.0f,
        {{"life", glscript::Value::floating(2.0)}}};
    sandbox.prefabs["chaser"] = Prefab{
        "ChaserBehaviour", Kind::Chaser, 12.0f, 0.0f, 0.0f,
        {{"speed", glscript::Value::floating(90.0)}}};

    glscript::HostRegistry host;
    glscript::register_engine_bindings(host); // println / log
    register_bindings(host, sandbox);

    glscript::DiagnosticSink diagnostics;
    glscript::Frontend frontend(host, diagnostics);
    if (!frontend.check_file(script_path)) {
        std::cerr << diagnostics.format();
        return 1;
    }

    const auto& program = frontend.program();
    auto bytecode = glscript::compile_program(program);
    glscript::Runtime runtime(program, bytecode, host, diagnostics);

    // The player: an entity plus its behaviour, the engine acting as prefab.
    const Prefab player_prefab{
        "PlayerBehaviour", Kind::Player, 14.0f, 0.0f, 0.0f,
        {{"speed", glscript::Value::floating(240.0)},
         {"cooldown", glscript::Value::floating(0.22)}}};
    sandbox.prefabs["player"] = player_prefab;
    sandbox.player = spawn_entity(sandbox, player_prefab, 120.0f, 400.0f);
    attach_behaviour(runtime, sandbox, program, module_name, sandbox.player,
                     player_prefab);

    // Three pickups and a chaser, placed deterministically.
    const auto apply = [&](const char* prefab, float x, float y) {
        const auto entity = spawn_entity(sandbox, sandbox.prefabs.at(prefab), x, y);
        attach_behaviour(runtime, sandbox, program, module_name, entity,
                         sandbox.prefabs.at(prefab));
        return entity;
    };
    apply("pickup", 200.0f, 260.0f);
    apply("pickup", 360.0f, 260.0f);
    apply("pickup", 520.0f, 260.0f);
    apply("chaser", 720.0f, 120.0f);

    // Lifecycle start for everything attached so far.
    for (const auto& behaviour : sandbox.behaviours) {
        if (has_method(behaviour.instance, "on_start")) {
            runtime.call_method(behaviour.instance, "on_start");
        }
    }
    if (diagnostics.has_errors()) {
        std::cerr << diagnostics.format();
        return 1;
    }

#ifdef GLSCRIPT_HAVE_RAYLIB
    if (headless) {
        return run_headless(runtime, sandbox, program, module_name, frames, diagnostics);
    }
    return run_windowed(runtime, sandbox, program, module_name, diagnostics);
#else
    if (!headless) {
        std::cout << "built without raylib; running headless\n";
    }
    return run_headless(runtime, sandbox, program, module_name, frames, diagnostics);
#endif
}
