#include "glscript/bytecode.hpp"
#include "glscript/hir.hpp"

#include <cassert>
#include <unordered_map>

namespace glscript {
namespace {

// Emits bytecode from a lowered HIR program. All resolution already happened
// during lowering: this is a mechanical walk that maps each node to 1-2 ops,
// patches the structured-control-flow jumps, and builds constant pools.
class Emitter {
public:
    explicit Emitter(const hir::Program& program) : program_(program) {}

    CompiledProgram emit() {
        copy_tables();
        emit_function_bodies();
        return std::move(out_);
    }

private:
    FunctionBytecode& fn() { return out_.functions[state_->fn_index]; }

    // ------------------------------------------------------------------
    // Program tables — function indices are 1:1 with the HIR program.
    // ------------------------------------------------------------------

    void copy_tables() {
        out_.functions.resize(program_.functions.size());
        for (std::size_t i = 0; i < program_.functions.size(); ++i) {
            const auto& function = *program_.functions[i];
            auto& bytecode = out_.functions[i];
            bytecode.name = function.name;
            bytecode.qualified_name = function.qualified_name;
            bytecode.param_count = function.param_count;
            bytecode.local_count = function.local_count;
            bytecode.capture_count = function.capture_count;
        }
        out_.handler_tables.reserve(program_.handler_tables.size());
        for (const auto& table : program_.handler_tables) {
            HandlerTable bytecode_table;
            for (const auto& entry : table.entries) {
                HandlerEntry bytecode_entry;
                bytecode_entry.effect = entry.effect;
                bytecode_entry.operation = entry.operation;
                bytecode_entry.function = entry.function;
                bytecode_table.entries.push_back(std::move(bytecode_entry));
            }
            out_.handler_tables.push_back(std::move(bytecode_table));
        }
        out_.enum_variants = program_.enum_variants;
        out_.operators = program_.operators;
        out_.host_functions = program_.host_functions;
        out_.downcast_targets = program_.downcast_targets;
        out_.function_ids = program_.function_ids;
        out_.module_inits = program_.module_inits;
        out_.module_ambient_handlers = program_.module_ambient_handlers;
    }

    void emit_function_bodies() {
        for (std::uint32_t i = 0; i < program_.functions.size(); ++i) {
            FuncState state;
            state.fn_index = i;
            state_ = &state;
            for (const auto& statement : program_.functions[i]->body.items) {
                emit_statement(statement);
            }
        }
    }

    // ------------------------------------------------------------------
    // Emission helpers
    // ------------------------------------------------------------------

    // Spans are recorded per code word so the interpreter can look one up
    // directly by instruction pointer.
    std::uint32_t emit_word(std::uint32_t word) {
        auto& code = fn().code;
        code.push_back(word);
        fn().spans.push_back(current_span_);
        return static_cast<std::uint32_t>(code.size() - 1);
    }

    std::uint32_t here() const {
        return static_cast<std::uint32_t>(
            out_.functions[state_->fn_index].code.size());
    }

    void patch(std::uint32_t operand_position, std::uint32_t target) {
        fn().code[operand_position] = target;
    }

    std::uint32_t make_constant(Constant constant) {
        auto& constants = fn().constants;
        for (std::uint32_t i = 0; i < constants.size(); ++i) {
            if (constants[i].value == constant.value) {
                return i;
            }
        }
        constants.push_back(std::move(constant));
        return static_cast<std::uint32_t>(constants.size() - 1);
    }

    // The operand word count written here must agree with operand_count(),
    // which the interpreter uses to find instruction boundaries — a
    // disagreement between the two tables used to look like heap corruption.
    // The assert keeps them single-source.
    std::uint32_t emit_instruction(Op op, const SourceSpan& span,
                                   std::initializer_list<std::uint32_t> words = {}) {
        current_span_ = span;
        auto& code = fn().code;
        const auto instruction = static_cast<std::uint32_t>(code.size());
        code.push_back(static_cast<std::uint32_t>(op));
        fn().spans.push_back(span);
        for (const auto word : words) {
            emit_word(word);
        }
        assert(code.size() - instruction - 1 == operand_count(op));
        return instruction;
    }

