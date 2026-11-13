# Debugger plan: TCP debug protocol + NSH debugger GUI

Status: **implemented** (landed 2026-11-12; deviations from the design below).

## 1. Goals

- A wire-level debugging protocol spoken by the bytecode VM over TCP: the VM
  binds a port, a GUI connects, and the VM pauses/inspects/steps scripts.
- A debugger GUI built with NSH (https://nsh.dev, Longbridge's Rust-native UI
  framework, GTK4 backend, TSX components): source panel with breakpoints,
  bytecode panel, stack trace, variables tree, expression evaluation, and
  continue/pause/step controls.
- `glc -r` (run mode, the remaining Stage 6 item) with `--debug`.

## 2. Architecture

```
tools/debugger (NSH TSX app)          glc -r --debug 9000 target.gl
   net.connect({host,port})  ──TCP──▶  DebugServer (src/debug_server.cpp)
   NDJSON request frames                       │ implements DebugHook
                                               ▼
                                     Runtime::execute() dispatch loop
                                     (hook invoked before each instruction;
                                      pumps the socket, blocks while paused)
```

The interpreter is synchronous, single-threaded, and non-reentrant, so the
debug server is **cooperative**: `Runtime::set_debug_hook` installs a
`DebugHook` (include/glscript/debug.hpp) invoked at the top of the dispatch
loop (runtime.cpp, after the span fetch, before the switch, guarded by a null
check — zero cost when unset). The hook checks breakpoints/step state and,
when a stop is due, sends a `stopped` event and pumps the socket in a blocking
loop until a resume/step command arrives. The server never touches the VM from
another thread.

## 3. Protocol (NDJSON over TCP)

Newline-delimited JSON, one message per line. Client→VM **requests** carry a
monotonic `id`; VM→client **responses** echo it (`{"id": N, ...}`), while
**events** have no `id` (`{"event": "...", ...}`).

| Request | Payload | Response |
|---|---|---|
| `set_breakpoints` | `{file, lines:[...]}` | `{accepted:[lines]}` (deduped, sorted) |
| `continue` | | `{}` then `stopped` event later |
| `pause` | | `{}` then `stopped {reason:"pause"}` |
| `step_in` | | `{}`; stop at the next source line (any frame) |
| `step_over` | | `{}`; stop when frame depth ≤ saved depth and line changes |
| `stack_trace` | | `{frames:[{index, function, function_index, file, line, resumed}]}` (innermost first) |
| `scopes` | `{frame_index}` | `{locals:[{name, type, value, ref?}]}` (`ref` present when expandable; `$`-prefixed compiler slots hidden) |
| `variables` | `{ref}` | `{variables:[{name, type, value, ref?}]}` |
| `evaluate` | `{frame_index, expression}` | `{value, type}` or `{error}` |
| `disassemble` | `{function_index?}` | `{functions:[...]}` defaulting to the paused frame's function; rows `{ip, text, line, current}` |
| `source` | `{file}` | `{content}` |
| `program` | | `{files:[...], functions:[{index, name, qualified_name, line_count}]}` |

Events: `stopped {reason, frame_index, function, ip, line}` (reason ∈
`entry | breakpoint | step | pause`), `output {text}` (print sink tap),
`exited {failed, result}`.

Variable refs are only valid while paused (they point at live stack slots /
heap objects); the server invalidates them on every resume.

## 4. Runtime changes

- `include/glscript/debug.hpp`: `DebugHook` interface —
  `on_instruction(Runtime&, frame_index, ip, span)`; plus `DebugState`
  structs for inspection. `runtime.hpp` gains `set_debug_hook` and a public
  debugger-access section (const frame/stack/handler/result accessors). No
  mutation of VM state is exposed to the debugger.
- Slot names: `hir::Function::local_names` filled by `lower.cpp declare()`
  (the single slot-allocation chokepoint) and copied through the emitter into
  `FunctionBytecode::local_names`. Compiler-generated slots keep their
  `$`-prefixed names so the debugger can filter them.

## 5. Disassembler

`include/glscript/disasm.hpp` + `src/disasm.cpp`: structured lines
`{ip, opcode, operand words, decoded text, source line}` from a
`FunctionBytecode`, walking `code` with `operand_count(op)`. Shared by
`glc --dump-bytecode` and the protocol's `disassemble`.

## 6. Evaluation

`src/dbg_eval.cpp`: a standalone recursive evaluator over the snippet's parsed
AST (lexer/parser reuse). Supports literals, identifiers (the paused frame's
named locals), array indexing, field reads, arithmetic/comparison operators,
`!`, unary `-`, parenthesized expressions. **No function calls** — the
interpreter cannot be re-entered while paused; a call in an evaluated
expression is a clean diagnostic. Locals are read through the debugger
accessors, never mutated.

## 7. glc

- `-r script.gl`: check → compile → `Runtime::run` (entry `main`), stdout
  print sink, `register_std` bindings.
- `--debug[=port]` (default 9000): binds, waits for the first client before
  running, emits `stopped {reason:"entry"}`.
- `--dump-bytecode`: disassembler dump.

## 8. GUI

`tools/debugger/`, NSH TSX template; `nsh.json` grants
`network.sockets: ["127.0.0.1:9000"]`. Toolbar (connect, continue, pause,
step-in, step-over, status), source panel (virtualized rows, gutter toggle,
current-line highlight, scroll-to-current-line), bytecode panel (function
selector, current-ip highlight), bottom row: stack trace, variables tree,
evaluate input + result.

## 9. Testing

`tests/debugger_test.cpp`: in-process end-to-end over real sockets — the
script runs under a `DebugServer` on a background thread, the test drives the
protocol from the main thread with a raw POSIX client, asserting entry stop,
breakpoint hit (line + stack), stepping semantics, locals, evaluate, and
disassemble. New ctest suite alongside the existing five.
