// The fps engine: glscript behaviours driving a first-person shooter.
//
// The C++ side owns the EnTT registry, a Jolt Physics world, the frame loop
// and the rendering (raylib, 3D). Every entity carries one behaviour
// instance of a script class from fps.gl; the engine:
//
//   - acts as the "prefab": it constructs instances and fills the
//     configured fields (Runtime::set_field),
//   - keeps instances alive across collections (Runtime::retain),
//   - reads the input devices and exposes them to script as effect
//     operations (axis, look) — what to do with the input is script logic,
//   - steps Jolt, syncs transforms, and routes contact events to
//     on_collision(other) on both behaviours,
//   - drives the lifecycle by dispatching on the instance's runtime class
//     (Runtime::call_method): on_start, update(dt), take_damage(amount),
//   - renders billboards read back from script fields (the enemy healthbar
//     is drawn from the behaviour's health/max_health, like the sandbox's
//     pickup spin).
//
// The script owns the game rules: firing (left click), the fork-cascade
// projectile behaviour, damage, and enemy aggro/chase.
//
// Run windowed:            ./fps_demo
// Run headless (for ctest): ./fps_demo --headless [frames]
// Debug (waits for the NSH debugger): ./fps_demo --debug[=port]

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

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef GLSCRIPT_HAVE_RAYLIB
#include "raylib.h"
#endif

namespace {

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr float kDt = 1.0f / 60.0f;
constexpr float kEyeOffset = 0.5f;     // camera height above the body center
constexpr float kMouseSensitivity = 0.003f;

constexpr JPH::uint kMaxBodies = 256;
constexpr JPH::uint kMaxBodyPairs = 1024;
constexpr JPH::uint kMaxContacts = 256;
constexpr JPH::uint kMaxJobs = 1024;

enum class Kind {
    Player,
    Enemy,
    Projectile,
    Static,
};

struct KindC {
    Kind kind = Kind::Static;
};

struct Position {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Half extents, for rendering the static arena geometry.
struct BoxC {
    float hx = 1.0f;
    float hy = 1.0f;
    float hz = 1.0f;
};

struct Name {
    std::string value;
};

struct JoltBody {
    JPH::BodyID id;
};

struct InputState {
    float forward = 0.0f;
    float strafe = 0.0f;
    float fire = 0.0f;
    float look_dx = 0.0f;
    float look_dy = 0.0f;
};

// A prefab: which script class to instantiate, the entity's body shape and
// motion, and the per-instance field overrides (the "inspector" values).
struct Prefab {
    std::string class_name;
    Kind kind = Kind::Player;
    float radius = 0.3f;        // capsule radius / projectile sphere radius
    float half_height = 0.5f;   // capsule cylinder half height
    float vx = 0.0f;
    float vy = 0.0f;
    float vz = 0.0f;
    std::vector<std::pair<std::string, glscript::Value>> overrides;
};

struct AttachedBehaviour {
    entt::entity owner = entt::null;
    glscript::Value instance;
};

struct FpsWorld {
    entt::registry registry;
    std::unordered_map<std::string, Prefab> prefabs;
    std::vector<AttachedBehaviour> behaviours;
    entt::entity player = entt::null;
    int entity_count = 0;
    int kills = 0;
    InputState input;

    // Deferred so host thunks never mutate what the frame loop iterates and
    // never re-enter the interpreter.
    struct PendingAttach {
        entt::entity owner = entt::null;
        std::string prefab;
        std::vector<std::pair<std::string, glscript::Value>> overrides;
    };
    struct PendingDamage {
        entt::entity target = entt::null;
        float amount = 0.0f;
    };
    std::vector<PendingAttach> pending_attaches;
    std::vector<PendingDamage> pending_damage;
    std::vector<entt::entity> pending_despawns;

    // Stats, for the deterministic headless run's assertions.
    int projectiles_spawned = 0;
    int damage_events = 0;
    std::vector<entt::entity> damaged_entities; // unique, in first-hit order
};

// ---------------------------------------------------------------------------
// Jolt wiring: two object layers, the usual pair filters, and a contact
// listener that only queues pairs — script calls never happen inside a Jolt
// callback.
// ---------------------------------------------------------------------------

namespace Layers {
enum ObjectLayers : JPH::uint8 {
    NonMoving = 0,
    Moving = 1,
    Count,
};
} // namespace Layers

class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        switch (layer) {
        case Layers::NonMoving:
            return JPH::BroadPhaseLayer(0);
        case Layers::Moving:
            return JPH::BroadPhaseLayer(1);
        default:
            JPH_ASSERT(false);
            return JPH::BroadPhaseLayer(0);
        }
    }

    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return static_cast<JPH::uint8>(layer) == 0 ? "static" : "moving";
    }
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override {
        return true;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return !(a == Layers::NonMoving && b == Layers::NonMoving);
    }
};

class ContactCollector final : public JPH::ContactListener {
public:
    // (entity id, entity id) pairs, deduplicated per step by the caller.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;

    JPH::ValidateResult OnContactValidate(const JPH::Body&, const JPH::Body&,
                                          JPH::RVec3Arg,
                                          const JPH::CollideShapeResult&) override {
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                        const JPH::ContactManifold&,
                        JPH::ContactSettings&) override {
        pairs.emplace_back(body1.GetUserData(), body2.GetUserData());
    }
};

struct Physics {
    BroadPhaseLayerInterface bp_layer_interface;
    ObjectVsBroadPhaseFilter obj_vs_bp_filter;
    ObjectLayerPairFilter obj_pair_filter;
    JPH::PhysicsSystem system;
    JPH::TempAllocatorMalloc temp_allocator;
    JPH::JobSystemSingleThreaded jobs{kMaxJobs};
    ContactCollector collector;

