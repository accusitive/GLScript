# glscript current state

Status as of 2026-11-12. Landed: handle-body delimitation (3c09d99) → HIR
refactor (8ca562e) → splice-aware continuations, suite green (603d0c0) →
scripted gravity demo (63f8416) → effects showcase (1276e65) → embedding API
+ sandbox engine → pattern matching → arrays → first-person Jolt + raylib demo
→ TCP debug protocol + NSH debugger GUI (see
[pattern-matching-plan.md](pattern-matching-plan.md),
[arrays-plan.md](arrays-plan.md),
[fps-demo-plan.md](fps-demo-plan.md) and
[debugger-plan.md](debugger-plan.md)).

## What works

**Pipeline** — `lexer → parser → checker → hir::lower → hir::emit →
interpreter` is fully wired. 71 unit tests + 6 ctest suites green (the demos
and the debugger protocol test run headless in ctest).

- **Language core**: functions, methods, classes with multi-level inheritance
  and absolute field slots, enums (payload + shorthand ctors), generics
  (method inference, explicit type arguments, generic enum payloads), imports
  with `pub`/`export` visibility, template strings, if/while/break/continue,
  operator overloading via host thunks, `downcast::<T>` with dynamic checked
  narrowing.
- **Pattern matching**: Rust-style `match` expressions and `let` destructuring
  — literal/wildcard/binding patterns, enum variant patterns (bare names
  resolve against the scrutinee's enum type, qualified paths for `dynamic`),
  nested payload patterns, binding type annotations, guards
  (`pattern if cond => ...`), block arm bodies (tail expression = value),
  exhaustiveness checking (enums, bool, unit; guarded arms don't count) and
  refutability checking for `let`. Implemented as a flat per-arm test plan in
  HIR (`MatchTest`: variant/literal/bind conjuncts) compiled by the emitter's
  jump patching; two new opcodes (`EnumIsVariant`, `EnumPayload`).
- **Arrays**: `array<T>` builtin generic (std module, no import needed) with
  JS-style literals `[0, 1, 2]` (element type unified; mixed/empty →
  `array<dynamic>`), element-covariant assignment (`array<int>` fits
  `array<dynamic>`), `len()`/`push(v)` host methods, element write `arr[i] = v`,
  and effectful reads: out-of-bounds (or non-int) indices raise the std
  `OutOfBounds::index(size, got)` effect — handle it at the callsite (the
  handler's result, or a `resume`d substitute, becomes the element value) or
  let the fallback thunk print details and abort. `arr.get(i) -> Option<T>`
  (`std` `Option` enum, pattern-matchable) wraps `[]` with a synthesized
  handler: `Some(elem)` / `None`, never raises.
- **Effects & continuations**: `handle … with {}` expressions, top-level
  ambient `handle Effect {}` declarations, handler cases with implicit
  continuation parameters, `resume` — including re-raises inside resumed
  computations (splice-aware capture), transfer-resume tail semantics, and
  identity continuations for ambient/delimiter-frame raises.
- **Embedding API (Unity–C# model)**: `Runtime::call_method` dispatches
  script methods on an instance's runtime type (the engine drives behaviour
  lifecycles), `Runtime::field`/`set_field` for prefab-style access,
  `Runtime::retain` for host-held GC roots, and `run`/`call_method` now
  return the entry call's value. Script/host module-name collisions are a
  checker error instead of silently dropped declarations.
  See [embedding.md](embedding.md).

| Example | What it shows |
|---|---|
| `examples/sandbox/` | Unity-style embedding: behaviour classes, prefab fields, `on_start`/`update`/`on_collision` lifecycle, spawn-from-script, raylib renderer |
| `examples/entt/` | Per-entity scripted physics (gravity sim) with EnTT + raylib |
| `examples/fps/` | First-person shooter over **Jolt Physics**: script-owned firing, fork-cascade projectiles (contact events → script component check → re-spawn aimed at other enemies), script-side damage/health with host-rendered healthbars; arrays + entity comparison operators in the fork targeting |
| `examples/effects/` | Algebraic effects: one battle script run under six handler stacks (transformation, god mode, layered delegation, narration re-routing, non-resuming abort) |

- **Tooling**: `glc --dump-hir` prints a readable per-function IR dump, and
  `glc --dump-bytecode` shares the structured disassembler with the debugger.
  `glc -r` runs `main`, with `--debug[=port]` serving the NDJSON debug
  protocol (breakpoints, stepping, stack/locals, evaluate, disassembly).
  The NSH TSX GUI in `tools/debugger/` drives it over `127.0.0.1:9000`;
  `tests/debugger_test.cpp` exercises the wire protocol end to end.
- **Runtime infrastructure**: mark-sweep GC with continuation + retained-root
  scanning, host-call bridge, per-instruction span tracking, and an
  `emit`/`operand_count` single-source assert in the emitter.

## What doesn't work yet / known limits

| Area | State |
|---|---|
| **Indexing** | Works for `array<T>` only (reads raise `OutOfBounds`, writes raise too). Indexing other types is a checker error; chained reads into `array<dynamic>` elements need `downcast`. No collections beyond arrays yet. |
| **Captured-variable assignment** | By-value upvalues: assigning to a captured var is silently discarded (documented limitation). |
| **Multi-shot semantics** | `resume` is tail-position: code after a non-identity resume in the handler is dead. One activation cannot resume twice *and* use both completions at the outer level. (True recursive multi-shot — the canonical algebraic reading — would give 48, not 4, for the multi-shot test script; the implemented semantics match the committed tests.) |
| **`glc -r` / debugger** | Runs `main` and speaks the TCP protocol. Suspended continuations are not yet exposed in the stack view; `evaluate` supports literals, locals, indexing, fields, arithmetic and logic but not calls (the VM is not reentrant while paused). |
| **Script-side virtual dispatch** | Method calls inside script resolve statically; runtime-type dispatch happens only when the *host* drives it via `call_method`. No vtables yet. |
| **Edge cases** | `handle` *inside* a handler case + resume from within is untested; handler-case bodies that `PushHandler` then transfer-resume leave stale delimiters (safe cleanup added, but untested). Single-name handler cases in `handle` expressions resolve only against global host effects (ambiguity across effects is a checker error). |
| **GC root growth** | `continuations_` and `retain`ed values are freed only when the Runtime dies. Fine for scripts and demos; long-lived engines want scoped root handles. |
| **Dead AST** | `ast::IfExpr` exists but the parser never produces it (only `if` *statements*); lowering handles it defensively as unit. |
| **Open roadmap items** | serialize metadata export, generic enum shorthand, recursive generic types, richer operator overloading, collection operations (README "Near-term work"). |

## Fixed alongside pattern matching (2026-11-11)

Three pre-existing defects surfaced by the pattern-matching tests, all fixed:

- **Handled expressions could not reference outer variables** — `handle
  f(x) with {...}` inside a function crashed the interpreter
  (`bad_variant_access`): handle bodies compiled as bare functions, so the
  capture's `LoadCapture` ran with no closure object. Handle bodies now
  compile as real closures; captures are bound at delimiter-installation time.
- **Handler-case captures** crashed the same way; they are now a clean checker
  error (`handler cases cannot reference outer variables`), since handler
  cases are invoked by the runtime as plain functions.
- **Calling a function value held in a variable** (`let f = ...; f()`) never
  bound the callee identifier and lowered to unit ("value of type unit is not
  callable"). `resolve_callee` now binds it.

## Sequencing

Stage 6 is complete: `glc -r` (run mode), bytecode dumps and the debugger
front end landed 2026-11-12. The abandoned runtime rework in the git stash is
superseded and can be dropped.
