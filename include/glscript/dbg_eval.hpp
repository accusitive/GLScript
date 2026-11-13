#pragma once

#include "glscript/debug.hpp"
#include "glscript/value.hpp"

#include <string>

namespace glscript {

class Runtime;

// Evaluates a debugger expression snippet against a paused frame. Supports
// literals, identifiers (the frame's named locals), array indexing, field
// reads, arithmetic/comparison/logical operators, and unary -, !. Function
// calls are unsupported (the interpreter is not reentrant while paused) and
// produce an error rather than a value. Globals are not reachable either —
// top-level lets live in the module-init frame, not in machine state.
//
// On success returns the value and leaves `error` empty; on failure returns
// unit and sets `error`.
Value evaluate_expression(Runtime& runtime, const DebugFrame& frame,
                          const std::string& expression, std::string& error);

} // namespace glscript
