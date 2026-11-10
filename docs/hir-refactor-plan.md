# HIR refactor plan: AST → typed HIR → bytecode

Status: **implemented** (landed 2026-11-10; §8 steps 1–6 done, deviations below).

Deviations from the original design, kept for behavior-identical output:
- Tail-expression returns apply to *all* block bodies (§2.3.3 said regular
  functions keep implicit-unit returns — that would have broken
  `function f() -> int { 42; }`).
- `Handle` and `Abort` are expression kinds (they compile in expression
  position today), not statement kinds (§2.3 listed them as statements).
- Module-init handle emission order is PopHandler-then-Pop instead of
  Pop-then-PopHandler — behaviorally equivalent.
- `HandlerTable::return_ip` was already gone from bytecode.hpp.

## 1. Why

The current `src/compiler.cpp` mixes two jobs in one pass:

1. **Semantic resolution** — for every AST node, re-derive meaning from the
   checker's `Binding` records: is this call an enum constructor, an effect
   operation, a host function, or a script function? Is this name a local, a
   receiver field, or a captured variable? Which absolute field slot?
2. **Code generation** — emit opcode words, patch jumps, build constant pools.

Everything resolved is *implicit* — encoded in emitted operand words. Debugging
the runtime milestone (multi-shot continuations) required reverse-engineering
raw dumps like `op=25 operands: 2 3 1`. Several compiler bugs found today were
directly of this class:

- `operand_count()` disagreed with the emitter → the interpreter parsed
  instruction boundaries wrongly (looked like heap corruption, was a
  disagreement between two hand-maintained tables).
- Handled `handle` bodies compiled inline in the caller's frame → captured
  continuations weren't delimited; fixed by ad-hoc desugaring to separate
  functions.

A typed, fully resolved HIR between the checker and the bytecode makes the
resolution decisions explicit and one-time, shrinks the emitter to a
mechanical walk, and gives us a readable per-function dump (`glc --dump-hir`)
to debug against.

Pipeline after the refactor:

```
lexer → parser → checker (Binding on AST)
                              ↓
                       hir::lower(program)     ← new: all resolution + desugaring
                              ↓
                       hir::Program            ← new: fully typed/resolved IR
                       ├── emit → bytecode::CompiledProgram
                       └── dump → text
```

## 2. HIR design (`include/glscript/hir.hpp`)

### 2.1 Functions

```cpp
struct Function {
    std::string name;
    std::string qualified_name;
    enum class Origin { Function, Method, Lambda, HandlerCase, HandleBody, ModuleInit };
    Origin origin;
    bool has_receiver = false;          // methods: slot 0 is the receiver
    std::uint32_t param_count = 0;
    std::vector<std::string> param_names;   // debug/dump only
    std::uint32_t capture_count = 0;
    std::uint32_t local_count = 0;
    std::uint32_t resume_slot = kNoSlot;    // HandlerCase only: continuation param slot
    Block body;
    SourceSpan span;
};
```

