#include "glscript/bytecode.hpp"

#include <algorithm>
#include <unordered_map>

namespace glscript {
namespace {

constexpr const char* kContinuationSlot = "$continuation";
constexpr const char* kReceiverSlot = "this";

struct Capture {
    bool is_local = false;
    std::uint32_t index = 0;
};

// Per-function compilation state. Functions are referenced by index into
// CompiledProgram::functions because the vector reallocates as lambdas and
// handler bodies are appended during compilation.
struct FuncState {
    std::uint32_t fn_index = 0;
    FuncState* enclosing = nullptr;
    const TypeSymbol* receiver_type = nullptr;
    std::int32_t resume_slot = -1;

    std::vector<std::unordered_map<std::string, std::uint32_t>> scopes;
    std::uint32_t local_count = 0;
    std::vector<Capture> captures;

    struct Loop {
        std::uint32_t continue_target = 0;     // absolute code position
        std::vector<std::uint32_t> break_jumps; // operand positions to patch
    };
    std::vector<Loop> loops;

    std::int32_t find_local(const std::string& name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            const auto found = it->find(name);
            if (found != it->end()) {
                return static_cast<std::int32_t>(found->second);
            }
        }
        return -1;
    }

    std::uint32_t declare(const std::string& name, FunctionBytecode& fn) {
        const auto slot = local_count++;
        scopes.back().emplace(name, slot);
        fn.local_count = local_count;
        return slot;
    }
};

// Compiles the last statement of a block with tail semantics: an expression
// statement in tail position returns its value. Used for lambda bodies and
// effect handler cases (where a trailing `resume` yields the continuation's
// completion value as the handler result).
bool is_tail_expression(const ast::BlockStmt& block) {
    return !block.statements.empty() &&
           dynamic_cast<const ast::ExprStmt*>(block.statements.back().get()) != nullptr;
}

std::size_t operand_count_impl(Op op) {
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
    case Op::CallFunction:
    case Op::CallHost:
    case Op::CallOperator:
    case Op::PushHandler:
    case Op::EnumVariant:
    case Op::GetField:
    case Op::SetField:
    case Op::GetFieldByName:
    case Op::SetFieldByName:
    case Op::Downcast:
    case Op::BuildString:
        return 1;
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

class Compiler {
public:
    explicit Compiler(const Program& program) : program_(program) {}

    CompiledProgram compile() {
        register_script_functions();
        compile_script_functions();
        return std::move(output_);
    }

private:
    FunctionBytecode& fn() { return output_.functions[state_->fn_index]; }

    // ------------------------------------------------------------------
    // Emission helpers
    // ------------------------------------------------------------------

    std::uint32_t emit(Op op, const SourceSpan& span) {
        auto& code = fn().code;
        code.push_back(static_cast<std::uint32_t>(op));
        fn().spans.push_back(span);
        return static_cast<std::uint32_t>(code.size() - 1);
    }

    std::uint32_t emit_word(std::uint32_t word) {
        fn().code.push_back(word);
        return static_cast<std::uint32_t>(fn().code.size() - 1);
    }

    std::uint32_t here() const {
        return static_cast<std::uint32_t>(
            output_.functions[state_->fn_index].code.size());
    }

    std::uint32_t emit_jump(Op op, const SourceSpan& span) {
        const auto instruction = emit(op, span);
        emit_word(0);
        return instruction + 1; // operand position
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

    void emit_load_const(Constant constant, const SourceSpan& span) {
        emit(Op::LoadConst, span);
        emit_word(make_constant(std::move(constant)));
    }

    // ------------------------------------------------------------------
    // Function registration (indices must exist before bodies compile)
    // ------------------------------------------------------------------

    void register_script_functions() {
        for (const auto& [name, module] : program_.modules) {
            (void)name;
            if (module->is_host) {
                continue;
            }
            for (const auto& [_, function] : module->functions) {
                register_function(function);
            }
            for (const auto& [_, type] : module->types) {
                for (const auto& [__, method] : type->methods) {
                    register_function(method);
                }
            }
        }
    }

    void register_function(const FunctionSymbol* function) {
        if (function->is_host || function->is_effect_operation) {
            return;
        }
        if (function->declaration == nullptr || function->declaration->body == nullptr) {
            return;
        }
        new_function(function->name, function->qualified_name);
        output_.function_ids[function] =
            static_cast<std::uint32_t>(output_.functions.size() - 1);
    }

    std::uint32_t new_function(std::string name, std::string qualified_name) {
        FunctionBytecode fn;
        fn.name = std::move(name);
        fn.qualified_name = std::move(qualified_name);
        output_.functions.push_back(std::move(fn));
        return static_cast<std::uint32_t>(output_.functions.size() - 1);
    }

    // ------------------------------------------------------------------
    // Function bodies
    // ------------------------------------------------------------------

    void compile_script_functions() {
        for (const auto& [name, module] : program_.modules) {
            (void)name;
            if (module->is_host) {
                continue;
            }
            for (const auto& [_, function] : module->functions) {
                compile_registered(function, nullptr);
            }
            for (const auto& [_, type] : module->types) {
                for (const auto& [__, method] : type->methods) {
                    compile_registered(method, type);
                }
            }
            compile_module_init(*module);
        }
    }

    void compile_registered(const FunctionSymbol* function, const TypeSymbol* receiver) {
        const auto it = output_.function_ids.find(function);
        if (it == output_.function_ids.end()) {
            return;
        }

        FuncState state;
        state.fn_index = it->second;
        state.receiver_type = receiver;
        state.scopes.emplace_back();
        state_ = &state;
        FunctionBytecode& fn = this->fn();

        if (receiver != nullptr) {
            state.declare(kReceiverSlot, fn);
        }
        for (const auto& parameter : function->parameter_names) {
            state.declare(parameter, fn);
        }
        fn.param_count = state.local_count;

        compile_body(*function->declaration->body, function->span);

        state_ = state.enclosing;
    }

    // Handler case bodies: operation parameters, then an implicit
    // continuation slot that `resume` reads.
    std::uint32_t compile_handler_case(const ast::HandlerCase& handler_case) {
        const auto fn_index =
            new_function(handler_case.operation + " (handler)",
                         handler_case.effect + "::" + handler_case.operation);

        FuncState state;
        state.fn_index = fn_index;
        state.scopes.emplace_back();
        state_ = &state;
        FunctionBytecode& fn = this->fn();

        for (const auto& parameter : handler_case.parameters) {
            state.declare(parameter.name, fn);
        }
        fn.param_count = state.local_count;
        state.resume_slot = static_cast<std::int32_t>(state.declare(kContinuationSlot, fn));

        if (handler_case.body != nullptr) {
            compile_body(*handler_case.body, handler_case.span);
        } else {
            emit_load_const(Constant{std::monostate{}}, handler_case.span);
            emit(Op::Return, handler_case.span);
        }

        state_ = state.enclosing;
        return fn_index;
    }

    // Module init: evaluates top-level `handle ... with { ... }` declarations.
    void compile_module_init(const ModuleInfo& module) {
        bool has_handler = false;
        for (const auto& declaration : module.unit->declarations) {
            if (dynamic_cast<const ast::HandlerDecl*>(declaration.get()) != nullptr) {
                has_handler = true;
            }
        }
        if (!has_handler) {
            return;
        }

        const auto fn_index = new_function("(module init)", module.name);

        FuncState state;
        state.fn_index = fn_index;
        state.scopes.emplace_back();
        state_ = &state;

        for (const auto& declaration : module.unit->declarations) {
            const auto* handler_decl = dynamic_cast<const ast::HandlerDecl*>(declaration.get());
            if (handler_decl == nullptr || handler_decl->handled_expression == nullptr) {
                continue;
            }
            const auto table =
                register_handler_table(handler_decl->effect, handler_decl->cases);
            emit(Op::PushHandler, handler_decl->span);
            emit_word(table);
            compile_expression(*handler_decl->handled_expression);
            emit(Op::Pop, handler_decl->span);
            emit(Op::PopHandler, handler_decl->span);
        }

        emit_load_const(Constant{std::monostate{}}, {});
        emit(Op::Return, {});
        state_ = state.enclosing;
        output_.module_inits[module.name] = fn_index;
    }

    std::uint32_t register_handler_table(const std::string& effect,
                                         const std::vector<ast::HandlerCase>& cases) {
        HandlerTable table;
        for (const auto& handler_case : cases) {
            HandlerEntry entry;
            entry.effect = handler_case.effect.empty() ? effect : handler_case.effect;
            entry.operation = handler_case.operation;
            entry.function = compile_handler_case(handler_case);
            table.entries.push_back(std::move(entry));
        }
        output_.handler_tables.push_back(std::move(table));
        return static_cast<std::uint32_t>(output_.handler_tables.size() - 1);
    }

    // Compiles a function-like body with tail-expression semantics: an
    // expression statement in tail position returns its value.
    void compile_body(const ast::Stmt& body, const SourceSpan& span) {
        if (const auto* block = dynamic_cast<const ast::BlockStmt*>(&body)) {
            const auto tail = is_tail_expression(*block);
            const auto count = block->statements.size();
            for (std::size_t i = 0; i < count; ++i) {
                const auto& statement = block->statements[i];
                if (tail && i + 1 == count) {
                    const auto* expression =
                        static_cast<const ast::ExprStmt*>(statement.get());
                    compile_expression(*expression->expression);
                    emit(Op::Return, span);
                } else {
                    compile_statement(*statement);
                }
            }
            if (!tail) {
                emit_load_const(Constant{std::monostate{}}, span);
                emit(Op::Return, span);
            }
            return;
        }

        if (const auto* expression = dynamic_cast<const ast::ExprStmt*>(&body)) {
            compile_expression(*expression->expression);
            emit(Op::Return, span);
            return;
        }

        compile_statement(body);
        emit_load_const(Constant{std::monostate{}}, span);
        emit(Op::Return, span);
    }

    // ------------------------------------------------------------------
    // Statements
    // ------------------------------------------------------------------

    void compile_block_contents(const ast::BlockStmt& block) {
        for (const auto& statement : block.statements) {
            compile_statement(*statement);
        }
    }

    void compile_statement(const ast::Stmt& statement) {
        if (dynamic_cast<const ast::EmptyStmt*>(&statement) != nullptr) {
            return;
        }

        if (const auto* block = dynamic_cast<const ast::BlockStmt*>(&statement)) {
            state_->scopes.emplace_back();
            compile_block_contents(*block);
            state_->scopes.pop_back();
            return;
        }

        if (const auto* expression = dynamic_cast<const ast::ExprStmt*>(&statement)) {
            compile_expression(*expression->expression);
            emit(Op::Pop, expression->span);
            return;
        }

        if (const auto* declaration = dynamic_cast<const ast::VarDeclStmt*>(&statement)) {
            if (declaration->initializer != nullptr) {
                compile_expression(*declaration->initializer);
            } else {
                emit_load_const(Constant{std::monostate{}}, declaration->span);
            }
            const auto slot = state_->declare(declaration->name, fn());
            emit(Op::StoreLocal, declaration->span);
            emit_word(slot);
            return;
        }

        if (const auto* assignment = dynamic_cast<const ast::AssignStmt*>(&statement)) {
            compile_assignment(*assignment);
            return;
        }

        if (const auto* return_statement = dynamic_cast<const ast::ReturnStmt*>(&statement)) {
            if (return_statement->value != nullptr) {
                compile_expression(*return_statement->value);
            } else {
                emit_load_const(Constant{std::monostate{}}, return_statement->span);
            }
            emit(Op::Return, return_statement->span);
            return;
        }

        if (const auto* if_statement = dynamic_cast<const ast::IfStmt*>(&statement)) {
            compile_expression(*if_statement->condition);
            const auto else_jump = emit_jump(Op::JumpIfFalse, if_statement->span);
            compile_statement(*if_statement->then_branch);
            if (if_statement->else_branch != nullptr) {
                const auto end_jump = emit_jump(Op::Jump, if_statement->span);
                patch(else_jump, here());
                compile_statement(*if_statement->else_branch);
                patch(end_jump, here());
            } else {
                patch(else_jump, here());
            }
            return;
        }

        if (const auto* while_statement = dynamic_cast<const ast::WhileStmt*>(&statement)) {
            const auto condition_position = here();
            compile_expression(*while_statement->condition);
            const auto exit_jump = emit_jump(Op::JumpIfFalse, while_statement->span);

            FuncState::Loop loop;
            loop.continue_target = condition_position;
            state_->loops.push_back(loop);

            compile_statement(*while_statement->body);

            loop = state_->loops.back();
            state_->loops.pop_back();

            emit(Op::Loop, while_statement->span);
            emit_word(condition_position);
            patch(exit_jump, here());
            for (const auto& break_jump : loop.break_jumps) {
                patch(break_jump, here());
            }
            return;
        }

        if (dynamic_cast<const ast::BreakStmt*>(&statement) != nullptr) {
            if (!state_->loops.empty()) {
                state_->loops.back().break_jumps.push_back(
                    emit_jump(Op::Jump, statement.span));
            }
            return;
        }

        if (dynamic_cast<const ast::ContinueStmt*>(&statement) != nullptr) {
            if (!state_->loops.empty()) {
                emit(Op::Loop, statement.span);
                emit_word(state_->loops.back().continue_target);
            }
            return;
        }
    }

    void compile_assignment(const ast::AssignStmt& assignment) {
        const auto& binding = assignment.target->binding;

        if (binding.kind == BindingKind::Variable) {
            if (binding.field != nullptr) {
                emit_load_receiver(assignment.target->span);
                compile_expression(*assignment.value);
                emit(Op::SetField, assignment.target->span);
                emit_word(field_slot(binding.field));
                return;
            }
            compile_expression(*assignment.value);
            const auto slot = state_->find_local(local_name(*assignment.target));
            if (slot >= 0) {
                emit(Op::StoreLocal, assignment.target->span);
                emit_word(static_cast<std::uint32_t>(slot));
            } else {
                // Assigning a captured (by-value) variable is not supported;
                // discard the value to keep the stack balanced.
                emit(Op::Pop, assignment.target->span);
            }
            return;
        }

        if (binding.kind == BindingKind::Field) {
            const auto* member =
                static_cast<const ast::MemberExpr*>(assignment.target.get());
            compile_expression(*member->object);
            compile_expression(*assignment.value);
            emit(Op::SetField, assignment.target->span);
            emit_word(field_slot(binding.field));
            return;
        }

        // Dynamic or unresolved target.
        if (const auto* member =
                dynamic_cast<const ast::MemberExpr*>(assignment.target.get())) {
            compile_expression(*member->object);
            compile_expression(*assignment.value);
            emit(Op::SetFieldByName, assignment.target->span);
            emit_word(make_constant(Constant{member->name}));
            return;
        }

        compile_expression(*assignment.value);
        emit(Op::Pop, assignment.target->span);
    }

    std::string local_name(const ast::Expr& expression) const {
        const auto* identifier = dynamic_cast<const ast::IdentifierExpr*>(&expression);
        return identifier != nullptr && !identifier->path.empty() ? identifier->path.front()
                                                                 : std::string{};
    }

    void emit_load_receiver(const SourceSpan& span) {
        const auto slot = state_->find_local(kReceiverSlot);
        if (slot >= 0) {
            emit(Op::LoadLocal, span);
            emit_word(static_cast<std::uint32_t>(slot));
            return;
        }
        const auto capture = resolve_upvalue(kReceiverSlot);
        if (capture >= 0) {
            emit(Op::LoadCapture, span);
            emit_word(static_cast<std::uint32_t>(capture));
            return;
        }
        emit_load_const(Constant{std::monostate{}}, span);
    }

    // ------------------------------------------------------------------
    // Expressions
    // ------------------------------------------------------------------

    void compile_expression(const ast::Expr& expression) {
        const auto& binding = expression.binding;

        if (dynamic_cast<const ast::ErrorExpr*>(&expression) != nullptr) {
            emit_load_const(Constant{std::monostate{}}, expression.span);
            return;
        }

        if (const auto* literal = dynamic_cast<const ast::IntLiteralExpr*>(&expression)) {
            emit_load_const(Constant{std::stoll(literal->text)}, literal->span);
            return;
        }

        if (const auto* literal = dynamic_cast<const ast::FloatLiteralExpr*>(&expression)) {
            emit_load_const(Constant{std::stod(literal->text)}, literal->span);
            return;
        }

        if (const auto* literal = dynamic_cast<const ast::StringLiteralExpr*>(&expression)) {
            emit_load_const(Constant{literal->value}, literal->span);
            return;
        }

        if (const auto* literal = dynamic_cast<const ast::BoolLiteralExpr*>(&expression)) {
            emit_load_const(Constant{literal->value}, literal->span);
            return;
        }

        if (dynamic_cast<const ast::UnitLiteralExpr*>(&expression) != nullptr ||
            dynamic_cast<const ast::WildcardExpr*>(&expression) != nullptr) {
            emit_load_const(Constant{std::monostate{}}, expression.span);
            return;
        }

        if (const auto* template_string =
                dynamic_cast<const ast::TemplateStringExpr*>(&expression)) {
            for (const auto& part : template_string->parts) {
                if (part.expression && part.expression_value != nullptr) {
                    compile_expression(*part.expression_value);
                } else {
                    emit_load_const(Constant{part.text}, template_string->span);
                }
            }
            emit(Op::BuildString, template_string->span);
            emit_word(static_cast<std::uint32_t>(template_string->parts.size()));
            return;
        }

        if (const auto* identifier = dynamic_cast<const ast::IdentifierExpr*>(&expression)) {
            compile_identifier(*identifier);
            return;
        }

        if (const auto* unary = dynamic_cast<const ast::UnaryExpr*>(&expression)) {
            compile_unary(*unary);
            return;
        }

        if (const auto* binary = dynamic_cast<const ast::BinaryExpr*>(&expression)) {
            compile_binary(*binary);
            return;
        }

        if (const auto* downcast = dynamic_cast<const ast::DowncastExpr*>(&expression)) {
            compile_expression(*downcast->operand);
            if (binding.type != nullptr) {
                output_.downcast_targets.push_back(binding.type);
                emit(Op::Downcast, downcast->span);
                emit_word(static_cast<std::uint32_t>(output_.downcast_targets.size() - 1));
            }
            return;
        }

        if (const auto* call = dynamic_cast<const ast::CallExpr*>(&expression)) {
            compile_call(*call);
            return;
        }

        if (const auto* member = dynamic_cast<const ast::MemberExpr*>(&expression)) {
            compile_member(*member);
            return;
        }

        if (dynamic_cast<const ast::IndexExpr*>(&expression) != nullptr) {
            emit_load_const(Constant{std::monostate{}}, expression.span);
            return;
        }

        if (const auto* lambda = dynamic_cast<const ast::LambdaExpr*>(&expression)) {
            compile_lambda(*lambda);
            return;
        }

        if (const auto* resume = dynamic_cast<const ast::ResumeExpr*>(&expression)) {
            compile_resume(*resume);
            return;
        }

        if (const auto* handle = dynamic_cast<const ast::HandleExpr*>(&expression)) {
            const auto table = register_handler_table("", handle->cases);
            emit(Op::PushHandler, handle->span);
            emit_word(table);
            compile_expression(*handle->body);
            emit(Op::PopHandler, handle->span);
            return;
        }

        if (const auto* if_expression = dynamic_cast<const ast::IfExpr*>(&expression)) {
            compile_expression(*if_expression->condition);
            const auto else_jump = emit_jump(Op::JumpIfFalse, if_expression->span);
            compile_expression(*if_expression->then_expr);
            const auto end_jump = emit_jump(Op::Jump, if_expression->span);
            patch(else_jump, here());
            compile_expression(*if_expression->else_expr);
            patch(end_jump, here());
            return;
        }

        emit_load_const(Constant{std::monostate{}}, expression.span);
    }

    void compile_identifier(const ast::IdentifierExpr& identifier) {
        const auto& binding = identifier.binding;
        const auto& name = identifier.path.empty() ? std::string{}
                                                   : identifier.path.front();

        if (binding.kind == BindingKind::EnumVariant) {
            emit(Op::EnumVariant, identifier.span);
            emit_word(variant_id(binding.variant));
            return;
        }

        if (binding.kind == BindingKind::Variable) {
            if (binding.field != nullptr) {
                emit_load_receiver(identifier.span);
                emit(Op::GetField, identifier.span);
                emit_word(field_slot(binding.field));
                return;
            }

            const auto slot = state_->find_local(name);
            if (slot >= 0) {
                emit(Op::LoadLocal, identifier.span);
                emit_word(static_cast<std::uint32_t>(slot));
                return;
            }

            const auto capture = resolve_upvalue(name);
            if (capture >= 0) {
                emit(Op::LoadCapture, identifier.span);
                emit_word(static_cast<std::uint32_t>(capture));
                return;
            }

            emit_load_const(Constant{std::monostate{}}, identifier.span);
            return;
        }

        if (binding.kind == BindingKind::StaticMethod) {
            if (binding.function != nullptr && !binding.function->is_host) {
                emit(Op::Closure, identifier.span);
                emit_word(output_.function_ids.at(binding.function));
                emit_word(0);
                return;
            }
            emit_load_const(Constant{std::monostate{}}, identifier.span);
            return;
        }

        // Function symbols (and anything unresolved) only make sense as call
        // targets; referenced directly they evaluate to unit.
        emit_load_const(Constant{std::monostate{}}, identifier.span);
    }

    void compile_unary(const ast::UnaryExpr& expression) {
        compile_expression(*expression.operand);

        if (expression.binding.kind == BindingKind::Operator) {
            emit(Op::CallOperator, expression.span);
            emit_word(operator_id(expression.binding.op));
            emit_word(1);
            return;
        }

        if (expression.op == "-") {
            emit(Op::Neg, expression.span);
        } else if (expression.op == "!") {
            emit(Op::Not, expression.span);
        }
        // Unary '+' is the identity.
    }

    void compile_binary(const ast::BinaryExpr& expression) {
        const auto& op = expression.op;

        // Short-circuit logical operators.
        if (op == "&&" || op == "||") {
            compile_expression(*expression.left);
            emit(Op::Dup, expression.span);
            const auto short_jump =
                emit_jump(op == "&&" ? Op::JumpIfFalse : Op::JumpIfTrue, expression.span);
            emit(Op::Pop, expression.span);
            compile_expression(*expression.right);
            patch(short_jump, here());
            return;
        }

        compile_expression(*expression.left);
        compile_expression(*expression.right);

        if (expression.binding.kind == BindingKind::Operator) {
            emit(Op::CallOperator, expression.span);
            emit_word(operator_id(expression.binding.op));
            emit_word(2);
            return;
        }

        Op intrinsic = Op::Add;
        if (op == "+") intrinsic = Op::Add;
        else if (op == "-") intrinsic = Op::Sub;
        else if (op == "*") intrinsic = Op::Mul;
        else if (op == "/") intrinsic = Op::Div;
        else if (op == "%") intrinsic = Op::Mod;
        else if (op == "==") intrinsic = Op::Eq;
        else if (op == "!=") intrinsic = Op::Ne;
        else if (op == "<") intrinsic = Op::Lt;
        else if (op == "<=") intrinsic = Op::Le;
        else if (op == ">") intrinsic = Op::Gt;
        else if (op == ">=") intrinsic = Op::Ge;
        emit(intrinsic, expression.span);
    }

    void compile_member(const ast::MemberExpr& member) {
        const auto& binding = member.binding;

        if (binding.kind == BindingKind::Field) {
            compile_expression(*member.object);
            emit(Op::GetField, member.span);
            emit_word(field_slot(binding.field));
            return;
        }

        if (binding.kind == BindingKind::StaticMethod) {
            compile_identifier_like_static(member);
            return;
        }

        // Dynamic member access.
        compile_expression(*member.object);
        emit(Op::GetFieldByName, member.span);
        emit_word(make_constant(Constant{member.name}));
    }

    void compile_identifier_like_static(const ast::MemberExpr& member) {
        if (member.binding.function != nullptr && !member.binding.function->is_host) {
            emit(Op::Closure, member.span);
            emit_word(output_.function_ids.at(member.binding.function));
            emit_word(0);
            return;
        }
        emit_load_const(Constant{std::monostate{}}, member.span);
    }

    void compile_resume(const ast::ResumeExpr& expression) {
        // Continuation: a handler-case local, or captured by nested lambdas.
        const auto slot = state_->find_local(kContinuationSlot);
        if (slot >= 0) {
            emit(Op::LoadLocal, expression.span);
            emit_word(static_cast<std::uint32_t>(slot));
        } else {
            const auto capture = resolve_upvalue(kContinuationSlot);
            if (capture >= 0) {
                emit(Op::LoadCapture, expression.span);
                emit_word(static_cast<std::uint32_t>(capture));
            } else {
                emit_load_const(Constant{std::monostate{}}, expression.span);
            }
        }

        if (!expression.wildcard && expression.value != nullptr) {
            compile_expression(*expression.value);
        } else {
            emit_load_const(Constant{std::monostate{}}, expression.span);
        }

        emit(Op::Resume, expression.span);
    }

    void compile_call(const ast::CallExpr& call) {
        const auto& binding = call.callee->binding;
        const auto argc = static_cast<std::uint32_t>(call.arguments.size());

        if (binding.kind != BindingKind::Function) {
            // First-class function value or dynamic callee.
            compile_expression(*call.callee);
            for (const auto& argument : call.arguments) {
                compile_expression(*argument);
            }
            emit(Op::Call, call.span);
            emit_word(argc);
            return;
        }

        const auto* function = binding.function;
        const bool has_receiver = binding.receiver_type != nullptr;

        const auto compile_arguments = [&] {
            if (has_receiver) {
                if (const auto* member =
                        dynamic_cast<const ast::MemberExpr*>(call.callee.get())) {
                    compile_expression(*member->object);
                } else {
                    emit_load_receiver(call.callee->span);
                }
            }
            for (const auto& argument : call.arguments) {
                compile_expression(*argument);
            }
        };

        // The builtin `abort` is a fatal runtime error.
        if (function->name == "abort" && function->owner == nullptr &&
            function->declaration == nullptr) {
            compile_arguments();
            emit(Op::Abort, call.span);
            return;
        }

        // Enum constructors (script and host) build enum values directly.
        if (function->owner != nullptr && function->owner->kind == TypeSymbol::Kind::Enum &&
            function->owner->variants.contains(function->name)) {
            compile_arguments();
            emit(Op::EnumCtor, call.span);
            emit_word(variant_id(function->owner->variants.at(function->name)));
            emit_word(argc);
            return;
        }

        if (function->is_effect_operation) {
            compile_arguments();
            emit(Op::Raise, call.span);
            emit_word(make_constant(Constant{
                function->owner != nullptr ? function->owner->qualified_name : ""}));
            emit_word(make_constant(Constant{function->name}));
            emit_word(argc);
            return;
        }

        if (function->is_host) {
            compile_arguments();
            emit(Op::CallHost, call.span);
            emit_word(host_function_id(function));
            emit_word(argc);
            return;
        }

        const auto fn_it = output_.function_ids.find(function);
        if (fn_it == output_.function_ids.end()) {
            // No compiled body (should not happen after typechecking).
            for (const auto& argument : call.arguments) {
                compile_expression(*argument);
                emit(Op::Pop, argument->span);
            }
            emit_load_const(Constant{std::monostate{}}, call.span);
            return;
        }

        compile_arguments();
        emit(Op::CallFunction, call.span);
        emit_word(fn_it->second);
        emit_word(argc + (has_receiver ? 1u : 0u));
    }

    void compile_lambda(const ast::LambdaExpr& lambda) {
        const auto fn_index = new_function("(lambda)", "(lambda)");

        FuncState state;
        state.fn_index = fn_index;
        state.enclosing = state_;
        state.scopes.emplace_back();
        state_ = &state;
        FunctionBytecode& fn = this->fn();

        for (const auto& parameter : lambda.parameters) {
            state.declare(parameter.name, fn);
        }
        fn.param_count = state.local_count;

        if (lambda.body != nullptr) {
            compile_body(*lambda.body, lambda.span);
        } else {
            emit_load_const(Constant{std::monostate{}}, lambda.span);
            emit(Op::Return, lambda.span);
        }

        const auto captures = std::move(state.captures);
        state_ = state.enclosing;

        for (const auto& capture : captures) {
            if (capture.is_local) {
                emit(Op::LoadLocal, lambda.span);
            } else {
                emit(Op::LoadCapture, lambda.span);
            }
            emit_word(capture.index);
        }
        emit(Op::Closure, lambda.span);
        emit_word(fn_index);
        emit_word(static_cast<std::uint32_t>(captures.size()));
    }

    // ------------------------------------------------------------------
    // Symbol tables
    // ------------------------------------------------------------------

    std::int32_t resolve_upvalue(const std::string& name) {
        if (state_->enclosing == nullptr) {
            return -1;
        }

        const auto local = state_->enclosing->find_local(name);
        if (local >= 0) {
            return add_capture(state_, static_cast<std::uint32_t>(local), true);
        }

        const auto upvalue = resolve_upvalue_in(state_->enclosing, name);
        if (upvalue >= 0) {
            return add_capture(state_, static_cast<std::uint32_t>(upvalue), false);
        }
        return -1;
    }

    std::int32_t resolve_upvalue_in(FuncState* state, const std::string& name) {
        if (state->enclosing == nullptr) {
            return -1;
        }

        const auto local = state->enclosing->find_local(name);
        if (local >= 0) {
            return add_capture(state, static_cast<std::uint32_t>(local), true);
        }

        const auto upvalue = resolve_upvalue_in(state->enclosing, name);
        if (upvalue >= 0) {
            return add_capture(state, static_cast<std::uint32_t>(upvalue), false);
        }
        return -1;
    }

    std::int32_t add_capture(FuncState* state, std::uint32_t index, bool is_local) {
        for (std::uint32_t i = 0; i < state->captures.size(); ++i) {
            const auto& capture = state->captures[i];
            if (capture.is_local == is_local && capture.index == index) {
                return static_cast<std::int32_t>(i);
            }
        }
        state->captures.push_back(Capture{is_local, index});
        output_.functions[state->fn_index].capture_count =
            static_cast<std::uint32_t>(state->captures.size());
        return static_cast<std::int32_t>(state->captures.size() - 1);
    }

    std::uint32_t variant_id(const EnumVariantSymbol* variant) {
        for (std::uint32_t i = 0; i < output_.enum_variants.size(); ++i) {
            if (output_.enum_variants[i] == variant) {
                return i;
            }
        }
        output_.enum_variants.push_back(variant);
        return static_cast<std::uint32_t>(output_.enum_variants.size() - 1);
    }

    std::uint32_t operator_id(const OperatorInfo* info) {
        for (std::uint32_t i = 0; i < output_.operators.size(); ++i) {
            if (output_.operators[i] == info) {
                return i;
            }
        }
        output_.operators.push_back(info);
        return static_cast<std::uint32_t>(output_.operators.size() - 1);
    }

    std::uint32_t host_function_id(const FunctionSymbol* function) {
        for (std::uint32_t i = 0; i < output_.host_functions.size(); ++i) {
            if (output_.host_functions[i] == function) {
                return i;
            }
        }
        output_.host_functions.push_back(function);
        return static_cast<std::uint32_t>(output_.host_functions.size() - 1);
    }

    // Object layout: base-chain fields first (root first), then the
    // declaring type's fields in declaration order.
    std::uint32_t field_slot(const FieldSymbol* field) const {
        std::vector<const TypeSymbol*> chain;
        for (auto* type = field->owner; type != nullptr; type = type->base) {
            chain.push_back(type);
        }

        std::uint32_t slot = 0;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            if (*it == field->owner) {
                break;
            }
            slot += static_cast<std::uint32_t>((*it)->fields.size());
        }
        return slot + static_cast<std::uint32_t>(field->index);
    }

    const Program& program_;
    CompiledProgram output_;
    FuncState* state_ = nullptr;
};

} // namespace

std::size_t operand_count(Op op) {
    return operand_count_impl(op);
}

CompiledProgram compile_program(const Program& program) {
    Compiler compiler(program);
    return compiler.compile();
}

} // namespace glscript