    std::uint32_t emit_jump(Op op, const SourceSpan& span) {
        return emit_instruction(op, span, {0}) + 1; // operand position
    }

    // ------------------------------------------------------------------
    // Statements — lowering already inserted every Return.
    // ------------------------------------------------------------------

    void emit_statement(const hir::Stmt& statement) {
        switch (statement.kind) {
        case hir::Stmt::Kind::Expr:
            emit_expression(*statement.expression);
            emit_instruction(Op::Pop, statement.span);
            return;

        case hir::Stmt::Kind::VarLocal:
            emit_expression(*statement.expression);
            emit_instruction(Op::StoreLocal, statement.span, {statement.slot});
            return;

        case hir::Stmt::Kind::Assign:
            emit_assignment(statement);
            return;

        case hir::Stmt::Kind::Return:
            emit_expression(*statement.expression);
            emit_instruction(Op::Return, statement.span);
            return;

        case hir::Stmt::Kind::If: {
            emit_expression(*statement.expression);
            const auto else_jump = emit_jump(Op::JumpIfFalse, statement.span);
            emit_block(*statement.then_branch);
            if (statement.else_branch != nullptr) {
                const auto end_jump = emit_jump(Op::Jump, statement.span);
                patch(else_jump, here());
                emit_block(*statement.else_branch);
                patch(end_jump, here());
            } else {
                patch(else_jump, here());
            }
            return;
        }

        case hir::Stmt::Kind::While: {
            const auto condition_position = here();
            emit_expression(*statement.expression);
            const auto exit_jump = emit_jump(Op::JumpIfFalse, statement.span);

            FuncState::Loop loop;
            loop.continue_target = condition_position;
            state_->loops.push_back(loop);

            emit_block(*statement.body);

            loop = state_->loops.back();
            state_->loops.pop_back();

            emit_instruction(Op::Loop, statement.span, {condition_position});
            patch(exit_jump, here());
            for (const auto& break_jump : loop.break_jumps) {
                patch(break_jump, here());
            }
            return;
        }

        case hir::Stmt::Kind::Break:
            if (!state_->loops.empty()) {
                state_->loops.back().break_jumps.push_back(
                    emit_jump(Op::Jump, statement.span));
            }
            return;

        case hir::Stmt::Kind::Continue:
            if (!state_->loops.empty()) {
                emit_instruction(Op::Loop, statement.span,
                                 {state_->loops.back().continue_target});
            }
            return;
        }
    }

    void emit_block(const hir::Block& block) {
        for (const auto& statement : block.items) {
            emit_statement(statement);
        }
    }

    void emit_assignment(const hir::Stmt& statement) {
        if (statement.target == nullptr) {
            // Assigning a captured (by-value) variable is not supported;
            // discard the value to keep the stack balanced.
            emit_instruction(Op::Pop, statement.span);
            return;
        }

        switch (statement.target->kind) {
        case hir::Expr::Kind::Local:
            emit_expression(*statement.expression);
            emit_instruction(Op::StoreLocal, statement.span, {statement.target->slot});
            return;
        case hir::Expr::Kind::Field:
            emit_expression(*statement.target->object);
            emit_expression(*statement.expression);
            emit_instruction(Op::SetField, statement.span, {statement.target->field_slot});
            return;
        case hir::Expr::Kind::FieldDynamic:
            emit_expression(*statement.target->object);
            emit_expression(*statement.expression);
            emit_instruction(Op::SetFieldByName, statement.span, {make_constant(
                Constant{statement.target->field_name})});
            return;
        case hir::Expr::Kind::ArrayGet:
            // arr[i] = v: object, index, value — ArraySet pops value/index/
            // object and leaves the op result (unit, or the handler's value
            // when an OutOfBounds raise was handled), which is discarded.
            emit_expression(*statement.target->object);
            emit_call_arguments(*statement.target);
            emit_expression(*statement.expression);
            emit_instruction(Op::ArraySet, statement.span);
            emit_instruction(Op::Pop, statement.span);
            return;
        default:
            assert(false && "invalid assignment target kind");
            return;
        }
    }

    // ------------------------------------------------------------------
    // Expressions
    // ------------------------------------------------------------------

