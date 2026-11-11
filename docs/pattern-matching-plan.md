# Pattern matching plan: `match` expressions and `let` destructuring

Status: **implemented** (landed 2026-11-11; deviations from the design below).

Rust-style pattern matching over glscript's enums (and literals), following the
HIR-refactor playbook: the checker resolves and validates everything once, the
lowering turns patterns into a flat, fully-resolved test plan, and the emitter
compiles that plan mechanically with the same jump patching it already does for
`if`/`while`.

Deviations from the original design:

- **Block arm bodies compile as immediate closures** (`ast::BlockExpr` lowered
  to a zero-argument lambda called on the spot). Consequently `break`,
  `continue`, and `return` cannot cross an arm block's value boundary — the
  checker rejects them with dedicated messages.
- **Exhaustiveness is finer than planned**: bool matches are exhaustive over
  `true`/`false` literal arms, and `()` covers a unit scrutinee.
- **Guarded arms never count toward exhaustiveness** (a guard can fail at
  runtime — Rust's rule). An all-guarded match is a checker error, so the
  emitter's no-match `Abort` is unreachable for well-typed programs and exists
  only as a safety net.
- Two pre-existing crashes found while testing were fixed (see status.md):
  handled expressions and handler cases referencing outer variables. Handle
  bodies now compile as real closures (captures bound at install time);
  handler-case captures are rejected by the checker (handler cases run as
  plain functions invoked by the runtime).
- Calling a function value held in a variable (`f()`) never bound the callee —
  fixed in `resolve_callee`.
- `resume` inside match arms works, subject to the existing transfer-resume
  semantics (code after a resume in a case does not execute).

## 1. Grammar

```
pattern      := "_"
              | literal                        int / float / bool / string / ()
              | "-" literal                    negative int/float
              | name [":" type]                binding (see §3 for name resolution)
              | path ["(" pattern ("," pattern)* ")"]   enum variant pattern

match_expr   := "match" expr "{"
                pattern ["if" expr] "=>" (block | expr) [","]
                "}"

let_stmt     := "let" pattern [":" type] "=" expr ";"
```

- `match` is an **expression** (usable in tail position, arguments, initializers).
  Arm bodies are blocks or single expressions; a comma separates arms and is
  optional after a block body and before `}`.
- Guards: `pattern if cond => ...` — the guard sees the arm's pattern bindings
  and must be bool; a failing guard falls through to the next arm.
- `let` keeps the existing `Type name [= expr];` form untouched and adds a
  pattern form. `let` patterns must be **irrefutable** (§4).

## 2. Patterns in the AST

```cpp
struct Pattern; using PatternPtr = std::unique_ptr<Pattern>;
struct WildcardPattern  : Pattern {};                    // _
struct BindingPattern   : Pattern { std::string name; TypeExprPtr annotation;
                                    // checker resolution for lowering:
                                    mutable EnumVariantSymbol* variant = nullptr; };
struct LiteralPattern   : Pattern { ExprPtr value; };    // literal (or -literal)
struct VariantPattern   : Pattern { std::vector<std::string> path;
                                    std::vector<PatternPtr> payload; };
struct ErrorPattern     : Pattern {};
```

`MatchExpr` (expression) carries `scrutinee` + `MatchArm {pattern, guard, body}`.
`VarDeclStmt` gains a `PatternPtr pattern` (null for the legacy form).

## 3. Name resolution in patterns

- A **qualified path** (`Enum::Variant`, `Module::Enum::Variant`) is always a
  variant pattern, resolved like an enum-constructor call.
- A **bare name** binds a variable — unless the *expected type* is an enum that
  declares a variant of exactly that name, in which case it matches that
  (payload-less) variant: `match opt { Some(x) => ..., None => 0 }`. This is
  Rust's resolution rule adapted to glscript's enums. The checker records the
  resolved variant (if any) on the `BindingPattern` node for lowering.
- Bare payload variants (`Some(x)`) resolve against the expected enum type;
  on `dynamic` scrutinees they are an error — use the qualified path.

## 4. Checker

`check_pattern(pattern, expected_type, scope)` defines the arm's bindings and
validates structure:

- **Binding**: annotation (if present) must be compatible with `expected`;
  the variable's type is the annotation when present, else `expected`.
  Duplicate names in one pattern hit the existing redefinition error.
- **Literal**: `expected` must be int/float/bool/String/unit/dynamic; the
  literal's type must be assignable to it. Rejected on enum types.
- **Variant**: `expected` must be that enum (or dynamic); payload count must
  match; payload patterns are checked recursively against the substituted
  payload types (generic enums supported via the existing substitution map).

Match result type: arm body types unify pairwise like `if` branches, with
never-type propagation.

**Exhaustiveness** — enums are checked; everything else needs a catch-all:

- Enum scrutinee: every variant covered by some arm (or a wildcard/binding
  arm) or error: `non-exhaustive match: missing variant 'X'`.
- Non-enum scrutinee: at least one wildcard/binding arm required.
- For `dynamic`, a catch-all arm is required (any arm can fail at runtime).

`let` patterns must be irrefutable: wildcard, binding, or a variant pattern
whose enum has exactly one variant and whose payload patterns are all
irrefutable. Otherwise: `refutable pattern in 'let'; use 'match'`.

## 5. HIR

Key insight: a pattern is a **conjunction** of primitive tests and bindings —
nested payload patterns just append more conjuncts. No backtracking exists.

```cpp
struct MatchTest {
    enum class Kind { Variant, Literal, Bind };
    enum class Term  { Scrutinee, Payload };   // Payload: payload[index] of the
                                               // enum value in value_slot
    Kind kind;
    Term term; std::uint32_t value_slot; std::uint32_t payload_index;
    std::uint32_t variant_id;                  // Variant
    ExprPtr literal;                           // Literal (pre-built value node)
    std::uint32_t slot;                        // Bind: destination local
};
struct MatchArm { std::vector<MatchTest> tests; ExprPtr guard; ExprPtr body; };
```

New HIR expression kinds:

- `Match` — `scrutinee` (ExprPtr), `scrutinee_slot` (fresh local the emitter
  stores it into), `std::vector<MatchArm>`.
- `EnumPayload` — `object` + `payload_index`: read `payload[i]` of an enum value
  (used to materialize `Bind` initializers for `let`).

Lowering:

- Per match: evaluate scrutinee, allocate `$match` local.
- Per arm (own name scope): pre-declare every binding slot, walk the pattern
  preorder producing tests — `Variant` tests, `Literal` tests, and `Bind`s;
  nested variant patterns bind an anonymous local first so payload terms can
  reference it. Guards and bodies lower with the bindings in scope (so lambdas
  inside arms capture them through the existing capture machinery).
- `let p = e;` lowers to plain `VarLocal` statements: a hidden temp for `e`,
  then one `VarLocal` per binding in preorder (initializer = the term
  expression). Irrefutability means no branch is needed; `Variant`/`Literal`
  tests are dropped.

## 6. Bytecode + runtime

Two opcodes (1 operand each):

- `EnumIsVariant vid` — pop value; push bool (heap Enum whose variant pointer
  equals `enum_variants[vid]`; false for anything else — safe on `dynamic`).
- `EnumPayload idx` — pop enum value; push `payload[idx]` (only reachable after
  a passing variant test; runtime error otherwise).

Emitter for `Match` (mirrors the `If` jump-patching pattern):

```
emit scrutinee; StoreLocal $match
for each arm:  [tests: term-load + EnumIsVariant/eq-test/JumpIfFalse→next
                       or term-load + StoreLocal]
               [guard; JumpIfFalse→next]
               emit body (leaves the value); Jump→end
               next: ...
end:           patch all next/end sites
(no arm matched → Op::Abort, reachable only for dynamic scrutinees)
```

`Bind` and literal tests pop what they push; `EnumIsVariant`'s bool is consumed
by `JumpIfFalse` — the stack stays balanced across the arm chain.

## 7. Out of scope (follow-ups)

Or-patterns (`p | q` — needs a `|` token), range patterns, struct patterns
(fields are named, not positional), `if let` / `while let` sugar, unreachable
arm warnings, match ergonomics (auto-ref).

## 8. Test plan (tests/test_main.cpp)

- `match` returning values in tail position; payload-less + payload variants;
  bare variant names (`None`, `Some(x)`); qualified paths.
- Nested variant patterns (`A(B(x))`), literal patterns (int/string/bool/unit,
  negative int), wildcard, guards, binding annotations.
- Exhaustiveness error (missing variant), non-exhaustive non-enum error,
  refutable `let` error, payload arity error, literal-vs-enum error.
- `let` destructuring of single-variant enums and generics; lambda inside an
  arm capturing pattern bindings.
- Runtime: dynamic scrutinee with no matching arm aborts cleanly.
