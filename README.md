# glscript

An embeddable, statically typed scripting language for game engines. The front
end is written in C++20 with no external dependencies and is distributed as the
`glscript` library.

This repository implements the full pipeline — lexer, parser, typechecker,
typed HIR, bytecode emitter and interpreter — plus a TCP debug protocol and an
NSH debugger GUI.

- lexer with source spans and diagnostics
- parser and AST for the syntax in `entity.gl` and `rt.gl`
- module import resolution (`import input::{Input, Axis};`)
- gradual type checking with `dynamic` and checked `downcast::<T>`
- classes, inheritance, fields, methods, generics, enums, and effect handlers
- a C++ host-registration API for engine modules and global types/effects
- a bytecode VM with multi-shot-style `resume` continuations, mark-sweep GC
  and per-instruction source spans
- `glc` run mode (`-r`), HIR/bytecode dumps, and `--debug` (TCP)
- the NSH debugger GUI in `tools/debugger/`

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run

```sh
./build/glc entity.gl                  # check only
./build/glc -r examples/debug/target.gl  # run main()
./build/glc --dump-hir entity.gl
./build/glc --dump-bytecode examples/debug/target.gl
```

`glc` exits with status `0` when the script and every imported module
typecheck (and, in run mode, when `main` returns), and `1` when diagnostics
are emitted.

## Debugger

`glc -r --debug[=port] script.gl` binds `127.0.0.1:9000` (or the given port),
waits for a client, then runs the script under the cooperative debug server in
`src/debug_server.cpp`. The protocol is newline-delimited JSON: breakpoints,
continue/pause/step, stack traces, locals and expandable values, expression
evaluation, disassembly and source loading.

The GUI is an NSH TSX app (GTK4 on Linux):

```sh
# terminal 1
./build/glc -r --debug examples/debug/target.gl

# terminal 2 (nsh from https://nsh.dev/install.sh)
cd tools/debugger
nsh run
```

Connect to `127.0.0.1:9000`, click the gutter to set breakpoints, and use the
toolbar to continue or step. `tools/debugger/nsh.json` grants only that socket.
See [docs/debugger-plan.md](docs/debugger-plan.md) for the protocol tables and
design notes.

For a minimal embedding host rather than the `glc` CLI, build and run
`debugger_host`. It loads a script, starts the debug server, waits for the GUI,
and then runs `main()` under debugger control. It also registers a small
`hostdemo::Game` module (`frames()`, `log(message)`) so breakpoints can walk
through a script that calls back into C++:

```sh
# terminal 1
cmake --build build --target debugger_host
./build/debugger_host                    # host_demo.gl on port 9000
./build/debugger_host examples/debug/target.gl 9000

# terminal 2
cd tools/debugger
nsh run
```

In `examples/debug/host_demo.gl`, set breakpoints on `let next = a + b;` (in
`fib`) and `total = total + value;` (in `main`), then inspect `a`, `b`, `i`,
`value` or evaluate `a + b` / `total + value`.

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
runs its scripted physics headlessly in ctest and renders with raylib when
given a display.

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
  hir.hpp           typed HIR: lowering output, emit() and dump() entry points
  host.hpp          C++ host registration API
  debug.hpp         DebugHook interface and read-only frame views
  debug_server.hpp  TCP NDJSON debug server (nlohmann/json)
  disasm.hpp        structured bytecode disassembler
  dbg_eval.hpp      paused-frame expression evaluator
  lexer.hpp         tokenization
  parser.hpp        parser entry points
  source.hpp        source files and spans
  token.hpp         token definitions
  type.hpp          semantic types
src/                implementation
  lower.cpp         checker AST + bindings -> HIR (resolution + desugaring)
  hir_dump.cpp      HIR printer (glc --dump-hir)
  compiler.cpp      HIR -> bytecode emitter
  runtime.cpp       bytecode interpreter, GC and effect continuations
  disasm.cpp        disassembler shared by glc and the debug protocol
  debug_server.cpp  TCP debug server implementing DebugHook
  dbg_eval.cpp      debugger expression evaluator (no calls)
examples/           example engine bindings
examples/debug/     target.gl walkthrough + minimal C++ debug host
tools/glc.cpp       command-line compiler / runner
tools/debugger/     NSH TSX debugger GUI
tests/              test suite (including debugger_test.cpp)
entity.gl, rt.gl    language sketches
```

## Effect model

Effect declarations name operations and their signatures. A handler case
receives the operation arguments and may `resume <value>` the continuation.
The typechecker validates operation lookup, argument/result types, and the
value passed to `resume`.

The VM executes handlers with splice-aware continuation capture. `resume` is
tail-position: following a non-identity resume in the handler is dead, and one
activation cannot resume twice *and* combine both completions at the outer
level. The implemented tests pin those semantics (see
[docs/status.md](docs/status.md)).

## Near-term work

1. `serialize` metadata export for `export` declarations
2. Unit tests for serialization and the host-call ABI
3. Full generic enum shorthand and recursive generic types
4. Collection/vector operations and richer operator overloading
5. Script-side virtual dispatch (the host can already dispatch via `call_method`)