    void emit_expression(const hir::Expr& expression) {
        switch (expression.kind) {
        case hir::Expr::Kind::Unit:
            emit_instruction(Op::LoadConst, expression.span,
                             {make_constant(Constant{std::monostate{}})});
            return;

        case hir::Expr::Kind::Bool:
            emit_instruction(Op::LoadConst, expression.span,
                             {make_constant(Constant{expression.bool_value})});
            return;

        case hir::Expr::Kind::Int:
            emit_instruction(Op::LoadConst, expression.span,
                             {make_constant(Constant{expression.int_value})});
            return;

        case hir::Expr::Kind::Float:
            emit_instruction(Op::LoadConst, expression.span,
                             {make_constant(Constant{expression.float_value})});
            return;

        case hir::Expr::Kind::String:
            emit_instruction(Op::LoadConst, expression.span,
                             {make_constant(Constant{expression.string_value})});
            return;

        case hir::Expr::Kind::Local:
            emit_instruction(Op::LoadLocal, expression.span, {expression.slot});
            return;

        case hir::Expr::Kind::Capture:
            emit_instruction(Op::LoadCapture, expression.span, {expression.slot});
            return;

        case hir::Expr::Kind::Field:
            emit_expression(*expression.object);
            emit_instruction(Op::GetField, expression.span, {expression.field_slot});
            return;

        case hir::Expr::Kind::FieldDynamic:
            emit_expression(*expression.object);
            emit_instruction(Op::GetFieldByName, expression.span, {make_constant(
                Constant{expression.field_name})});
            return;

        case hir::Expr::Kind::EnumVariant:
            emit_instruction(Op::EnumVariant, expression.span, {expression.variant_id});
            return;

        case hir::Expr::Kind::EnumPayload:
            emit_expression(*expression.object);
            emit_instruction(Op::EnumPayload, expression.span, {expression.payload_index});
            return;

        case hir::Expr::Kind::ArrayGet:
            emit_expression(*expression.object);
            emit_call_arguments(expression); // the index
            emit_instruction(Op::ArrayGet, expression.span);
            return;

        case hir::Expr::Kind::ArrayNew:
            emit_call_arguments(expression); // the elements
            emit_instruction(Op::ArrayNew, expression.span,
                             {static_cast<std::uint32_t>(expression.args.size())});
            return;

        case hir::Expr::Kind::Match: {
            // Arm dispatch: the emitter's version of the lowered test plans.
            // Tests and guards jump to the next arm on failure; a passing arm
            // leaves its body's value and jumps past the chain.
            emit_expression(*expression.scrutinee);
            emit_instruction(Op::StoreLocal, expression.span, {expression.scrutinee_slot});

            std::vector<std::uint32_t> end_jumps;
            for (const auto& arm : expression.match_arms) {
                std::vector<std::uint32_t> next_jumps;
                for (const auto& test : arm.tests) {
                    emit_match_term(test, expression.span);
                    switch (test.kind) {
                    case hir::MatchTest::Kind::Variant:
                        emit_instruction(Op::EnumIsVariant, arm.span, {test.variant_id});
                        next_jumps.push_back(emit_jump(Op::JumpIfFalse, arm.span));
                        break;
                    case hir::MatchTest::Kind::Literal:
                        emit_expression(*test.literal);
                        emit_instruction(Op::Eq, arm.span);
                        next_jumps.push_back(emit_jump(Op::JumpIfFalse, arm.span));
                        break;
                    case hir::MatchTest::Kind::Bind:
                        emit_instruction(Op::StoreLocal, arm.span, {test.slot});
                        break;
                    }
                }
                if (arm.guard != nullptr) {
                    emit_expression(*arm.guard);
                    next_jumps.push_back(emit_jump(Op::JumpIfFalse, arm.span));
                }
                emit_expression(*arm.body);
                end_jumps.push_back(emit_jump(Op::Jump, arm.span));
                for (const auto jump : next_jumps) {
                    patch(jump, here());
                }
            }
            // No arm matched — reachable only for dynamic scrutinees.
            emit_instruction(Op::Abort, expression.span);
            for (const auto jump : end_jumps) {
                patch(jump, here());
            }
            return;
        }

        case hir::Expr::Kind::IntrinsicBinary:
            emit_expression(*expression.lhs);
            emit_expression(*expression.rhs);
            emit_instruction(intrinsic_op(expression.binary_op), expression.span);
            return;

        case hir::Expr::Kind::IntrinsicUnary:
            emit_expression(*expression.object);
            emit_instruction(expression.unary_op == hir::UnaryOp::Neg ? Op::Neg : Op::Not,
                             expression.span);
            return;

        case hir::Expr::Kind::ShortCircuit: {
            emit_expression(*expression.lhs);
            emit_instruction(Op::Dup, expression.span);
            const auto short_jump = emit_jump(
                expression.short_circuit_is_and ? Op::JumpIfFalse : Op::JumpIfTrue,
                expression.span);
            emit_instruction(Op::Pop, expression.span);
            emit_expression(*expression.rhs);
            patch(short_jump, here());
            return;
        }

        case hir::Expr::Kind::OperatorCall:
            emit_call_arguments(expression);
            emit_instruction(Op::CallOperator, expression.span,
                             {expression.operator_id,
                              static_cast<std::uint32_t>(expression.args.size())});
            return;

        case hir::Expr::Kind::Call:
            emit_call_arguments(expression);
            if (expression.call_target == hir::Expr::CallTarget::HostFn) {
                // The receiver is part of the thunk's argument list.
                emit_instruction(Op::CallHost, expression.span,
                                 {expression.fn_id,
                                  static_cast<std::uint32_t>(
                                      expression.args.size() +
                                      (expression.receiver != nullptr ? 1u : 0u))});
            } else {
                emit_instruction(Op::CallFunction, expression.span,
                                 {expression.fn_id,
                                  static_cast<std::uint32_t>(
                                      expression.args.size() +
                                      (expression.receiver != nullptr ? 1u : 0u))});
            }
            return;

        case hir::Expr::Kind::CallValue:
            emit_expression(*expression.object);
            emit_call_arguments(expression);
            emit_instruction(Op::Call, expression.span,
                             {static_cast<std::uint32_t>(expression.args.size())});
            return;

        case hir::Expr::Kind::Raise:
            emit_call_arguments(expression);
            emit_instruction(Op::Raise, expression.span,
                             {make_constant(Constant{expression.effect}),
                              make_constant(Constant{expression.operation}),
                              static_cast<std::uint32_t>(expression.args.size())});
            return;

        case hir::Expr::Kind::EnumNew:
            emit_call_arguments(expression);
            emit_instruction(Op::EnumCtor, expression.span,
                             {expression.variant_id,
                              static_cast<std::uint32_t>(expression.args.size())});
            return;

        case hir::Expr::Kind::Downcast:
            emit_expression(*expression.object);
            emit_instruction(Op::Downcast, expression.span, {expression.fn_id});
            return;

        case hir::Expr::Kind::BuildString:
            for (const auto& part : expression.parts) {
                emit_expression(*part);
            }
            emit_instruction(Op::BuildString, expression.span,
                             {static_cast<std::uint32_t>(expression.parts.size())});
            return;

        case hir::Expr::Kind::Closure:
            for (const auto& capture : expression.captures) {
                emit_instruction(capture.is_local ? Op::LoadLocal : Op::LoadCapture,
                                 expression.span, {capture.index});
            }
            emit_instruction(Op::Closure, expression.span,
                             {expression.closure_fn,
                              static_cast<std::uint32_t>(expression.captures.size())});
            return;

        case hir::Expr::Kind::Resume:
            emit_expression(*expression.continuation);
            emit_expression(*expression.resume_value);
            emit_instruction(Op::Resume, expression.span);
            return;

        case hir::Expr::Kind::Handle:
            // The handled body is its own function (lowering desugaring): a
            // raise inside it is exactly one frame above this delimiter. The
            // body is invoked as a closure so it can capture outer locals.
            emit_instruction(Op::PushHandler, expression.span, {expression.table_ref});
            for (const auto& capture : expression.captures) {
                emit_instruction(capture.is_local ? Op::LoadLocal : Op::LoadCapture,
                                 expression.span, {capture.index});
            }
            emit_instruction(Op::Closure, expression.span,
                             {expression.body_fn,
                              static_cast<std::uint32_t>(expression.captures.size())});
            emit_instruction(Op::Call, expression.span, {0});
            emit_instruction(Op::PopHandler, expression.span);
            return;

        case hir::Expr::Kind::Abort:
            emit_call_arguments(expression);
            emit_instruction(Op::Abort, expression.span);
            return;
        }
    }

