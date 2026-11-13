#pragma once

#include "glscript/bytecode.hpp"
#include "glscript/source.hpp"

#include <cstdint>

namespace glscript {

class Runtime;

// A debugger's read-only view of one interpreter frame. Indices reference
// live interpreter state (Runtime::debug_frame rebuilds it), so views are
// meaningful only while the runtime is paused at a hook.
struct DebugFrame {
    std::uint32_t function = 0;  // index into CompiledProgram::functions
    const FunctionBytecode* fn = nullptr;
    std::uint32_t ip = 0;        // opcode-word index of the pending instruction
    std::uint32_t base = 0;      // locals live at stack[base .. base + local_count)
    std::uint32_t local_count = 0;
    bool resumed_slice = false;  // installed by a continuation resume splice
};

// Observe-before-execute hook for the interpreter dispatch loop. The runtime
// invokes it before every instruction while one is installed (see
// Runtime::set_debug_hook); when unset the call is a null check.
//
// The hook may block (a debug server pumps its protocol inside a pause), but
// must never re-enter the runtime: the interpreter is not reentrant, and the
// machine state it observes is only coherent while it stays paused.
class DebugHook {
public:
    virtual ~DebugHook() = default;

    // Called before the instruction at frame.ip executes; span is that
    // instruction's source position.
    virtual void on_instruction(Runtime& runtime, const DebugFrame& frame,
                                const SourceSpan& span) = 0;
};

} // namespace glscript
