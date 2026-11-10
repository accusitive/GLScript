# glscript current state

Status as of 2026-11-10, after the HIR refactor, the continuation-runtime
fixes, and the entt_demo gravity sim. Commit sequence: handle-body
delimitation (3c09d99) → HIR refactor (8ca562e) → runtime splice-aware
continuations (603d0c0) → scripted gravity demo.

## What works

**Pipeline** — `lexer → parser → checker → hir::lower → hir::emit →
interpreter` is fully wired. All 38 tests pass (22 checker + 16 runtime),
plus the entt_demo build test.

- **Language core**: functions, methods, classes with multi-level inheritance
  and absolute field slots, enums (payload + shorthand ctors), generics
  (method inference, explicit type arguments, generic enum payloads), imports
  with `pub`/`export` visibility, template strings, if/while/break/continue,
  operator overloading via host thunks, `downcast::<T>` with dynamic checked
  narrowing.
- **Effects & continuations**: `handle … with {}` expressions, top-level
  ambient `handle Effect {}` declarations, handler cases with implicit
  continuation parameters, `resume` — including re-raises inside resumed
  computations (splice-aware capture), transfer-resume tail semantics, and
  identity continuations for ambient/delimiter-frame raises.
  `run_multi_shot`, `run_nested_handlers`, ambient handlers all green.
- **Runtime infrastructure**: mark-sweep GC with continuation rooting,
  host-call bridge, per-instruction span tracking, and an
  `emit`/`operand_count` single-source assert in the emitter.
- **Tooling**: `glc --dump-hir` prints a readable per-function IR dump —
  the debugging surface the HIR refactor exists for.

## What doesn't work yet / known limits

| Area | State |
|---|---|
| **Indexing** | `IndexExpr` is a stub (lowers to unit); the checker rejects it for most types. No collections/vectors yet. |
| **Captured-variable assignment** | By-value upvalues: assigning to a captured var is silently discarded (documented limitation). |
| **Multi-shot semantics** | `resume` is tail-position: code after a non-identity resume in the handler is dead. One activation cannot resume twice *and* use both completions at the outer level. (True recursive multi-shot — the canonical algebraic reading — would give 48, not 4, for the multi-shot test script; the implemented semantics match the committed tests.) |
| **`glc` can't run scripts** | `glc` checks and dumps HIR only; no `-r`/run mode and no bytecode output. Running requires the C++ API. |
| **entt_demo** | Runs a gravity sim: 12 EnTT bodies, one scripted `update(entity, dt)` invocation per entity per frame (gravity, integration, floor/wall bounce all in glscript), rendered with raylib. `--headless N` runs without a window and is what ctest exercises. |
| **Edge cases** | `handle` *inside* a handler case + resume from within is untested; handler-case bodies that `PushHandler` then transfer-resume leave stale delimiters (safe cleanup added, but untested). |
| **Continuation GC roots** | `Runtime::continuations_` grows monotonically — freed only when the Runtime dies. Fine for scripts, worth a pool for long-lived engines. |
| **Dead AST** | `ast::IfExpr` exists but the parser never produces it (only `if` *statements*); lowering handles it defensively as unit. |
| **Open roadmap items** | serialize metadata export, generic enum shorthand, recursive generic types, richer operator overloading, collection operations (README "Near-term work"). |

## Sequencing

Stage 6 leftovers: `glc -r` (run mode) and the README refresh — the entt_demo
script-execution item is done (gravity sim above). The abandoned runtime
rework in the git stash is superseded and can be dropped.
