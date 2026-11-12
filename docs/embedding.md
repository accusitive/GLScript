# Embedding glscript: the Unity–C# model

glscript embeds into a C++ host the way C# embeds into Unity: the engine owns
the world and the frame loop, scripts own behaviour, and the two sides talk
across a small, explicit boundary. This guide walks through the patterns,
using the working examples in the repo.

| Example | What it shows |
|---|---|
| `examples/sandbox/` | Behaviour classes, lifecycle dispatch, prefabs, spawn-from-script |
| `examples/entt/` | Per-entity scripted physics with EnTT + raylib |
| `examples/fps/` | First-person demo over Jolt Physics + raylib: input ops, contact-event routing, fork-cascade projectiles, damage intents, healthbar field readback |
| `examples/effects/wave.gl` | Algebraic effects: one script, many rulebooks |

## 1. Register the host world (the "engine API")

Script sees the host through modules of types and *global effects*. Global
effect operations are lifted as ambient callables — `spawn(...)` in script is
a host thunk in C++:

```cpp
// C++ (examples/sandbox/sandbox_demo.cpp)
auto world = host.module("app").effect("World").global();
world.operation("spawn", TypeRef::named("Entity"))
    .parameter("prefab", TypeRef::string())
    .parameter("x", TypeRef::floating())
    .parameter("y", TypeRef::floating())
    .call([&sandbox](Runtime& runtime, std::vector<Value>& args) {
        const auto entity = spawn_entity(sandbox, ...);
        sandbox.pending_attaches.push_back({entity, prefab_name});
        return make_entity_value(runtime, entity);
    });
```

```glscript
// script — the same call, ambient (no import needed for global effects)
spawn("bullet", p.x, p.y - 16.0f);
```

`Value` is the script heap object (string/int/float/bool/unit or a heap
object: `ObjectInstance`, `EnumObj`, `ClosureObj`, `ContinuationObj`).
Object instances carry an opaque `host` pointer — the sandbox stores the
`entt::entity` id there so script-side `Entity` values map straight onto
registry handles.

## 2. Behaviours are script classes

```glscript
class PlayerBehaviour extends EntityBehaviour {
    // prefab-configured: the engine fills these per instance
    float speed = 0.0f;
    float cooldown = 0.0f;
    // runtime state
    float shoot_timer = 0.0f;
    Entity entity;

    function on_start() { ... }
    function update(dt: float) { ... }
    function on_collision(other: Entity) { ... }
}
```

The base class can be any host type; the lifecycle method names are a
convention between engine and script, checked with
`type->methods.contains(name)`.

## 3. The host is the prefab

The engine constructs each behaviour instance and fills its fields — the
"inspector values" live on the C++ side, per instance:

```cpp
auto* type = program.find_module("sandbox")->types.at("PlayerBehaviour");
auto instance = runtime.make_object(type, initial_fields); // declaration order
runtime.set_field(instance, "entity", entity_value);       // back-reference
runtime.retain(instance);                                  // see §6
```

`Runtime::field` / `Runtime::set_field` read and write by name (they walk
the base chain for the absolute slot). The sandbox's collision renderer even
reads script state back — the pickups rotate by their `spin` field, which
the *script* updates every frame.

## 4. The engine drives the lifecycle

`Runtime::call_method` dispatches on the instance's **runtime type** (walking
the base chain), so two classes with the same method get their own bodies —
this is the virtual-dispatch story, provided by the host, exactly like
Unity's native engine calling into Mono:

```cpp
runtime.call_method(behaviour.instance, "update", {Value::floating(dt)});
runtime.call_method(behaviour.instance, "on_collision", {other_entity});
```

The receiver is argument slot 0, matching compiled method calls.

> **Re-entrancy**: `run()` and `call_method()` are *entry points*. The
> interpreter loop is not re-entrant, so host thunks invoked from a running
> script must not call back into it. The sandbox defers such work: `spawn`
> queues the attach, and `on_start` runs after the current script call
> finishes.

## 5. Instantiation from script

`spawn("bullet", x, y)` creates the entity (C++ components), queues the
behaviour attach, and returns an `Entity` handle to the script. The next
deferred pass constructs the script instance, sets the prefab fields, and
runs `on_start`. Script-driven spawning therefore never re-enters the
interpreter.

## 6. GC: the host must retain what it holds

The collector scans the machine state and the Runtime's own tables — a
behaviour instance held only in a C++ vector is invisible to it. Anything
the host keeps across frames must be rooted:

```cpp
runtime.retain(instance); // roots the value for the Runtime's lifetime
```

## 7. Effects where they shine

Game-rule logic is a natural fit for algebraic effects: the script raises
*intents* (`Combat::damage(target, amount) -> float`) and handler stacks
reinterpret them — armor halves the blow, god mode lands nothing, narration
is re-routed — all without touching the logic. See
`examples/effects/wave.gl` for six rulebooks over one battle script.

## Run it

```sh
cmake --build build -j
./build/sandbox_demo            # windowed (raylib)
./build/sandbox_demo --headless 300   # deterministic, used by ctest
./build/fps_demo                # first-person: Jolt physics + raylib, fork-cascade projectiles
./build/fps_demo --headless 400       # deterministic, used by ctest
ctest --test-dir build          # unit tests + all demos' headless runs
```
