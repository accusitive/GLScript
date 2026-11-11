# Arrays plan: `array<T>`, literals, effectful indexing, `Option<T>.get`

Status: **planned** (this commit lands the design; implementation follows).

## 1. Surface

```
type        := "array" "<" type ">"          e.g. array<int>, array<dynamic>, array<array<int>>
literal     := "[" [expr ("," expr)* [","]] "]"     e.g. [0, 1, 2], [], [1, "a", true]
index read  := expr "[" expr "]"             arr[i] : T
index write := expr "[" expr "]" "=" expr    arr[i] = v
methods     := arr.len() -> int              number of elements
               arr.push(v: T) -> unit        append (mutates)
               arr.get(i: int) -> Option<T>  Some(elem) / None (never raises)
```

- `array<T>` is a builtin generic; `array<dynamic>` is the heterogeneous form.
  A literal's element type is the unified type of its elements (`[1, 2]` is
  `array<int>`, mixed/empty literals are `array<dynamic>`). `array<int>`
  assigns to `array<dynamic>` (element-wise covariance; arrays only).
- Indexing outside `0..len-1` (or with a non-int on `dynamic` paths) **raises
  the std effect** rather than crashing:

```
effect OutOfBounds {
    function index(size: int, index: int) -> dynamic;
}
```

  Handled at the callsite, the handler's result becomes the element value:

```
handle arr[10] with {
    function OutOfBounds::index(size: int, got: int) -> int {
        return 0;              // or `resume fallback` to continue the read
    }
}
```

  Unhandled, the op falls back to a host thunk that prints the details and
  aborts (same shape as `invalid_downcast`).
- `arr.get(i)` is sugar implemented *over* `[]`: a synthesized handler around
  `Option::Some(arr[i])` that catches `OutOfBounds::index` and yields
  `Option::None`.

## 2. std module

A host-registered module `"std"` (new `register_std(HostRegistry&)` in
`host.cpp`, called by embedders alongside their own bindings):

- class `array<T>` (global) — host methods `len`, `push` (plain host thunks;
  no effect machinery needed),
- enum `Option<T>` (global; `Some(T)`, `None`) — generic host enums already
  work end-to-end (ctors, variant values, pattern matching),
- effect `OutOfBounds` (global) with the aborting `index` fallback thunk.

Types in the `std` module get **bare qualified names** (`array`, `Option`,
`OutOfBounds` instead of `std::array`) so diagnostics read `array<int>` and
scripts write `handle OutOfBounds::index(...)`. They are `global`, so no
import is needed.

## 3. Checker

- `array<T>` resolves through the normal named-type path (global scope
  symbol, one type parameter — arity errors come free).
- Injects a synthetic method `get` on `array` after host setup: a
  `FunctionSymbol` with no declaration, `parameter_types = [int]`,
  `result = Option<T>`. Generic binding through the receiver (owner's `T`)
  reuses `check_function_call`'s existing machinery.
- `IndexExpr` checks only against `array` types (element type `T`, index
  assignable to `int`); everything else — including `dynamic` — is a checker
  error (indexing used to be a silent lowering stub). Binds
  `BindingKind::Index`; assignment targets check the same way.
- Array literals unify element types pairwise (match-arm rules) →
  `array<T>` / `array<dynamic>`; binds `BindingKind::ArrayLiteral`.
- `is_assignable` gains element-wise covariance for `array` only.

## 4. HIR + bytecode

Three opcodes (GC-traced `ArrayObj` on the heap):

- `ArrayNew argc` — pop argc elements → array value (literals).
- `ArrayGet` — pop index, array → element, or push `size, index` and raise
  `OutOfBounds::index` (read path; the handler's result is the element).
- `ArraySet` — pop value, index, array → write, or raise; pushes the op
  result so handled raises stay stack-balanced (the assignment emitter pops).

`arr.get(i)` lowers to the existing `handle` desugaring, fully synthesized in
the lowering pass: a cached handler-case function returning `None`, a
per-callsite body function `Some(ArrayGet(recv, i))` (origin `HandleBody`),
and a `Handle` node carrying the body's captures — the same shape effect
bodies already compile to, so continuations/resume work unchanged.

## 5. Runtime

`ArrayObj : GcObject { std::vector<Value> elements }` (traced);
`type_name` → `"array"`; `stringify` → `[1, 2, 3]` recursively.
`len`/`push` run as host thunks over `as_array`.

## 6. Out of scope (follow-ups)

Iteration (`for x in arr`), `pop`/`insert`/`remove`, slices, fixed-length
arrays, `==` on arrays, arrays as downcast targets.

## 7. Test plan

- Literals (typed, mixed → dynamic, nested, empty), `len`, `push`, element
  assignment, `array<dynamic>` heterogeneity, arrays as params/fields/
  generic arguments.
- `get` on present/missing indices (`Some`/`None`), on empty arrays.
- Out-of-bounds handled at the callsite (return-a-fallback and `resume`
  forms), unhandled → runtime error; negative indices.
- Type errors: indexing a non-array, float index, `array<int>` vs
  `array<String>` assignment, wrong `push` element type.
