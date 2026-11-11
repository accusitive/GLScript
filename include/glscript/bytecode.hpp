#pragma once

#include "glscript/source.hpp"
#include "glscript/symbols.hpp"
#include "glscript/type.hpp"
#include "glscript/value.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace glscript {

// ---------------------------------------------------------------------------
// Bytecode
//
// Word-addressed: the code vector holds one opcode word followed by that
// instruction's operand words. Jump targets are absolute code indices patched
// at compile time.
// ---------------------------------------------------------------------------

enum class Op : std::uint32_t {
    LoadConst,     // c                        push constants[c]
    LoadLocal,     // slot                     push stack[base + slot]
    StoreLocal,    // slot                     stack[base + slot] = pop
    LoadCapture,   // i                        push captures[i]

    Add,           // a b -> (a+b)             int/float arithmetic, string concat
    Sub, Mul, Div, Mod, Neg, Not,
    Eq, Ne, Lt, Le, Gt, Ge,        // a b -> bool

    Jump,          // target                   ip = target
    JumpIfFalse,   // target                   pop; if falsy ip = target
    JumpIfTrue,    // target                   pop; if truthy ip = target
    Loop,          // target                   ip = target (backwards)

    Call,          // argc                     callee+args on stack -> invoke closure
    CallFunction,  // fn argc                  direct call to compiled function
    CallHost,      // hostfn argc              call a registered host thunk
    CallOperator,  // op argc                  call an overloaded operator thunk
    Raise,         // effect op argc           invoke/raise an effect operation
    Resume,        //                          continuation + value on stack -> splice
    Return,        //                          return top value to caller frame

    PushHandler,   // table                    install handler table
    PopHandler,    //                          uninstall innermost handler

    EnumVariant,   // variant                  push payload-less enum value
    EnumCtor,      // variant argc             payload on stack -> enum value
    EnumIsVariant, // variant                  pop value; push bool (variant match)
    EnumPayload,   // index                    pop enum value; push payload[index]
    ArrayNew,      // argc                     elements on stack -> array value
    ArrayGet,      //                          pop index, array -> element; raises
                   //                          OutOfBounds::index when invalid
    ArraySet,      //                          pop value, index, array -> write or
                   //                          raise; pushes the op result
    GetField,      // slot                     object.field[slot]
    SetField,      // slot                     object + value on stack
    GetFieldByName,// name                     dynamic member read
    SetFieldByName,// name                     dynamic member write
    Downcast,      // target                   checked narrowing; raises on failure
    BuildString,   // argc                     parts on stack -> template string
    Closure,       // fn captures              capture values on stack -> closure
    Abort,         //                          fatal abort (builtin `abort`)
    Dup, Pop,
};

// Number of operand words that follow each opcode.
std::size_t operand_count(Op op);

struct Constant {
    std::variant<std::monostate, bool, std::int64_t, double, std::string> value;
};

struct FunctionBytecode {
    std::string name;
    std::string qualified_name;
    std::uint32_t param_count = 0;   // receiver is slot 0 for methods
    std::uint32_t local_count = 0;   // total slots incl. params
    std::uint32_t capture_count = 0;
    std::vector<std::uint32_t> code;
    std::vector<Constant> constants;
    std::vector<SourceSpan> spans;   // one per instruction
};

struct HandlerEntry {
    std::string effect;          // qualified effect name
    std::string operation;
    std::uint32_t function = 0;  // compiled handler body (params, then continuation slot)
};

struct HandlerTable {
    std::vector<HandlerEntry> entries;
};

// Everything the interpreter needs to execute a checked program. All pointers
// reference symbols inside the Program, which must outlive this object.
struct CompiledProgram {
    std::vector<FunctionBytecode> functions;
    std::vector<HandlerTable> handler_tables;
    std::vector<const EnumVariantSymbol*> enum_variants;
    std::vector<const OperatorInfo*> operators;
    std::vector<const FunctionSymbol*> host_functions;
    std::vector<TypePtr> downcast_targets;
    std::unordered_map<const FunctionSymbol*, std::uint32_t> function_ids;
    std::unordered_map<std::string, std::uint32_t> module_inits; // module -> init function
    // Top-level `handle Effect { ... }` declarations: ambient handlers
    // installed for the whole entry-module execution.
    std::unordered_map<std::string, std::vector<std::uint32_t>> module_ambient_handlers;

    const FunctionBytecode* function(std::uint32_t index) const {
        return &functions[index];
    }
};

// Compiles every script function body in the checked program.
CompiledProgram compile_program(const Program& program);

} // namespace glscript