- `Origin` replaces the implicit conventions the compiler currently uses
  ("methods get a receiver at slot 0", "handler cases get a continuation
  param at `param_count`").
- All script functions and methods are **pre-registered** (indices stable
  before any body lowers, so forward calls work). Lambdas, handler cases, and
  handle bodies are appended during lowering; store functions as
  `std::vector<std::unique_ptr<Function>>` so references survive growth.

### 2.2 Expressions — fully resolved, nothing to look up

Tagged variant (the codebase's first `std::variant` use; sanctioned):

```cpp
struct Expr {
    enum class Kind { /* below */ };
    Kind kind;
    SourceSpan span;
    // kind-specific payload fields (flat struct, matching codebase style)
};
```

| Node | Replaces (today) | Notes |
|---|---|---|
| `Unit` / `Bool` / `Int` / `Float` / `String` | literal AST nodes + `LoadConst` | values inline in the node |
| `Local { slot }` | identifier + `Binding::Variable` + scope search | slot assigned at lowering |
| `Capture { index }` | identifier resolved via `resolve_upvalue` | transitive capture decided at lowering |
| `Field { object, slot }` | `Binding::Field` + `field_slot()` | absolute slot computed **once** |
| `FieldDynamic { object, name }` | dynamic member access | name string kept |
| `EnumVariant { variant_id }` | payload-less variant value | |
| `IntrinsicBinary { op, lhs, rhs }` | `BinaryExpr` with string op | `op` becomes an enum (`Add`, `Lt`, …), not a string |
| `IntrinsicUnary { op, operand }` | `UnaryExpr` | `UnaryPlus` lowered away entirely |
| `ShortCircuit { and/or, lhs, rhs }` | `&&` / `||` in the emitter | explicit node; emitter does the Dup/Jump dance |
| `OperatorCall { op_id, args }` | `Binding::Operator` + `CallOperator` | arity 1 or 2 folded in |
| `Call { target, receiver, args }` | `compile_call`'s dispatch tree | `target` = `ScriptFn{fn}` \| `HostFn{id}` \| `EnumCtor{variant_id}`; `receiver` present ⇔ implicit `this` or `.method()`; one decision site |
| `CallValue { callee, args }` | first-class closure calls | |
| `Raise { effect, operation, args }` | effect-op calls | desugared from any call to an `is_effect_operation` symbol; names are strings, matched against handler tables |
| `EnumNew { variant_id, args }` | enum ctor calls | |
| `Downcast { target: TypePtr, operand }` | `Binding::Downcast` | |
| `BuildString { parts }` | `TemplateStringExpr` | literal/expr parts flattened at lowering |
| `Closure { fn, capture_count }` | `LambdaExpr` | lambda body already a separate `Function` |
| `Resume { continuation, value }` | `ResumeExpr` | `continuation` is a `Local`/`Capture` node — the "$continuation" lookup resolved at lowering |

Statements / control flow (structured — the emitter does jump patching):

```cpp
Block { items }, ExprStmt, VarLocal { slot, value },      // declaration = slot + init
Assign { target, value },                                  // target: Local | Field | FieldDynamic
Return { value },                                          // explicit; lowering inserts unit returns
If { cond, then, else }, While { cond, body },
Break, Continue,                                           // loops tracked at lowering
Handle { body_fn, table_ref },                             // see §2.3
Abort
```

Assignment to captured variables stays unsupported (by-value capture);
lowering reports it like today.

### 2.3 Desugarings performed by lowering

1. **Handle bodies become functions** (`Origin::HandleBody`). This is the
   structural fix for continuation delimitation discovered today: the handled
   expression must always be exactly one frame above the handler's delimiter,
   so a captured continuation ends at the body's `Return`. `Handle` nodes then
   lower to `PushHandler(table) / CallFunction(body_fn) / PopHandler` in the
   emitter.
2. **Handler cases become functions** (`Origin::HandlerCase`) with an implicit
   continuation parameter at slot `param_count`; `resume_slot` records it.
3. **Tail expressions**: the last `ExprStmt` of a lambda / handler-case body
   becomes `Return { value }` (gives `resume` tail-position semantics and
   expression-lambda results). Regular functions keep implicit-unit returns.
4. **Logical operators** → `ShortCircuit` nodes.
5. **Unary `+`** → dropped.
6. **Effect op calls** → `Raise` (script *and* host effect ops — the runtime
   falls back to host thunks when unhandled).
7. **`resume` value parsing** already fixed in the parser (`parse_unary`);
   lowering just sees well-formed `ResumeExpr`s.

### 2.4 Program

```cpp
struct Program {
    std::vector<std::unique_ptr<Function>> functions;
    std::vector<HandlerTable> handler_tables;
    std::vector<const EnumVariantSymbol*> enum_variants;
    std::vector<const OperatorInfo*> operators;
    std::vector<const FunctionSymbol*> host_functions;
    std::vector<TypePtr> downcast_targets;
    std::unordered_map<const FunctionSymbol*, std::uint32_t> function_ids;
    std::unordered_map<std::string, std::uint32_t> module_inits;
    std::unordered_map<std::string, std::vector<std::uint32_t>> module_ambient_handlers;
};
```

Mirrors today's `CompiledProgram` tables minus the bytecode; pointer targets
live in the checker's `Program` (lifetime requirement unchanged: checker
Program must outlive the HIR and the bytecode).

## 3. Lowering pass (`src/lower.cpp`, `hir::lower()`)

One `Lowerer` class, per-function state mirrors today's `FuncState`
(slot scopes, capture resolution, loop stack) but **produces HIR nodes
instead of opcode words**:

- Walks `Program` → for each source module, pre-register functions/methods,
  compile bodies, build module inits (top-level `handle <expr> with {}`) and
  ambient handler tables (top-level `handle Effect {}`).
- Local slots: block-scoped scope stack, params first (receiver at 0 for
  methods), `local_count` recorded per function.
- Captures: transitive by-value upvalue resolution (the current
  `resolve_upvalue` algorithm, unchanged).
- Field slots: base-chain-first absolute slot computation (`field_slot`).
- Every decision uses the checker's `Binding` — one consumer, no re-resolution.

## 4. Bytecode emitter (`src/compiler.cpp` rewritten)

`emit(hir::Program) → CompiledProgram` — mechanical:

- one `switch` over HIR node kinds; each maps to 1–2 ops;
- `operand_count()` lives next to the emitter and is derived from the same
  table the emitter writes (single source of truth — kills the class of bug
  from today);
- structured `If`/`While`/`Break` → jump patching local to the emitter;
- constants pool per function; spans copied from HIR nodes (per code word, as
  today) so runtime errors keep source locations.

`bytecode.hpp`'s op set stays as-is (it's sound); only `HandlerTable`'s
`return_ip` field is dropped (obsolete with handle-body desugaring).

