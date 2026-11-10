# glscript

An embeddable, statically typed scripting language for game engines. The front
end is written in C++20 with no external dependencies and is distributed as the
`glscript` library.

This repository currently implements the **typechecker milestone**:

- lexer with source spans and diagnostics
- parser and AST for the syntax in `entity.gl` and `rt.gl`
- module import resolution (`import input::{Input, Axis};`)
- gradual type checking with `dynamic` and checked `downcast::<T>`
- classes, inheritance, fields, methods, generics, enums, and effect handlers
- a C++ host-registration API for engine modules and global types/effects
- a `glc` command-line typechecker and a test suite

The runtime/interpreter is not implemented yet. Effects are parsed and
typechecked; multi-shot continuation execution is the next milestone.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run

```sh
./build/glc entity.gl
./build/glc rt.gl
./build/glc -I path/to/modules path/to/script.gl
```

`glc` exits with status `0` when the script and every imported module
typecheck, and `1` when diagnostics are emitted.

## EnTT example

`examples/entt/` is a small integration demo. It creates an `entt::registry`
with a player and a goblin (`Position`, `Velocity`, `Health`, `Name`
components), registers those entities as the script-facing `game::Entity` and
global `World` effect, then typechecks `entt_demo.gl`, which has the two
entities interact:

```sh
./build/entt_demo
```

```gl
import game::{Entity, Vec2, World};

function update() {
    Entity player = World::player();
    Entity enemy = World::enemy();

    Vec2 offset = enemy.position() - player.position();
    if (offset.length() < 2.0f) {
        enemy.damage(15.0f);
    }

    player.move(1.0f, 0.0f);
}
```

CMake downloads the EnTT single header into the build directory at configure
time. To use a local copy instead, pass
`-DGLSCRIPT_ENTT_INCLUDE_DIR=/path/containing/entt/entt.hpp`. Pass
`-DGLSCRIPT_BUILD_ENTT_EXAMPLE=OFF` to skip the example entirely. The demo
currently stops after typechecking; executing the script is the runtime
milestone.

## Language surface

The parser accepts the syntax used in the sketches. `function` is the only
function keyword; `fn` is intentionally rejected with a diagnostic.

```gl
import input::{Input, Axis};
import math::Vector3;

export class PlayerMovement extends EntityBehaviour {
    #[serialize]
    float move_speed = 5.0f;

    export function update() {
        println("inside PlayerMovement");
        Runtime::println("also inside PlayerMovement");

        float move_x = Input::get_axis(Axis::Horizontal);
        float move_z = Input::get_axis(Axis::Vertical);
        Vector3<float> move_direction = Vector3::new(move_x, 0.0f, move_z);

        get_object()
            .get_component<Transform>()
            .translate(move_direction * move_speed * dt());

        dynamic value = 100;
        int number = downcast::<int> value;
        String text = downcast::<String> value;
    }
}
```

Supported constructs include:

- typed declarations and assignments: `float x = 0.0f;`, `x = x + 1.0f;`
- `if` / `else`, `while`, `return`, `break`, `continue`
- classes with `extends`, fields, methods, attributes, and type parameters
- enums with optional payloads: `enum Expr<C> { Const(C), Add(Expr<C>, Expr<C>) }`
- effects, operations, handlers, `resume`, and top-level `handle Effect { ... }`
- enum access (`Axis::Horizontal`), static calls (`Vector3::new`), instance
  calls (`get_component<Transform>()`), and `downcast::<T> expr`
- template strings with `${expression}` interpolation

Control-flow bodies use the same brace/semicolon style as the sketches.

## Visibility

- no modifier: visible only in the declaring scope
- `export`: exposed to the host, **not** importable by other scripts
- `pub`: importable by other scripts, **not** exposed to the host
- `pub export`: both

## Host embedding

The engine supplies modules, global types, and ambient effects through
`HostRegistry`. `examples/engine_bindings.cpp` is a complete example.

```cpp
#include "glscript/checker.hpp"
#include "glscript/diagnostic.hpp"
#include "glscript/host.hpp"

glscript::HostRegistry host;

host.module("input")
    .enum_type("Axis")
    .variant("Horizontal")
    .variant("Vertical");

host.module("input")
    .class_type("Input")
    .static_method("get_axis", glscript::TypeRef::floating())
    .parameter("axis", glscript::TypeRef::named("Axis"));

auto engine = host.module("engine");
engine.class_type("GameObject").global()
    .method("get_component", glscript::TypeRef::type_variable("T"))
    .type_parameter("T");

auto runtime = engine.effect("Runtime").global();
runtime.operation("println", glscript::TypeRef::unit())
    .parameter("message", glscript::TypeRef::string());

glscript::DiagnosticSink diagnostics;
glscript::Frontend frontend(host, diagnostics, [](const std::string& module) {
    // Return an optional ModuleSource for source modules, or nullopt to let
    // the host registry handle the module.
    return std::optional<glscript::ModuleSource>{};
});

if (!frontend.check_file("entity.gl")) {
    std::cerr << diagnostics.format();
}
```

Global effects have two call forms. If `Runtime` is registered as a global
effect, `Runtime::println(...)` and the unqualified `println(...)` both resolve
to the same operation.

## Project layout

```text
include/glscript/   public headers
  ast.hpp           AST node definitions
  checker.hpp       Frontend, ModuleSource, ModuleLoader
  diagnostic.hpp    diagnostics with source snippets
  host.hpp          C++ host registration API
  lexer.hpp         tokenization
  parser.hpp        parser entry points
  source.hpp        source files and spans
  token.hpp         token definitions
  type.hpp          semantic types
src/                implementation
examples/           example engine bindings
examples/entt/      EnTT integration demo and its glscript script
tools/glc.cpp       command-line typechecker
tests/              test suite
entity.gl, rt.gl    language sketches
```

## Effect model

Effect declarations name operations and their signatures. A handler case
receives the operation arguments and may `resume <value>` the continuation.
The typechecker validates operation lookup, argument/result types, and the
value passed to `resume`.

The selected runtime semantics are **multi-shot continuations**: a handler may
resume more than once. The current milestone deliberately stops at the
typechecker; no continuation capture or execution exists yet. The AST,
`#[abort]` metadata, and handler scopes are structured so the runtime can use
CPS/continuation-based execution without changing the surface syntax.

## Near-term work

1. Runtime/interpreter with multi-shot resumable handlers (`resume` continuations)
2. Runtime installation of the native `Runtime` effect and host-call bridge
3. `serialize` metadata export for `export` declarations
4. Unit tests for serialization and host-call ABI
5. Full generic enum shorthand and recursive generic types
6. Collection/vector operations and richer operator overloading