    Physics() {
        system.Init(kMaxBodies, 0, kMaxBodyPairs, kMaxContacts, bp_layer_interface,
                    obj_vs_bp_filter, obj_pair_filter);
        system.SetContactListener(&collector);
    }
};

// RAII for the Jolt global factory/type registry.
struct JoltGlobals {
    JoltGlobals() {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

    ~JoltGlobals() {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
};

JPH::BodyID add_body(Physics& physics, const Prefab& prefab, float x, float y,
                     float z, float vx, float vy, float vz,
                     std::uintptr_t entity_id) {
    JPH::RefConst<JPH::Shape> shape;
    JPH::EMotionType motion = JPH::EMotionType::Kinematic;
    JPH::ObjectLayer layer = Layers::Moving;

    switch (prefab.kind) {
    case Kind::Player:
    case Kind::Enemy:
        shape = new JPH::CapsuleShape(prefab.half_height, prefab.radius);
        break;
    case Kind::Projectile:
        shape = new JPH::SphereShape(prefab.radius);
        motion = JPH::EMotionType::Dynamic;
        break;
    case Kind::Static:
        JPH_ASSERT(false); // statics go through add_static
        shape = new JPH::BoxShape(JPH::Vec3(1.0f, 1.0f, 1.0f));
        motion = JPH::EMotionType::Static;
        layer = Layers::NonMoving;
        break;
    }

    JPH::BodyCreationSettings settings(shape, JPH::RVec3(x, y, z),
                                       JPH::Quat::sIdentity(), motion, layer);
    if (prefab.kind == Kind::Projectile) {
        settings.mGravityFactor = 0.0f; // straight flight: aiming stays honest
        settings.mLinearDamping = 0.0f;
        settings.mMotionQuality = JPH::EMotionQuality::LinearCast; // no tunneling
    }

    auto& interface = physics.system.GetBodyInterface();
    JPH::Body& body = *interface.CreateBody(settings);
    body.SetUserData(entity_id);
    interface.AddBody(body.GetID(), JPH::EActivation::Activate);
    if (vx != 0.0f || vy != 0.0f || vz != 0.0f) {
        interface.SetLinearVelocity(body.GetID(), JPH::Vec3(vx, vy, vz));
    }
    return body.GetID();
}

entt::entity add_static(FpsWorld& world, Physics& physics, std::string name,
                        float x, float y, float z, float hx, float hy, float hz) {
    const auto entity = world.registry.create();
    world.registry.emplace<Name>(entity, Name{std::move(name)});
    world.registry.emplace<KindC>(entity, KindC{Kind::Static});
    world.registry.emplace<Position>(entity, Position{x, y, z});
    world.registry.emplace<BoxC>(entity, BoxC{hx, hy, hz});

    Prefab box;
    box.kind = Kind::Static;
    const JPH::BodyID id = [&] {
        JPH::BodyCreationSettings settings(
            new JPH::BoxShape(JPH::Vec3(hx, hy, hz)), JPH::RVec3(x, y, z),
            JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::NonMoving);
        auto& interface = physics.system.GetBodyInterface();
        JPH::Body& body = *interface.CreateBody(settings);
        body.SetUserData(static_cast<std::uintptr_t>(entt::to_integral(entity)));
        interface.AddBody(body.GetID(), JPH::EActivation::DontActivate);
        return body.GetID();
    }();
    world.registry.emplace<JoltBody>(entity, JoltBody{id});
    ++world.entity_count;
    return entity;
}

entt::entity spawn_entity(FpsWorld& world, Physics& physics, const Prefab& prefab,
                          float x, float y, float z,
                          const std::vector<std::pair<std::string, glscript::Value>>&
                              overrides) {
    (void)overrides; // consumed by the caller for the behaviour attach
    const auto entity = world.registry.create();
    world.registry.emplace<Name>(entity, Name{"prefab:" + prefab.class_name});
    world.registry.emplace<KindC>(entity, KindC{prefab.kind});
    world.registry.emplace<Position>(entity, Position{x, y, z});
    const JPH::BodyID id =
        add_body(physics, prefab, x, y, z, prefab.vx, prefab.vy, prefab.vz,
                 static_cast<std::uintptr_t>(entt::to_integral(entity)));
    world.registry.emplace<JoltBody>(entity, JoltBody{id});
    ++world.entity_count;
    if (prefab.kind == Kind::Projectile) {
        ++world.projectiles_spawned;
    }
    return entity;
}

// ---------------------------------------------------------------------------
// glscript value helpers
// ---------------------------------------------------------------------------

glscript::Value make_entity_value(glscript::Runtime& runtime, entt::entity entity) {
    const auto id = static_cast<std::uintptr_t>(entt::to_integral(entity));
    return runtime.make_object(runtime.find_type("game::Entity"), {},
                               reinterpret_cast<void*>(id));
}

entt::entity entity_of(const glscript::Value& value) {
    return static_cast<entt::entity>(
        reinterpret_cast<std::uintptr_t>(as_object(value)->host));
}

// Vec2/Vec3 (same shape as the sandbox demo's bindings, extended to 3D).
bool is_vec(const glscript::Value& value, const char* type_name) {
    return value.is_heap() && value.as_heap()->kind == glscript::GcObject::Kind::Object &&
           as_object(value)->type->name == type_name;
}

double vec_field(const glscript::Value& value, std::size_t index) {
    return as_number(as_object(value)->fields[index]);
}

glscript::Value make_vec2(glscript::Runtime& runtime, double x, double y) {
    return runtime.make_object(runtime.find_type("game::Vec2"),
                               {glscript::Value::floating(x),
                                glscript::Value::floating(y)});
}

glscript::Value make_vec3(glscript::Runtime& runtime, double x, double y, double z) {
    return runtime.make_object(runtime.find_type("game::Vec3"),
                               {glscript::Value::floating(x),
                                glscript::Value::floating(y),
                                glscript::Value::floating(z)});
}

struct Vec3Values {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Vec3Values vec3_of(const glscript::Value& value) {
    return Vec3Values{vec_field(value, 0), vec_field(value, 1), vec_field(value, 2)};
}

// Field values for a fresh behaviour instance: the overrides win, everything
// else gets a type default. Declaration order == slot order.
std::vector<glscript::Value> initial_fields(
    glscript::Runtime& runtime, const glscript::TypeSymbol& type,
    const std::vector<std::pair<std::string, glscript::Value>>& overrides) {
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
        for (const auto& [name, value] : overrides) {
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

// Constructs the script class instance for an entity and attaches it. The
// instance is retained: the host holds it, so it must outlive GC cycles.
void attach_behaviour(glscript::Runtime& runtime, FpsWorld& world,
                      const glscript::Program& program, const std::string& module_name,
                      entt::entity owner, const std::string& class_name,
                      const std::vector<std::pair<std::string, glscript::Value>>&
                          overrides) {
    auto* module = program.find_module(module_name);
    auto* type = module != nullptr && module->types.contains(class_name)
                     ? module->types.at(class_name)
                     : nullptr;
    if (type == nullptr) {
        std::cerr << "fps: script class '" << class_name << "' not found\n";
        return;
    }
    auto instance = runtime.make_object(type, initial_fields(runtime, *type, overrides));
    runtime.set_field(instance, "entity", make_entity_value(runtime, owner));
    runtime.retain(instance);
    world.behaviours.push_back(AttachedBehaviour{owner, std::move(instance)});
}

AttachedBehaviour* find_behaviour(FpsWorld& world, entt::entity owner) {
    for (auto& behaviour : world.behaviours) {
        if (behaviour.owner == owner) {
            return &behaviour;
        }
    }
    return nullptr;
}

bool has_method(const glscript::Value& instance, const std::string& name) {
    return as_object(instance)->type->methods.contains(name);
}

// The camera orientation is script state: the PlayerBehaviour's yaw/pitch
// fields, updated by the script from the look() op, read back here.
struct Aim {
    float yaw = 0.0f;
    float pitch = 0.0f;
};

Aim aim_of(glscript::Runtime& runtime, FpsWorld& world) {
    Aim aim;
    if (world.player == entt::null) {
        return aim;
    }
    if (auto* behaviour = find_behaviour(world, world.player);
        behaviour != nullptr) {
        aim.yaw = static_cast<float>(as_number(runtime.field(behaviour->instance, "yaw")));
        aim.pitch =
            static_cast<float>(as_number(runtime.field(behaviour->instance, "pitch")));
    }
    return aim;
}

// forward = (cos(pitch) sin(yaw), sin(pitch), -cos(pitch) cos(yaw));
// yaw 0 looks down -Z. Turning right (mouse right) increases yaw.
struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3f aim_forward(const Aim& aim) {
    const float cp = std::cos(aim.pitch);
    return Vec3f{cp * std::sin(aim.yaw), std::sin(aim.pitch), -cp * std::cos(aim.yaw)};
}

Vec3f aim_right(const Aim& aim) {
    return Vec3f{std::cos(aim.yaw), 0.0f, std::sin(aim.yaw)};
}

// One frame: step the physics, route contact events, update behaviours, then
// apply the deferred spawns, damage and despawns. call_method is only ever
// called from here — never from inside a host thunk — because the
// interpreter is not re-entrant.
bool update_frame(glscript::Runtime& runtime, FpsWorld& world, Physics& physics,
                  const glscript::Program& program, const std::string& module_name,
                  float dt, glscript::DiagnosticSink& diagnostics) {
    // Physics step, then sync transforms into the registry.
    const JPH::EPhysicsUpdateError update_error =
        physics.system.Update(dt, 1, &physics.temp_allocator, &physics.jobs);
    if (update_error != JPH::EPhysicsUpdateError::None) {
        std::cerr << "fps: physics update error " << static_cast<int>(update_error)
                  << "\n";
    }
    auto& interface = physics.system.GetBodyInterface();
    for (auto [entity, position, body] :
         world.registry.view<Position, JoltBody>().each()) {
        (void)entity;
        const JPH::RVec3 p = interface.GetPosition(body.id);
        position.x = static_cast<float>(p.GetX());
        position.y = static_cast<float>(p.GetY());
        position.z = static_cast<float>(p.GetZ());
    }

    // Contact routing: new contact pairs this step get on_collision(other)
    // on both behaviours (like the sandbox demo's collision pass, but fed by
    // Jolt's contact listener). Projectiles are the only script class that
    // defines on_collision here.
    std::set<std::pair<std::uint64_t, std::uint64_t>> unique;
    for (const auto& pair : physics.collector.pairs) {
        unique.insert(pair.first < pair.second ? pair : std::make_pair(pair.second, pair.first));
    }
    physics.collector.pairs.clear();
    for (const auto& [id_a, id_b] : unique) {
        const auto a = static_cast<entt::entity>(id_a);
        const auto b = static_cast<entt::entity>(id_b);
        if (!world.registry.valid(a) || !world.registry.valid(b)) {
            continue; // a body may have despawned earlier in this pass
        }
        for (const auto owner : {a, b}) {
            auto* behaviour = find_behaviour(world, owner);
            if (behaviour == nullptr || !has_method(behaviour->instance, "on_collision")) {
                continue;
            }
            const entt::entity other = owner == a ? b : a;
            runtime.call_method(behaviour->instance, "on_collision",
                                {make_entity_value(runtime, other)});
            if (diagnostics.has_errors()) {
                std::cerr << diagnostics.format();
                return false;
            }
        }
    }

    // Script lifecycle: update(dt) on every attached behaviour.
    for (auto& behaviour : world.behaviours) {
        if (!world.registry.valid(behaviour.owner)) {
            continue;
        }
        runtime.call_method(behaviour.instance, "update",
                            {glscript::Value::floating(dt)});
        if (diagnostics.has_errors()) {
            std::cerr << diagnostics.format();
            return false;
        }
    }

    // Deferred spawns: attach the behaviour now, outside any script call.
    for (const auto& pending : world.pending_attaches) {
        attach_behaviour(runtime, world, program, module_name, pending.owner,
                         pending.prefab, pending.overrides);
        auto* behaviour = find_behaviour(world, pending.owner);
        if (behaviour != nullptr && has_method(behaviour->instance, "on_start")) {
            runtime.call_method(behaviour->instance, "on_start");
            if (diagnostics.has_errors()) {
                std::cerr << diagnostics.format();
                return false;
            }
        }
    }
    world.pending_attaches.clear();

    // Deferred damage: the script raised Combat::damage intents; apply them
    // by dispatching take_damage(amount) on the target's behaviour.
    auto damage = std::move(world.pending_damage);
    world.pending_damage.clear();
    for (const auto& hit : damage) {
        if (!world.registry.valid(hit.target)) {
            continue;
        }
        auto* behaviour = find_behaviour(world, hit.target);
        if (behaviour == nullptr || !has_method(behaviour->instance, "take_damage")) {
            continue;
        }
        runtime.call_method(behaviour->instance, "take_damage",
                            {glscript::Value::floating(hit.amount)});
        if (diagnostics.has_errors()) {
            std::cerr << diagnostics.format();
            return false;
        }
    }

    // Deferred despawns: destroy the entity *and* its Jolt body.
    while (!world.pending_despawns.empty()) {
        const auto entity = world.pending_despawns.back();
        world.pending_despawns.pop_back();
        if (!world.registry.valid(entity)) {
            continue;
        }
        if (auto* body = world.registry.try_get<JoltBody>(entity);
            body != nullptr) {
            interface.RemoveBody(body->id);
            interface.DestroyBody(body->id);
        }
        world.registry.destroy(entity);
        --world.entity_count;
        std::erase_if(world.behaviours, [&entity](const AttachedBehaviour& behaviour) {
            return behaviour.owner == entity;
        });
    }

    return true;
}

void register_bindings(glscript::HostRegistry& host, FpsWorld& world,
                       Physics& physics) {
    using namespace glscript;

    auto game = host.module("game");

    auto entity_type = game.class_type("Entity").global();
    {
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
    }

    {
        auto vec3 = game.class_type("Vec3");
        vec3.field("x", TypeRef::floating());
        vec3.field("y", TypeRef::floating());
        vec3.field("z", TypeRef::floating());
        vec3.static_method("new", TypeRef::named("Vec3"))
            .parameter("x", TypeRef::floating())
            .parameter("y", TypeRef::floating())
            .parameter("z", TypeRef::floating())
            .call([](Runtime& runtime, std::vector<Value>& args) {
                if (args.size() != 3) {
                    return Value::unit();
                }
                return make_vec3(runtime, as_number(args[0]), as_number(args[1]),
                                 as_number(args[2]));
            });
        vec3.method("length", TypeRef::floating())
            .call([](Runtime& runtime, std::vector<Value>& args) {
                (void)runtime;
                if (args.empty() || !is_vec(args[0], "Vec3")) {
                    return Value::unit();
                }
                const auto v = vec3_of(args[0]);
                return Value::floating(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
            });
        vec3.method("normalized", TypeRef::named("Vec3"))
            .call([](Runtime& runtime, std::vector<Value>& args) {
                if (args.empty() || !is_vec(args[0], "Vec3")) {
                    return Value::unit();
                }
                const auto v = vec3_of(args[0]);
                const double length =
                    std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                if (length < 1.0e-9) {
                    return make_vec3(runtime, 0.0, 0.0, 0.0);
                }
                return make_vec3(runtime, v.x / length, v.y / length, v.z / length);
            });
        vec3.binary_operator("+", TypeRef::named("game::Vec3"),
                             TypeRef::named("game::Vec3"), TypeRef::named("game::Vec3"))
            .call([](Runtime& runtime, std::vector<Value>& args) {
                if (args.size() != 2 || !is_vec(args[0], "Vec3") || !is_vec(args[1], "Vec3")) {
                    return Value::unit();
                }
                const auto a = vec3_of(args[0]);
                const auto b = vec3_of(args[1]);
                return make_vec3(runtime, a.x + b.x, a.y + b.y, a.z + b.z);
            });
        vec3.binary_operator("-", TypeRef::named("game::Vec3"),
                             TypeRef::named("game::Vec3"), TypeRef::named("game::Vec3"))
            .call([](Runtime& runtime, std::vector<Value>& args) {
                if (args.size() != 2 || !is_vec(args[0], "Vec3") || !is_vec(args[1], "Vec3")) {
                    return Value::unit();
                }
                const auto a = vec3_of(args[0]);
                const auto b = vec3_of(args[1]);
                return make_vec3(runtime, a.x - b.x, a.y - b.y, a.z - b.z);
            });
        vec3.binary_operator("*", TypeRef::named("game::Vec3"), TypeRef::floating(),
                             TypeRef::named("game::Vec3"))
            .call([](Runtime& runtime, std::vector<Value>& args) {
                if (args.size() != 2 || !is_vec(args[0], "Vec3")) {
                    return Value::unit();
                }
                const auto v = vec3_of(args[0]);
                const double scalar = as_number(args[1]);
                return make_vec3(runtime, v.x * scalar, v.y * scalar, v.z * scalar);
            });
    }

    {
        auto entity = entity_type;
        entity.method("position", TypeRef::named("game::Vec3"))
            .call([&world](Runtime& runtime, std::vector<Value>& args) {
                if (args.empty() || !world.registry.valid(entity_of(args[0]))) {
                    return Value::unit();
                }
                const auto& position = world.registry.get<Position>(entity_of(args[0]));
                return make_vec3(runtime, position.x, position.y, position.z);
            });
        entity.method("set_velocity", TypeRef::unit())
            .parameter("velocity", TypeRef::named("game::Vec3"))
            .call([&world, &physics](Runtime& runtime, std::vector<Value>& args) {
                (void)runtime;
                if (args.size() != 2 || !is_vec(args[1], "Vec3")) {
                    return Value::unit();
                }
                const auto target = entity_of(args[0]);
                if (!world.registry.valid(target)) {
                    return Value::unit();
                }
                auto* body = world.registry.try_get<JoltBody>(target);
                if (body == nullptr) {
                    return Value::unit();
                }
                const auto v = vec3_of(args[1]);
                auto& interface = physics.system.GetBodyInterface();
                interface.ActivateBody(body->id);
                interface.SetLinearVelocity(
                    body->id, JPH::Vec3(static_cast<float>(v.x),
                                        static_cast<float>(v.y),
                                        static_cast<float>(v.z)));
                return Value::unit();
            });
        // Entity values are freshly constructed per call, so script-side
        // identity checks (the fork targeting excludes the entity just hit)
        // go through host-registered comparison operators on the id.
        entity.binary_operator("==", TypeRef::named("game::Entity"),
                               TypeRef::named("game::Entity"), TypeRef::boolean())
            .call([](Runtime&, std::vector<Value>& args) {
                if (args.size() != 2) {
                    return Value::boolean(false);
                }
                return Value::boolean(entity_of(args[0]) == entity_of(args[1]));
            });
        entity.binary_operator("!=", TypeRef::named("game::Entity"),
                               TypeRef::named("game::Entity"), TypeRef::boolean())
            .call([](Runtime&, std::vector<Value>& args) {
                if (args.size() != 2) {
                    return Value::boolean(false);
                }
                return Value::boolean(entity_of(args[0]) != entity_of(args[1]));
            });
    }

    // Script -> engine services, exposed as global effect operations.
    auto input = host.module("app").effect("Input").global();
    input.operation("axis", TypeRef::floating())
        .parameter("name", TypeRef::string())
        .call([&world](Runtime& runtime, std::vector<Value>& args) {
            (void)runtime;
            if (args.size() != 1) {
                return Value::floating(0.0);
            }
            const auto name = runtime.stringify(args[0]);
            if (name == "forward") {
                return Value::floating(world.input.forward);
            }
            if (name == "strafe") {
                return Value::floating(world.input.strafe);
            }
            if (name == "fire") {
                return Value::floating(world.input.fire);
            }
            return Value::floating(0.0);
        });
    input.operation("look", TypeRef::named("game::Vec2"))
        .call([&world](Runtime& runtime, std::vector<Value>& args) {
            (void)args;
            return make_vec2(runtime, world.input.look_dx, world.input.look_dy);
        });

    auto world_module = host.module("app").effect("World").global();
    world_module.operation("player", TypeRef::named("game::Entity"))
        .call([&world](Runtime& runtime, std::vector<Value>& args) {
            (void)args;
            return make_entity_value(runtime, world.player);
        });
    world_module.operation("enemies",
                           TypeRef::named("array", {TypeRef::named("game::Entity")}))
        .call([&world](Runtime& runtime, std::vector<Value>& args) {
            (void)args;
            std::vector<Value> enemies;
            for (auto [entity, kind] :
                 world.registry.view<KindC>().each()) {
                if (kind.kind == Kind::Enemy && world.registry.valid(entity)) {
                    enemies.push_back(make_entity_value(runtime, entity));
                }
            }
            return runtime.make_array(std::move(enemies));
        });
    world_module.operation("spawn", TypeRef::named("game::Entity"))
        .parameter("prefab", TypeRef::string())
        .parameter("pos", TypeRef::named("game::Vec3"))
        .parameter("vel", TypeRef::named("game::Vec3"))
        .parameter("forks", TypeRef::integer())
        .call([&world, &physics](Runtime& runtime, std::vector<Value>& args) {
            if (args.size() != 4 || !is_vec(args[1], "Vec3") || !is_vec(args[2], "Vec3")) {
                return Value::unit();
            }
            const auto it = world.prefabs.find(runtime.stringify(args[0]));
            if (it == world.prefabs.end()) {
                return Value::unit();
            }
            const auto pos = vec3_of(args[1]);
            const auto vel = vec3_of(args[2]);
            Prefab prefab = it->second;
            prefab.vx = static_cast<float>(vel.x);
            prefab.vy = static_cast<float>(vel.y);
            prefab.vz = static_cast<float>(vel.z);
            // Per-spawn field overrides win over the prefab's own.
            prefab.overrides.push_back({"forks", args[3]});
            const auto entity = spawn_entity(world, physics, prefab,
                                             static_cast<float>(pos.x),
                                             static_cast<float>(pos.y),
                                             static_cast<float>(pos.z),
                                             prefab.overrides);
            world.pending_attaches.push_back({entity, it->second.class_name,
                                              prefab.overrides});
            return make_entity_value(runtime, entity);
        });
    world_module.operation("kind", TypeRef::string())
        .parameter("entity", TypeRef::named("game::Entity"))
        .call([&world](Runtime& runtime, std::vector<Value>& args) {
            if (args.empty() || !world.registry.valid(entity_of(args[0]))) {
                return runtime.make_string("");
            }
            switch (world.registry.get<KindC>(entity_of(args[0])).kind) {
            case Kind::Player:
                return runtime.make_string("player");
            case Kind::Enemy:
                return runtime.make_string("enemy");
            case Kind::Projectile:
                return runtime.make_string("projectile");
            case Kind::Static:
                return runtime.make_string("static");
            }
            return runtime.make_string("");
        });
    world_module.operation("camera_forward", TypeRef::named("game::Vec3"))
        .call([&world, &physics](Runtime& runtime, std::vector<Value>& args) {
            (void)physics;
            (void)args;
            const Aim aim = aim_of(runtime, world);
            const Vec3f forward = aim_forward(aim);
            return make_vec3(runtime, forward.x, forward.y, forward.z);
        });
    world_module.operation("camera_right", TypeRef::named("game::Vec3"))
        .call([&world](Runtime& runtime, std::vector<Value>& args) {
            (void)args;
            const Aim aim = aim_of(runtime, world);
            const Vec3f right = aim_right(aim);
            return make_vec3(runtime, right.x, right.y, right.z);
        });
    world_module.operation("despawn", TypeRef::unit())
        .parameter("entity", TypeRef::named("game::Entity"))
        .call([&world](Runtime& runtime, std::vector<Value>& args) {
            (void)runtime;
            if (args.size() == 1 && args[0].is_heap() &&
                args[0].as_heap()->kind == GcObject::Kind::Object) {
                world.pending_despawns.push_back(entity_of(args[0]));
            }
            return Value::unit();
        });
    world_module.operation("add_kill", TypeRef::unit())
        .call([&world](Runtime& runtime, std::vector<Value>& args) {
            (void)runtime;
            (void)args;
            ++world.kills;
            return Value::unit();
        });

    auto combat = host.module("app").effect("Combat").global();
    combat.operation("damage", TypeRef::unit())
        .parameter("target", TypeRef::named("game::Entity"))
        .parameter("amount", TypeRef::floating())
        .call([&world](Runtime& runtime, std::vector<Value>& args) {
            (void)runtime;
            if (args.size() == 2 && args[0].is_heap() &&
                args[0].as_heap()->kind == GcObject::Kind::Object) {
                const auto target = entity_of(args[0]);
                world.pending_damage.push_back(
                    {target, static_cast<float>(as_number(args[1]))});
                ++world.damage_events;
                if (std::find(world.damaged_entities.begin(),
                              world.damaged_entities.end(), target) ==
                    world.damaged_entities.end()) {
                    world.damaged_entities.push_back(target);
                }
            }
            return Value::unit();
        });
}

#ifdef GLSCRIPT_HAVE_RAYLIB
void draw_healthbars(glscript::Runtime& runtime, FpsWorld& world, const Camera3D& camera,
                     const Vec3f& forward) {
    for (const auto& behaviour : world.behaviours) {
        if (!world.registry.valid(behaviour.owner)) {
            continue;
        }
        const auto& kind = world.registry.get<KindC>(behaviour.owner);
        if (kind.kind != Kind::Enemy) {
            continue;
        }
        const double health = as_number(runtime.field(behaviour.instance, "health"));
        const double max_health =
            as_number(runtime.field(behaviour.instance, "max_health"));
        if (max_health <= 0.0 || health <= 0.0) {
            continue;
        }
        const auto& position = world.registry.get<Position>(behaviour.owner);
        const Vector3 top{position.x, position.y + 0.95f, position.z};

        // Skip enemies behind the camera (GetWorldToScreen mirrors them).
        const Vector3 to_enemy{top.x - camera.position.x, top.y - camera.position.y,
                               top.z - camera.position.z};
        if (to_enemy.x * forward.x + to_enemy.y * forward.y + to_enemy.z * forward.z <=
            0.0f) {
            continue;
        }
        const Vector2 screen = GetWorldToScreen(top, camera);
        constexpr float kBarWidth = 52.0f;
        constexpr float kBarHeight = 7.0f;
        const float fraction =
            static_cast<float>(health / max_health);
        DrawRectangle(static_cast<int>(screen.x - kBarWidth / 2.0f),
                      static_cast<int>(screen.y), static_cast<int>(kBarWidth),
                      static_cast<int>(kBarHeight), Color{0, 0, 0, 160});
        DrawRectangle(static_cast<int>(screen.x - kBarWidth / 2.0f),
                      static_cast<int>(screen.y),
                      static_cast<int>(kBarWidth * fraction),
                      static_cast<int>(kBarHeight), Color{240, 84, 84, 230});
    }
}

int run_windowed(glscript::Runtime& runtime, FpsWorld& world, Physics& physics,
                 const glscript::Program& program, const std::string& module_name,
                 glscript::DiagnosticSink& diagnostics) {
    InitWindow(kWindowWidth, kWindowHeight,
               "glscript fps - fork-cascade projectiles over Jolt physics");
    SetTargetFPS(60);
    DisableCursor();

    Camera3D camera{};
    camera.up = Vector3{0.0f, 1.0f, 0.0f};
    camera.fovy = 70.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    while (!WindowShouldClose()) {
        // Devices -> InputState (the script reads it via the axis/look ops).
        const Vector2 mouse = GetMouseDelta();
        world.input.look_dx = mouse.x * kMouseSensitivity;
        world.input.look_dy = -mouse.y * kMouseSensitivity;
        world.input.forward =
            (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP) ? 1.0f : 0.0f) -
            (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN) ? 1.0f : 0.0f);
        world.input.strafe =
            (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT) ? 1.0f : 0.0f) -
            (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT) ? 1.0f : 0.0f);
        world.input.fire = IsMouseButtonDown(MOUSE_BUTTON_LEFT) ? 1.0f : 0.0f;

        float dt = GetFrameTime();
        if (dt > kDt) {
            dt = kDt;
        }
        if (!update_frame(runtime, world, physics, program, module_name, dt,
                          diagnostics)) {
            break;
        }

        // Camera follows the player body; orientation is script state.
        const auto& player_position =
            world.registry.valid(world.player)
                ? world.registry.get<Position>(world.player)
                : Position{};
        const Aim aim = aim_of(runtime, world);
        const Vec3f forward = aim_forward(aim);
        camera.position =
            Vector3{player_position.x, player_position.y + kEyeOffset, player_position.z};
        camera.target = Vector3{camera.position.x + forward.x,
                                camera.position.y + forward.y,
                                camera.position.z + forward.z};

        BeginDrawing();
        ClearBackground(Color{24, 28, 38, 255});
        BeginMode3D(camera);

        DrawGrid(60, 1.0f);
        for (auto [entity, position, box] :
             world.registry.view<Position, BoxC>().each()) {
            (void)entity;
            DrawCube(Vector3{position.x, position.y, position.z}, box.hx * 2.0f,
                     box.hy * 2.0f, box.hz * 2.0f, Color{70, 78, 96, 255});
            DrawCubeWires(Vector3{position.x, position.y, position.z},
                          box.hx * 2.0f, box.hy * 2.0f, box.hz * 2.0f,
                          Color{110, 120, 140, 255});
        }
        for (auto [entity, position, kind] :
             world.registry.view<Position, KindC>().each()) {
            switch (kind.kind) {
            case Kind::Enemy:
                // The capsule shape's sphere centers are half_height apart.
                DrawCapsule(Vector3{position.x, position.y - 0.5f, position.z},
                            Vector3{position.x, position.y + 0.5f, position.z},
                            0.3f, 4, 8, Color{214, 80, 80, 255});
                break;
            case Kind::Projectile:
                DrawSphere(Vector3{position.x, position.y, position.z}, 0.15f,
                           Color{250, 176, 84, 255});
                break;
            default:
                break;
            }
        }
        EndMode3D();

        // Script state drives the UI: the healthbar reads the enemy
        // behaviour's health/max_health fields.
        draw_healthbars(runtime, world, camera, forward);

        // Crosshair.
        const float cx = kWindowWidth / 2.0f;
        const float cy = kWindowHeight / 2.0f;
        DrawLineV(Vector2{cx - 9.0f, cy}, Vector2{cx + 9.0f, cy},
                  Color{235, 238, 245, 200});
        DrawLineV(Vector2{cx, cy - 9.0f}, Vector2{cx, cy + 9.0f},
                  Color{235, 238, 245, 200});

        DrawText("glscript fps - left click: shoot (forks chase other enemies) - wasd + mouse look",
                 12, 10, 15, Color{226, 229, 238, 255});
        DrawText(TextFormat("kills: %d    entities: %zu    behaviours: %zu    fps: %d",
                            world.kills, static_cast<std::size_t>(world.entity_count),
                            world.behaviours.size(), GetFPS()),
                 12, 30, 13, Color{150, 155, 170, 255});
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
#endif // GLSCRIPT_HAVE_RAYLIB

// Deterministic headless run: the player stands still facing enemy #1
// (yaw 0 = down -Z) and fires three times. Each hit forks into projectiles
// aimed at the nearest other enemies, so one trigger pull cascades.
int run_headless(glscript::Runtime& runtime, FpsWorld& world, Physics& physics,
                 const glscript::Program& program, const std::string& module_name,
                 int frames, glscript::DiagnosticSink& diagnostics) {
    int fired = 0;
    for (int frame = 0; frame < frames; ++frame) {
        world.input.forward = 0.0f;
        world.input.strafe = 0.0f;
        world.input.look_dx = 0.0f;
        world.input.look_dy = 0.0f;
        world.input.fire = (frame == 30 || frame == 90 || frame == 150) ? 1.0f : 0.0f;
        if (world.input.fire > 0.0f) {
            ++fired;
        }
        if (!update_frame(runtime, world, physics, program, module_name, kDt,
                          diagnostics)) {
            return 1;
        }
    }

    std::cout << "headless fps: " << fired << " shots, " << world.projectiles_spawned
              << " projectiles spawned (forks included), " << world.damage_events
              << " damage events on " << world.damaged_entities.size()
              << " distinct enemies, " << world.kills << " kills, "
              << static_cast<std::size_t>(world.entity_count) << " entities\n";

    bool failed = false;
    if (world.projectiles_spawned < 7) {
        std::cerr << "headless fps: expected the fork cascade to spawn >= 7 projectiles"
                     " (1 direct + 2 forks + 4 forks)\n";
        failed = true;
    }
    if (world.damaged_entities.size() < 3) {
        std::cerr << "headless fps: expected >= 3 distinct enemies damaged by the"
                     " cascade\n";
        failed = true;
    }
    if (world.damage_events < 5) {
        std::cerr << "headless fps: expected >= 5 damage events\n";
        failed = true;
    }
    if (world.kills < 1) {
        std::cerr << "headless fps: expected enemy #1 to die to the three direct hits\n";
        failed = true;
    }
    if (failed) {
        return 1;
    }
    std::cout << "fork cascade ran end to end: scripted firing, Jolt contact"
                 " events, script-side forking and damage\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
#ifdef GLSCRIPT_FPS_SCRIPT
    std::string script_path = GLSCRIPT_FPS_SCRIPT;
#else
    std::string script_path = "examples/fps/fps.gl";
#endif

    bool headless = false;
    int frames = 400;
    glscript::demo::DebugOptions debug_options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        std::string error;
        switch (glscript::demo::parse_debug_argument(argument, debug_options, error)) {
        case glscript::demo::DebugArgResult::Parsed:
            continue;
        case glscript::demo::DebugArgResult::Invalid:
            std::cerr << "fps_demo: " << error << '\n';
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

    const std::string module_name =
        std::filesystem::path(script_path).stem().string();

    JoltGlobals jolt;
    Physics physics;
    FpsWorld world;

    world.prefabs["projectile"] = Prefab{
        "ProjectileBehaviour", Kind::Projectile, 0.15f, 0.0f, 0.0f, 0.0f, 0.0f,
        {{"forks", glscript::Value::integer(2)},
         {"power", glscript::Value::floating(1.0)},
         {"speed", glscript::Value::floating(26.0)},
         {"life", glscript::Value::floating(3.0)}}};
    world.prefabs["enemy"] = Prefab{
        "EnemyBehaviour", Kind::Enemy, 0.3f, 0.5f, 0.0f, 0.0f, 0.0f,
        {{"health", glscript::Value::floating(3.0)},
         {"max_health", glscript::Value::floating(3.0)},
         {"speed", glscript::Value::floating(0.6)}}};

    glscript::HostRegistry host;
    glscript::demo::DebugSession* debug_session = nullptr;
    glscript::register_engine_bindings(host, [&](const std::string& line) {
        std::cout << line << '\n';
        if (debug_session != nullptr) {
            debug_session->emit_output(line + '\n');
        }
    });
    register_bindings(host, world, physics);

    glscript::DiagnosticSink diagnostics;
    glscript::Frontend frontend(host, diagnostics);
    if (!frontend.check_file(script_path)) {
        std::cerr << diagnostics.format();
        return 1;
    }

    const auto& program = frontend.program();
    auto bytecode = glscript::compile_program(program);
    glscript::Runtime runtime(program, bytecode, host, diagnostics);

    glscript::demo::DebugSession session(runtime, program, bytecode, debug_options);
    debug_session = &session;
    if (!session.start()) {
        return 1;
    }

    // The arena: floor, walls, pillars (static bodies + renderable boxes).
    add_static(world, physics, "floor", 0.0f, -0.5f, 0.0f, 30.0f, 0.5f, 30.0f);
    add_static(world, physics, "wall-north", 0.0f, 2.0f, -30.0f, 30.0f, 2.0f, 1.0f);
    add_static(world, physics, "wall-south", 0.0f, 2.0f, 30.0f, 30.0f, 2.0f, 1.0f);
    add_static(world, physics, "wall-west", -30.0f, 2.0f, 0.0f, 1.0f, 2.0f, 30.0f);
    add_static(world, physics, "wall-east", 30.0f, 2.0f, 0.0f, 1.0f, 2.0f, 30.0f);
    add_static(world, physics, "pillar-west", -5.0f, 1.0f, 2.0f, 0.7f, 1.0f, 0.7f);
    add_static(world, physics, "pillar-east", 5.0f, 1.0f, 2.0f, 0.7f, 1.0f, 0.7f);

    // The player: an entity plus its behaviour, the engine acting as prefab.
    const Prefab player_prefab{
        "PlayerBehaviour", Kind::Player, 0.3f, 0.5f, 0.0f, 0.0f, 0.0f,
        {{"speed", glscript::Value::floating(6.0)},
         {"cooldown", glscript::Value::floating(0.25)}}};
    world.prefabs["player"] = player_prefab;
    world.player = spawn_entity(world, physics, player_prefab, 0.0f, 0.8f, 12.0f,
                                player_prefab.overrides);
    attach_behaviour(runtime, world, program, module_name, world.player,
                     player_prefab.class_name, player_prefab.overrides);

    // Five enemies, deterministically placed in the open. Enemy #1 sits
    // straight ahead of the player's spawn (yaw 0 looks down -Z).
    const auto spawn_enemy = [&](float x, float z) {
        const auto& prefab = world.prefabs.at("enemy");
        const auto entity =
            spawn_entity(world, physics, prefab, x, 0.8f, z, prefab.overrides);
        attach_behaviour(runtime, world, program, module_name, entity,
                         prefab.class_name, prefab.overrides);
        return entity;
    };
    spawn_enemy(0.0f, -16.0f);
    spawn_enemy(-8.0f, -6.0f);
    spawn_enemy(8.0f, -6.0f);
    spawn_enemy(-8.0f, -18.0f);
    spawn_enemy(8.0f, -18.0f);

    // Lifecycle start for everything attached so far.
    for (const auto& behaviour : world.behaviours) {
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
        return run_headless(runtime, world, physics, program, module_name, frames,
                            diagnostics);
    }
    return run_windowed(runtime, world, physics, program, module_name, diagnostics);
#else
    if (!headless) {
        std::cout << "built without raylib; running headless\n";
    }
    return run_headless(runtime, world, physics, program, module_name, frames,
                        diagnostics);
#endif
}
