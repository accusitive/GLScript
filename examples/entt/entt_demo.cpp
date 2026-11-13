// EnTT + raylib demo: a small gravity simulation where every entity's
// physics step is a glscript invocation.
//
// The EnTT registry stores Position/Velocity/Body state in C++. Each frame,
// the host calls the script function `update(entity, dt)` once per entity —
// the script does the gravity, integration, and bounce logic through the
// game::Entity and game::Vec2 host bindings below. raylib renders the
// registry.
//
// Run windowed:      ./entt_demo [script.gl]
// Run headless (for tests): ./entt_demo --headless [frames] [script.gl]
// Debug (waits for the NSH debugger): ./entt_demo --debug[=port]

#include <entt/entt.hpp>

#include "debug_session.hpp"
#include "engine_bindings.hpp"

#include "glscript/bytecode.hpp"
#include "glscript/checker.hpp"
#include "glscript/diagnostic.hpp"
#include "glscript/host.hpp"
#include "glscript/runtime.hpp"
#include "glscript/symbols.hpp"
#include "glscript/value.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#ifdef GLSCRIPT_HAVE_RAYLIB
#include "raylib.h"
#endif

namespace {

constexpr int kWindowWidth = 800;
constexpr int kWindowHeight = 450;
constexpr float kFloorY = 380.0f;     // body centers bounce here
constexpr float kMaxRadius = 13.0f;
constexpr float kWallLeft = 24.0f;
constexpr float kWallRight = 776.0f;

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
};

struct Name {
    std::string value;
};

// The EnTT world the script acts on.
struct ScriptWorld {
    entt::registry registry;
    std::vector<entt::entity> bodies;

    static ScriptWorld make(std::size_t count) {
        std::mt19937 rng(42); // fixed seed: the headless test is deterministic
        std::uniform_real_distribution<float> x(60.0f, 740.0f);
        std::uniform_real_distribution<float> y(40.0f, 160.0f);
        std::uniform_real_distribution<float> vx(-70.0f, 70.0f);
        std::uniform_real_distribution<float> vy(-30.0f, 30.0f);
        std::uniform_real_distribution<float> radius(5.0f, kMaxRadius);

        ScriptWorld world;
        for (std::size_t i = 0; i < count; ++i) {
            const auto entity = world.registry.create();
            world.registry.emplace<Name>(entity, Name{"body-" + std::to_string(i)});
            world.registry.emplace<Position>(entity, Position{x(rng), y(rng)});
            world.registry.emplace<Velocity>(entity, Velocity{vx(rng), vy(rng)});
            world.registry.emplace<Body>(entity, Body{radius(rng)});
            world.bodies.push_back(entity);
        }
        return world;
    }
};

// game::Entity values are opaque object handles carrying the entt entity id
// in the ObjectInstance's host pointer.
entt::entity entity_of(const glscript::Value& value) {
    return static_cast<entt::entity>(
        reinterpret_cast<std::uintptr_t>(as_object(value)->host));
}

glscript::Value make_entity_value(glscript::Runtime& runtime, entt::entity entity) {
    const auto id = static_cast<std::uintptr_t>(entt::to_integral(entity));
    return runtime.make_object(runtime.find_type("game::Entity"), {},
                               reinterpret_cast<void*>(id));
}

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

