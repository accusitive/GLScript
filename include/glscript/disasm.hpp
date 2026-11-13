#pragma once

#include "glscript/bytecode.hpp"

#include <string>
#include <vector>

namespace glscript {

// One rendered instruction of a bytecode listing.
struct DisasmLine {
    std::uint32_t ip = 0;    // opcode-word index into FunctionBytecode::code
    std::uint32_t size = 1;  // words occupied (opcode + operands)
    std::string text;        // rendered instruction, operands decoded
    std::uint32_t line = 0;  // 1-based source line (0 when the span has none)
};

// Disassembles one function, decoding operands against the program's tables
// (constants, function/host/operator/enum-variant indices, handler tables,
// local slot names). Instruction boundaries follow operand_count(op).
std::vector<DisasmLine> disassemble(const FunctionBytecode& function,
                                    const CompiledProgram& program);

} // namespace glscript