    void emit_call_arguments(const hir::Expr& expression) {
        if (expression.receiver != nullptr) {
            emit_expression(*expression.receiver);
        }
        for (const auto& argument : expression.args) {
            emit_expression(*argument);
        }
    }

    // The value a match test operates on: the scrutinee local, or a payload
    // slot of an enum value bound by an enclosing variant test.
    void emit_match_term(const hir::MatchTest& test, const SourceSpan& span) {
        emit_instruction(Op::LoadLocal, span, {test.value_slot});
        if (test.term == hir::MatchTest::Term::Payload) {
            emit_instruction(Op::EnumPayload, span, {test.payload_index});
        }
    }

    static Op intrinsic_op(hir::BinaryOp op) {
        switch (op) {
        case hir::BinaryOp::Add: return Op::Add;
        case hir::BinaryOp::Sub: return Op::Sub;
        case hir::BinaryOp::Mul: return Op::Mul;
        case hir::BinaryOp::Div: return Op::Div;
        case hir::BinaryOp::Mod: return Op::Mod;
        case hir::BinaryOp::Eq: return Op::Eq;
        case hir::BinaryOp::Ne: return Op::Ne;
        case hir::BinaryOp::Lt: return Op::Lt;
        case hir::BinaryOp::Le: return Op::Le;
        case hir::BinaryOp::Gt: return Op::Gt;
        case hir::BinaryOp::Ge: return Op::Ge;
        }
        return Op::Add;
    }