## 5. HIR printer (`src/hir_dump.cpp`)

`dump(const hir::Program&, std::ostream&)` — indented line-based output, one
function per section, e.g.:

```text
fn dbg::main (params=0 locals=1)
  var local 0 = handle table 0
    call body_fn 1
  ...
fn dbg::(handle body) (params=0)
  raise engine::Runtime::count argc=0
  ...
```

Expose via `glc --dump-hir script.gl` (after a successful check). This is the
debugging surface the refactor exists for.

## 6. Testing

- The 38-test suite (22 checker + 16 runtime) is the gate: behavior must be
  byte-for-byte identical before/after.
- Runtime tests currently blocked by the in-flight splice bug (§7) — that fix
  is runtime-side and happens after this refactor per the agreed sequencing.
- Add a smoke test: `compile_program(lower(check(program)))` runs the same
  entry function as today for a representative script.

## 7. Explicit non-goals / known follow-ups

- **The multi-shot splice bug is runtime-side** (`invoke_handler` /
  `resume_continuation` stack-slice alignment) and is *not* fixed by this
  refactor. Sequenced after the refactor: slice the captured stack from the
  bottom captured frame's base so a completed continuation's `Return` restores
  the resumer's stack exactly, then get the suite green.
- Captured-variable assignment (by-value upvalues) — unchanged limitation.
- `IndexExpr` and dynamic member semantics — unchanged (HIR keeps
  `FieldDynamic`; indexing still a stub).

## 8. Migration steps (each lands green)

1. `include/glscript/hir.hpp` — node types, `hir::Program`, dumper decl.
2. `src/hir_dump.cpp` — printer + `glc --dump-hir` (against a stub lowering is
   not possible; printer lands with lowering).
3. `src/lower.cpp` — `hir::lower(const Program&)`; reuse `FuncState` logic;
   delete nothing yet.
4. Rewrite `src/compiler.cpp` as `emit(hir::Program)`; `compile_program`
   becomes `emit(hir::lower(program))`.
5. Delete the old resolution/emission internals from step 4's predecessor;
   drop `HandlerTable::return_ip`; remove the temporary GLTRACE/GLADD/GLRET
   instrumentation from `src/runtime.cpp`.
6. Update `tools/glc.cpp` (`--dump-hir`), README project-layout section.

Estimated shape: ~350 lines hir.hpp, ~700 lines lower.cpp (mostly moved from
compiler.cpp), ~350 lines emitter, ~120 lines dumper — net roughly neutral,
with the resolution logic isolated and inspectable.

## 9. Open questions (none blocking)

- Keep `Binding` as the lowering input (recommended — checker stays the single
  resolution authority) vs. folding resolution into lowering later. Decided:
  keep.
- Whether `Raise` should carry a `TypePtr` result for future checked effects —
  deferred until needed.
