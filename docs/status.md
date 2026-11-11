# glscript current state

Status as of 2026-11-10. Landed: handle-body delimitation (3c09d99) → HIR
refactor (8ca562e) → splice-aware continuations, suite green (603d0c0) →
scripted gravity demo (63f8416) → effects showcase (1276e65) → embedding API
+ sandbox engine.

## What works

**Pipeline** — `lexer → parser → checker → hir::lower → hir::emit →
interpreter` is fully wired. 41 unit tests + 4 ctest suites green (the three
demos run headless in ctest).

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
| `examples/effects/` | Algebraic effects: one battle script run under six handler stacks (transformation, god mode, layered delegation, narration re-routing, non-resuming abort) |

- **Tooling**: `glc --dump-hir` prints a readable per-function IR dump —
  the debugging surface the HIR refactor exists for.
- **Runtime infrastructure**: mark-sweep GC with continuation + retained-root
  scanning, host-call bridge, per-instruction span tracking, and an
  `emit`/`operand_count` single-source assert in the emitter.

## What doesn't work yet / known limits

| Area | State |
|---|---|
| **Indexing** | `IndexExpr` is a stub (lowers to unit); the checker rejects it for most types. No collections/vectors yet. |
| **Captured-variable assignment** | By-value upvalues: assigning to a captured var is silently discarded (documented limitation). |
| **Multi-shot semantics** | `resume` is tail-position: code after a non-identity resume in the handler is dead. One activation cannot resume twice *and* use both completions at the outer level. (True recursive multi-shot — the canonical algebraic reading — would give 48, not 4, for the multi-shot test script; the implemented semantics match the committed tests.) |
| **`glc` can't run scripts** | `glc` checks and dumps HIR only; no `-r`/run mode and no bytecode output. Running requires the C++ API. |
| **Script-side virtual dispatch** | Method calls inside script resolve statically; runtime-type dispatch happens only when the *host* drives it via `call_method`. No vtables yet. |
| **Edge cases** | `handle` *inside* a handler case + resume from within is untested; handler-case bodies that `PushHandler` then transfer-resume leave stale delimiters (safe cleanup added, but untested). Single-name handler cases in `handle` expressions resolve only against global host effects (ambiguity across effects is a checker error). |
| **GC root growth** | `continuations_` and `retain`ed values are freed only when the Runtime dies. Fine for scripts and demos; long-lived engines want scoped root handles. |
| **Dead AST** | `ast::IfExpr` exists but the parser never produces it (only `if` *statements*); lowering handles it defensively as unit. |
| **Open roadmap items** | serialize metadata export, generic enum shorthand, recursive generic types, richer operator overloading, collection operations (README "Near-term work"). |

## Sequencing

Remaining Stage 6: `glc -r` (run mode) and the README refresh. The abandoned
runtime rework in the git stash is superseded and can be dropped.
