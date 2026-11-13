#include "glscript/disasm.hpp"

#include <cstdio>

namespace glscript {
namespace {

std::string render_constant(const Constant& constant) {
    if (std::holds_alternative<std::monostate>(constant.value)) {
        return "unit";
    }
    if (const auto* boolean = std::get_if<bool>(&constant.value)) {
        return *boolean ? "true" : "false";
    }
    if (const auto* integer = std::get_if<std::int64_t>(&constant.value)) {
        return std::to_string(*integer);
    }
    if (const auto* real = std::get_if<double>(&constant.value)) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.10g", *real);
        return buffer;
    }
    if (const auto* text = std::get_if<std::string>(&constant.value)) {
        return "\"" + *text + "\"";
    }
    return "?";
}

class Disassembler {
public:
    Disassembler(const FunctionBytecode& function, const CompiledProgram& program)
        : function_(function), program_(program) {}

    std::vector<DisasmLine> run() {
        std::vector<DisasmLine> lines;
        std::uint32_t ip = 0;
        while (ip < function_.code.size()) {
            const auto op = static_cast<Op>(function_.code[ip]);
            const std::uint32_t size = 1 + static_cast<std::uint32_t>(operand_count(op));
            DisasmLine line;
            line.ip = ip;
            line.size = size;
            line.text = render(op, ip);
            if (ip < function_.spans.size()) {
                line.line = function_.spans[ip].file
                                ? function_.spans[ip].file->location(function_.spans[ip].begin).line
                                : 0;
            }
            lines.push_back(std::move(line));
            ip += size;
        }
        return lines;
    }

private:
    // Operand word accessors relative to the current instruction.
    std::uint32_t word(std::uint32_t index) const { return function_.code.at(index); }
    const Constant& constant(std::uint32_t index) const {
        return function_.constants.at(word(index));
    }
    std::string local_name(std::uint32_t slot) const {
        if (slot < function_.local_names.size()) {
            return function_.local_names[slot];
        }
        return "?" + std::to_string(slot);
    }

    std::string render(Op op, std::uint32_t ip) {
        const std::uint32_t* operands = function_.code.data() + ip + 1;
        switch (op) {
        case Op::LoadConst:
            return "LoadConst " + render_constant(constant(ip + 1));
        case Op::LoadLocal:
            return "LoadLocal " + local_name(operands[0]) +
                   " (slot " + std::to_string(operands[0]) + ")";
        case Op::StoreLocal:
            return "StoreLocal " + local_name(operands[0]) +
                   " (slot " + std::to_string(operands[0]) + ")";
        case Op::LoadCapture:
            return "LoadCapture [" + std::to_string(operands[0]) + "]";
        case Op::Jump:
        case Op::JumpIfFalse:
        case Op::JumpIfTrue:
        case Op::Loop:
            return opcode_name(op) + " -> " + std::to_string(operands[0]);
        case Op::Call:
            return "Call argc=" + std::to_string(operands[0]);
        case Op::CallFunction:
            return "CallFunction " + callee_name(operands[0]) +
                   " argc=" + std::to_string(operands[1]);
        case Op::CallHost:
            return "CallHost " + host_name(operands[0]) +
                   " argc=" + std::to_string(operands[1]);
        case Op::CallOperator:
            return "CallOperator " + operator_name(operands[0]) +
                   " argc=" + std::to_string(operands[1]);
        case Op::Raise:
            return "Raise " + constant_text(ip + 1) + "." + constant_text(ip + 2) +
                   " argc=" + std::to_string(operands[2]);
        case Op::Resume:
            return "Resume";
        case Op::Return:
            return "Return";
        case Op::PushHandler:
            return "PushHandler " + handler_name(operands[0]);
        case Op::PopHandler:
            return "PopHandler";
        case Op::EnumVariant:
            return "EnumVariant " + variant_name(operands[0]);
        case Op::EnumCtor:
            return "EnumCtor " + variant_name(operands[0]) +
                   " argc=" + std::to_string(operands[1]);
        case Op::EnumIsVariant:
            return "EnumIsVariant " + variant_name(operands[0]);
        case Op::EnumPayload:
            return "EnumPayload [" + std::to_string(operands[0]) + "]";
        case Op::ArrayNew:
            return "ArrayNew argc=" + std::to_string(operands[0]);
        case Op::ArrayGet:
            return "ArrayGet";
        case Op::ArraySet:
            return "ArraySet";
        case Op::GetField:
            return "GetField slot " + std::to_string(operands[0]);
        case Op::SetField:
            return "SetField slot " + std::to_string(operands[0]);
        case Op::GetFieldByName:
            return "GetFieldByName " + constant_text(ip + 1);
        case Op::SetFieldByName:
            return "SetFieldByName " + constant_text(ip + 1);
        case Op::Downcast:
            return "Downcast " + type_name(operands[0]);
        case Op::BuildString:
            return "BuildString argc=" + std::to_string(operands[0]);
        case Op::Closure:
            return "Closure " + callee_name(operands[0]) +
                   " captures=" + std::to_string(operands[1]);
        case Op::Abort:
            return "Abort";
        case Op::Dup:
            return "Dup";
        case Op::Pop:
            return "Pop";
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
        case Op::Neg: case Op::Not:
        case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le:
        case Op::Gt: case Op::Ge:
            return opcode_name(op);
        }
        return "Op(" + std::to_string(static_cast<std::uint32_t>(op)) + ")";
    }