void register_game_bindings(glscript::HostRegistry& host, ScriptWorld& world) {
    using namespace glscript;

    auto game = host.module("game");

    // game::Vec2 — a plain script value; the host constructs instances and
    // implements length() and the arithmetic operators.
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

    // game::Entity — an opaque handle whose methods read and write the
    // EnTT registry.
    auto entity = game.class_type("Entity");
    entity.method("name", TypeRef::string())
        .call([&world](Runtime& runtime, std::vector<Value>& args) {
            if (args.empty() || !args[0].is_heap() ||
                args[0].as_heap()->kind != GcObject::Kind::Object) {
                return Value::unit();
            }
            return runtime.make_string(
                world.registry.get<Name>(entity_of(args[0])).value);
        });
    entity.method("position", TypeRef::named("Vec2"))
        .call([&world](Runtime& runtime, std::vector<Value>& args) {
            if (args.empty() || !args[0].is_heap() ||
                args[0].as_heap()->kind != GcObject::Kind::Object) {
                return Value::unit();
            }
            const auto& position = world.registry.get<Position>(entity_of(args[0]));
            return make_vec2(runtime, position.x, position.y);
        });
    entity.method("set_position", TypeRef::unit())
        .parameter("value", TypeRef::named("Vec2"))
        .call([&world](Runtime& runtime, std::vector<Value>& args) {
            (void)runtime;
            if (args.size() != 2 || !is_vec2(args[1])) {
                return Value::unit();
            }
            auto& position = world.registry.get<Position>(entity_of(args[0]));
            position.x = static_cast<float>(vec_field(args[1], 0));
            position.y = static_cast<float>(vec_field(args[1], 1));
            return Value::unit();
        });
    entity.method("velocity", TypeRef::named("Vec2"))
        .call([&world](Runtime& runtime, std::vector<Value>& args) {
            if (args.empty() || !args[0].is_heap() ||
                args[0].as_heap()->kind != GcObject::Kind::Object) {
                return Value::unit();
            }
            const auto& velocity = world.registry.get<Velocity>(entity_of(args[0]));
            return make_vec2(runtime, velocity.dx, velocity.dy);
        });
    entity.method("set_velocity", TypeRef::unit())
        .parameter("value", TypeRef::named("Vec2"))
        .call([&world](Runtime& runtime, std::vector<Value>& args) {
            (void)runtime;
            if (args.size() != 2 || !is_vec2(args[1])) {
                return Value::unit();
            }
            auto& velocity = world.registry.get<Velocity>(entity_of(args[0]));
            velocity.dx = static_cast<float>(vec_field(args[1], 0));
            velocity.dy = static_cast<float>(vec_field(args[1], 1));
            return Value::unit();
        });
}

// Runs the script's `update(entity, dt)` once per entity. Returns false when
// the script failed.
bool step(glscript::Runtime& runtime, ScriptWorld& world,
          const std::string& module_name, float dt,
          glscript::DiagnosticSink& diagnostics) {
    for (const auto entity : world.bodies) {
        if (!world.registry.valid(entity)) {
            continue;
        }
        std::vector<glscript::Value> arguments;
        arguments.push_back(make_entity_value(runtime, entity));
        arguments.push_back(glscript::Value::floating(dt));
        runtime.run(module_name, "update", std::move(arguments));
        if (diagnostics.has_errors()) {
            std::cerr << diagnostics.format();
            return false;
        }
    }
    return true;
}

void print_state(const ScriptWorld& world) {
    const auto view = world.registry.view<Name, Position, Velocity>();
    for (auto entity : view) {
        const auto& name = view.get<Name>(entity);
        const auto& position = view.get<Position>(entity);
        const auto& velocity = view.get<Velocity>(entity);
        std::printf("  %-10s position=(%7.2f, %7.2f) velocity=(%7.2f, %7.2f)\n",
                    name.value.c_str(), position.x, position.y, velocity.dx, velocity.dy);
    }
}

int run_headless(glscript::Runtime& runtime, ScriptWorld& world,
                 const std::string& module_name, int frames,
                 glscript::DiagnosticSink& diagnostics) {
    constexpr float kDt = 1.0f / 60.0f;
    for (int frame = 0; frame < frames; ++frame) {
        if (!step(runtime, world, module_name, kDt, diagnostics)) {
            return 1;
        }
    }
    std::cout << "headless gravity sim: " << world.bodies.size()
              << " entities, " << frames << " frames of scripted physics\n";
    print_state(world);
    return 0;
}