    // Per-function emission state: only what jump patching needs. Loop
    // bookkeeping moved here from the old compiler because HIR control flow
    // is structured — lowering has no loop stack.
    struct FuncState {
        std::uint32_t fn_index = 0;

        struct Loop {
            std::uint32_t continue_target = 0;      // absolute code position
            std::vector<std::uint32_t> break_jumps; // operand positions to patch
        };
        std::vector<Loop> loops;
    };

    const hir::Program& program_;
    CompiledProgram out_;
    FuncState* state_ = nullptr;
    SourceSpan current_span_;
};

} // namespace

namespace hir {

CompiledProgram emit(const Program& program) {
    Emitter emitter(program);
    return emitter.emit();
}

} // namespace hir

// Number of operand words that follow each opcode. The emitter's
// emit_instruction() asserts against this on every instruction, so the two
// can never silently disagree.
std::size_t operand_count(Op op) {
    switch (op) {
    case Op::LoadConst:
    case Op::LoadLocal:
    case Op::StoreLocal:
    case Op::LoadCapture:
    case Op::Jump:
    case Op::JumpIfFalse:
    case Op::JumpIfTrue:
    case Op::Loop:
    case Op::Call:
    case Op::PushHandler:
    case Op::EnumVariant:
    case Op::EnumIsVariant:
    case Op::EnumPayload:
    case Op::ArrayNew:
    case Op::GetField:
    case Op::SetField:
    case Op::GetFieldByName:
    case Op::SetFieldByName:
    case Op::Downcast:
    case Op::BuildString:
        return 1;
    case Op::CallFunction:
    case Op::CallHost:
    case Op::CallOperator:
    case Op::Closure:
    case Op::EnumCtor:
        return 2;
    case Op::Raise:
        return 3;
    case Op::Add:
    case Op::Sub:
    case Op::Mul:
    case Op::Div:
    case Op::Mod:
    case Op::Neg:
    case Op::Not:
    case Op::Eq:
    case Op::Ne:
    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge:
    case Op::Return:
    case Op::Resume:
    case Op::PopHandler:
    case Op::Abort:
    case Op::Dup:
    case Op::Pop:
        return 0;
    }
    return 0;
}

CompiledProgram compile_program(const Program& program) {
    return hir::emit(hir::lower(program));
}

} // namespace glscript