    static std::string opcode_name(Op op) {
        switch (op) {
        case Op::Add: return "Add";
        case Op::Sub: return "Sub";
        case Op::Mul: return "Mul";
        case Op::Div: return "Div";
        case Op::Mod: return "Mod";
        case Op::Neg: return "Neg";
        case Op::Not: return "Not";
        case Op::Eq: return "Eq";
        case Op::Ne: return "Ne";
        case Op::Lt: return "Lt";
        case Op::Le: return "Le";
        case Op::Gt: return "Gt";
        case Op::Ge: return "Ge";
        case Op::Jump: return "Jump";
        case Op::JumpIfFalse: return "JumpIfFalse";
        case Op::JumpIfTrue: return "JumpIfTrue";
        case Op::Loop: return "Loop";
        default: return "?";
        }
    }

    std::string callee_name(std::uint32_t index) const {
        if (index < program_.functions.size()) {
            return program_.functions[index].qualified_name;
        }
        return "fn" + std::to_string(index);
    }

    std::string host_name(std::uint32_t index) const {
        if (index < program_.host_functions.size() && program_.host_functions[index]) {
            return program_.host_functions[index]->qualified_name;
        }
        return "host" + std::to_string(index);
    }

    std::string operator_name(std::uint32_t index) const {
        if (index < program_.operators.size() && program_.operators[index]) {
            return program_.operators[index]->op;
        }
        return "op" + std::to_string(index);
    }

    std::string variant_name(std::uint32_t index) const {
        if (index < program_.enum_variants.size() && program_.enum_variants[index]) {
            const auto* variant = program_.enum_variants[index];
            if (variant->owner != nullptr) {
                return variant->owner->name + "." + variant->name;
            }
            return variant->name;
        }
        return "variant" + std::to_string(index);
    }

    std::string type_name(std::uint32_t index) const {
        if (index < program_.downcast_targets.size() && program_.downcast_targets[index]) {
            return program_.downcast_targets[index]->to_string();
        }
        return "type" + std::to_string(index);
    }

    std::string handler_name(std::uint32_t index) const {
        if (index >= program_.handler_tables.size()) {
            return "table" + std::to_string(index);
        }
        std::string text = "[";
        bool first = true;
        for (const auto& entry : program_.handler_tables[index].entries) {
            if (!first) {
                text += ", ";
            }
            first = false;
            text += entry.effect + "." + entry.operation + " -> fn" +
                    std::to_string(entry.function);
        }
        return text + "]";
    }

    std::string constant_text(std::uint32_t word_index) const {
        return render_constant(constant(word_index));
    }

    const FunctionBytecode& function_;
    const CompiledProgram& program_;
};

} // namespace

std::vector<DisasmLine> disassemble(const FunctionBytecode& function,
                                    const CompiledProgram& program) {
    Disassembler disassembler(function, program);
    return disassembler.run();
}

} // namespace glscript