#ifdef GLSCRIPT_HAVE_RAYLIB
int run_windowed(glscript::Runtime& runtime, ScriptWorld& world,
                 const std::string& module_name,
                 glscript::DiagnosticSink& diagnostics) {
    InitWindow(kWindowWidth, kWindowHeight,
               "glscript gravity sim - entt registry, scripted physics");
    SetTargetFPS(60);

    const std::size_t count = world.bodies.size();
    int frame = 0;
    while (!WindowShouldClose()) {
        const float dt = std::min(GetFrameTime(), 1.0f / 20.0f);
        if (!step(runtime, world, module_name, dt, diagnostics)) {
            break;
        }
        ++frame;

        BeginDrawing();
        ClearBackground(Color{18, 20, 28, 255});

        DrawLine(0, static_cast<int>(kFloorY + kMaxRadius), kWindowWidth,
                 static_cast<int>(kFloorY + kMaxRadius), Color{90, 96, 110, 255});

        for (std::size_t i = 0; i < world.bodies.size(); ++i) {
            const auto entity = world.bodies[i];
            const auto& position = world.registry.get<Position>(entity);
            const auto& velocity = world.registry.get<Velocity>(entity);
            const float radius = world.registry.get<Body>(entity).radius;
            const auto color =
                ColorFromHSV(static_cast<float>(i) * 360.0f / static_cast<float>(count),
                             0.65f, 0.92f);

            DrawLineEx({position.x, position.y},
                       {position.x - velocity.dx * 0.12f, position.y - velocity.dy * 0.12f},
                       2.0f, Color{color.r, color.g, color.b, 110});
            DrawCircleV({position.x, position.y}, radius, color);
            DrawCircleLines(static_cast<int>(position.x), static_cast<int>(position.y),
                            radius, Color{0, 0, 0, 90});
        }

        DrawText("glscript gravity sim - one scripted update(entity, dt) per entity per frame",
                 12, 10, 16, Color{230, 232, 240, 255});
        DrawText(TextFormat("bodies: %zu   frame: %d   fps: %d", count, frame, GetFPS()),
                 12, 30, 12, Color{150, 155, 170, 255});
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
#endif // GLSCRIPT_HAVE_RAYLIB

} // namespace

int main(int argc, char** argv) {
#ifdef GLSCRIPT_ENTT_DEMO_SCRIPT
    std::string script_path = GLSCRIPT_ENTT_DEMO_SCRIPT;
#else
    std::string script_path = "examples/entt/gravity.gl";
#endif

    bool headless = false;
    int frames = 240;
    glscript::demo::DebugOptions debug_options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        std::string error;
        switch (glscript::demo::parse_debug_argument(argument, debug_options, error)) {
        case glscript::demo::DebugArgResult::Parsed:
            continue;
        case glscript::demo::DebugArgResult::Invalid:
            std::cerr << "entt_demo: " << error << '\n';
            return 2;
        case glscript::demo::DebugArgResult::NotDebug:
            break;
        }
        if (argument == "--headless") {
            headless = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                frames = std::atoi(argv[++i]);
            }
            continue;
        }
        script_path = argument;
    }

    auto world = ScriptWorld::make(12);

    glscript::HostRegistry host;
    glscript::demo::DebugSession* debug_session = nullptr;
    glscript::register_engine_bindings(host, [&](const std::string& line) {
        std::cout << line << '\n';
        if (debug_session != nullptr) {
            debug_session->emit_output(line + '\n');
        }
    });
    register_game_bindings(host, world);

    glscript::DiagnosticSink diagnostics;
    glscript::Frontend frontend(host, diagnostics);
    if (!frontend.check_file(script_path)) {
        std::cerr << diagnostics.format();
        return 1;
    }

    auto bytecode = glscript::compile_program(frontend.program());
    glscript::Runtime runtime(frontend.program(), bytecode, host, diagnostics);

    glscript::demo::DebugSession session(runtime, frontend.program(), bytecode,
                                         debug_options);
    debug_session = &session;
    if (!session.start()) {
        return 1;
    }

    const std::string module_name = std::filesystem::path(script_path).stem().string();

#ifdef GLSCRIPT_HAVE_RAYLIB
    if (headless) {
        return run_headless(runtime, world, module_name, frames, diagnostics);
    }
    return run_windowed(runtime, world, module_name, diagnostics);
#else
    if (!headless) {
        std::cout << "built without raylib; running headless\n";
    }
    return run_headless(runtime, world, module_name, frames, diagnostics);
#endif
}
