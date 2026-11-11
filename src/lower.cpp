#include "glscript/hir.hpp"

#include <unordered_map>
#include <vector>

namespace glscript::hir {
namespace {

constexpr const char* kContinuationSlot = "$continuation";
constexpr const char* kReceiverSlot = "this";

using Origin = Function::Origin;

struct Capture {
    bool is_local = false;
    std::uint32_t index = 0;
};

// Per-function lowering state. Mirrors the old compiler's FuncState, but
// produces HIR nodes instead of opcode words: local slots and capture
// indices are assigned here and recorded in the nodes themselves.
struct FuncState {
    std::uint32_t fn_index = 0;
    FuncState* enclosing = nullptr;

    std::vector<std::unordered_map<std::string, std::uint32_t>> scopes;
    std::uint32_t local_count = 0;
    std::vector<Capture> captures;

    std::int32_t find_local(const std::string& name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            const auto found = it->find(name);
            if (found != it->end()) {
                return static_cast<std::int32_t>(found->second);
            }
        }
        return -1;
    }

    std::uint32_t declare(const std::string& name, Function& fn) {
        const auto slot = local_count++;
        scopes.back().emplace(name, slot);
        fn.local_count = local_count;
        return slot;
    }
};

// Compiles the last statement of a block with tail semantics: an expression
// statement in tail position returns its value. Applies to every block body
// (functions, methods, lambdas, handler cases) — matching the old emitter.
bool is_tail_expression(const ast::BlockStmt& block) {
    return !block.statements.empty() &&
           dynamic_cast<const ast::ExprStmt*>(block.statements.back().get()) != nullptr;
}

// The value a pattern test operates on: the match's scrutinee local, or a
// payload slot of an enum value bound by an enclosing variant pattern.
struct PatternTerm {
    bool is_payload = false;
    std::uint32_t value_slot = 0;
    std::uint32_t payload_index = 0;
};

BinaryOp intrinsic_binary(const std::string& op) {
    if (op == "-") return BinaryOp::Sub;
    if (op == "*") return BinaryOp::Mul;
    if (op == "/") return BinaryOp::Div;
    if (op == "%") return BinaryOp::Mod;
    if (op == "==") return BinaryOp::Eq;
    if (op == "!=") return BinaryOp::Ne;
    if (op == "<") return BinaryOp::Lt;
    if (op == "<=") return BinaryOp::Le;
    if (op == ">") return BinaryOp::Gt;
    if (op == ">=") return BinaryOp::Ge;
    return BinaryOp::Add;
}

class Lowerer {
public:
    explicit Lowerer(const glscript::Program& program) : program_(program) {
        // std::Option variants for the synthesized array-get handler.
        if (auto* std_module = program.find_module("std"); std_module != nullptr) {
            if (const auto it = std_module->types.find("Option"); it != std_module->types.end()) {
                const auto& variants = it->second->variants;
                if (const auto some = variants.find("Some"); some != variants.end()) {
                    some_variant_ = some->second;
                }
                if (const auto none = variants.find("None"); none != variants.end()) {
                    none_variant_ = none->second;
                }
            }
            // The runtime raises OutOfBounds::index directly (no Raise node
            // lowers it), so the unhandled-op fallback thunk must be
            // registered here.
            if (const auto it = std_module->types.find("OutOfBounds");
                it != std_module->types.end()) {
                for (const auto* operation : it->second->effect_operations) {
                    if (operation->name == "index") {
                        host_function_id(operation);
                        break;
                    }
                }
            }
        }
    }

    hir::Program lower() {
        register_script_functions();
        lower_script_functions();
        return std::move(out_);
    }

private:
    Function& fn(std::uint32_t index) { return *out_.functions[index]; }
    Function& fn() { return fn(state_->fn_index); }

    // ------------------------------------------------------------------
    // Function registration (indices must exist before bodies lower)
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
        const auto index = new_function(function->name, function->qualified_name,
                                        Origin::Function);
        out_.function_ids[function] = index;
    }

    std::uint32_t new_function(std::string name, std::string qualified_name, Origin origin) {
        auto function = std::make_unique<Function>();
        function->name = std::move(name);
        function->qualified_name = std::move(qualified_name);
        function->origin = origin;
        out_.functions.push_back(std::move(function));
        return static_cast<std::uint32_t>(out_.functions.size() - 1);
    }

    // ------------------------------------------------------------------
    // Function bodies
    // ------------------------------------------------------------------

    void lower_script_functions() {
        for (const auto& [name, module] : program_.modules) {
            (void)name;
            if (module->is_host) {
                continue;
            }
            for (const auto& [_, function] : module->functions) {
                lower_registered(function, nullptr);
            }
            for (const auto& [_, type] : module->types) {
                for (const auto& [__, method] : type->methods) {
                    lower_registered(method, type);
                }
            }
            lower_module_init(*module);
        }
    }

    void lower_registered(const FunctionSymbol* function, const TypeSymbol* receiver) {
        const auto it = out_.function_ids.find(function);
        if (it == out_.function_ids.end()) {
            return;
        }

        FuncState state;
        state.fn_index = it->second;
        state.enclosing = state_;
        state.scopes.emplace_back();
        state_ = &state;
        Function& fn = this->fn();

        fn.has_receiver = receiver != nullptr;
        if (receiver != nullptr) {
            state.declare(kReceiverSlot, fn);
        }
        for (const auto& parameter : function->parameter_names) {
            fn.param_names.push_back(parameter);
            state.declare(parameter, fn);
        }
        fn.param_count = state.local_count;

        fn.body = lower_body(*function->declaration->body, function->span);

        state_ = state.enclosing;
    }

    // Handler case bodies: operation parameters, then an implicit
    // continuation slot that `resume` reads.
    std::uint32_t lower_handler_case(const ast::HandlerCase& handler_case) {
        const auto fn_index =
            new_function(handler_case.operation + " (handler)",
                         handler_case.effect + "::" + handler_case.operation,
                         Origin::HandlerCase);

        FuncState state;
        state.fn_index = fn_index;
        state.enclosing = state_;
        state.scopes.emplace_back();
        state_ = &state;
        Function& fn = this->fn();

        for (const auto& parameter : handler_case.parameters) {
            fn.param_names.push_back(parameter.name);
            state.declare(parameter.name, fn);
        }
        fn.param_count = state.local_count;
        const auto resume_slot = state.declare(kContinuationSlot, fn);
        fn.resume_slot = resume_slot;

        if (handler_case.body != nullptr) {
            fn.body = lower_body(*handler_case.body, handler_case.span);
        } else {
            fn.body = unit_return_body(handler_case.span);
        }

        state_ = state.enclosing;
        return fn_index;
    }

    // Handled bodies lower as their own function so that a raise inside them
    // is always exactly one frame above the handler's delimiter: the captured
    // continuation ends at the body's Return, and the delimiter frame
    // receives the handler's value as the value of the whole `handle`
    // expression. The emitter builds the body as a closure over its captures,
    // so the handled expression may reference outer locals.
    struct LoweredHandleBody {
        std::uint32_t fn = 0;
        std::vector<Capture> captures;
    };

    LoweredHandleBody lower_handle_body(const ast::Expr& body) {
        const auto fn_index = new_function("(handle body)", "(handle body)",
                                           Origin::HandleBody);

        FuncState state;
        state.fn_index = fn_index;
        state.enclosing = state_;
        state.scopes.emplace_back();
        state_ = &state;

        Block body_block;
        body_block.items.push_back(make_return(lower_expression(body), body.span));
        fn().body = std::move(body_block);

        LoweredHandleBody lowered;
        lowered.fn = fn_index;
        lowered.captures = std::move(state.captures);
        state_ = state.enclosing;
        return lowered;
    }

    // Module init evaluates `handle <expr> with { ... }` declarations;
    // expression-less `handle Effect { ... }` declarations become ambient
    // handlers installed for the module's whole execution.
    void lower_module_init(const ModuleInfo& module) {
        for (const auto& declaration : module.unit->declarations) {
            const auto* handler_decl = dynamic_cast<const ast::HandlerDecl*>(declaration.get());
            if (handler_decl == nullptr || handler_decl->handled_expression != nullptr) {
                continue;
            }
            const auto table =
                register_handler_table(handler_decl->effect, handler_decl->cases);
            out_.module_ambient_handlers[module.name].push_back(table);
        }

        bool has_init = false;
        for (const auto& declaration : module.unit->declarations) {
            const auto* handler_decl = dynamic_cast<const ast::HandlerDecl*>(declaration.get());
            if (handler_decl != nullptr && handler_decl->handled_expression != nullptr) {
                has_init = true;
            }
        }
        if (!has_init) {
            return;
        }

        const auto fn_index = new_function("(module init)", module.name, Origin::ModuleInit);

        FuncState state;
        state.fn_index = fn_index;
        state.enclosing = state_;
        state.scopes.emplace_back();
        state_ = &state;

        Block body;
        for (const auto& declaration : module.unit->declarations) {
            const auto* handler_decl = dynamic_cast<const ast::HandlerDecl*>(declaration.get());
            if (handler_decl == nullptr || handler_decl->handled_expression == nullptr) {
                continue;
            }
            const auto table =
                register_handler_table(handler_decl->effect, handler_decl->cases);
            const auto lowered_body = lower_handle_body(*handler_decl->handled_expression);

            auto handle = make_expr(Expr::Kind::Handle, handler_decl->span);
            handle->body_fn = lowered_body.fn;
            for (auto& capture : lowered_body.captures) {
                handle->captures.push_back(CaptureRef{capture.is_local, capture.index});
            }
            handle->table_ref = table;

            auto statement = make_stmt(Stmt::Kind::Expr, handler_decl->span);
            statement.expression = std::move(handle);
            body.items.push_back(std::move(statement));
        }
        body.items.push_back(make_return(make_expr(Expr::Kind::Unit, {}), {}));

        fn().body = std::move(body);

        state_ = state.enclosing;
        out_.module_inits[module.name] = fn_index;
    }

    std::uint32_t register_handler_table(const std::string& effect,
                                         const std::vector<ast::HandlerCase>& cases) {
        HandlerTable table;
        for (const auto& handler_case : cases) {
            HandlerEntry entry;
            entry.effect = handler_case.effect.empty() ? effect : handler_case.effect;
            entry.operation = handler_case.operation;
            entry.function = lower_handler_case(handler_case);
            table.entries.push_back(std::move(entry));
        }
        out_.handler_tables.push_back(std::move(table));
        return static_cast<std::uint32_t>(out_.handler_tables.size() - 1);
    }

    // ------------------------------------------------------------------
    // Bodies, statements
    // ------------------------------------------------------------------

    // Lowers a function-like body with tail-expression semantics: an
    // expression statement in tail position returns its value.
    Block lower_body(const ast::Stmt& body, const SourceSpan& span) {
        Block block;
        if (const auto* stmt_block = dynamic_cast<const ast::BlockStmt*>(&body)) {
            const auto tail = is_tail_expression(*stmt_block);
            const auto count = stmt_block->statements.size();
            for (std::size_t i = 0; i < count; ++i) {
                const auto& statement = stmt_block->statements[i];
                if (tail && i + 1 == count) {
                    const auto* expression =
                        static_cast<const ast::ExprStmt*>(statement.get());
                    block.items.push_back(
                        make_return(lower_expression(*expression->expression), span));
                } else {
                    lower_statement(*statement, block.items);
                }
            }
            if (!tail) {
                block.items.push_back(make_return(make_expr(Expr::Kind::Unit, span), span));
            }
            return block;
        }

        if (const auto* expression = dynamic_cast<const ast::ExprStmt*>(&body)) {
            block.items.push_back(
                make_return(lower_expression(*expression->expression), span));
            return block;
        }

        lower_statement(body, block.items);
        block.items.push_back(make_return(make_expr(Expr::Kind::Unit, span), span));
        return block;
    }

    Block unit_return_body(const SourceSpan& span) {
        Block block;
        block.items.push_back(make_return(make_expr(Expr::Kind::Unit, span), span));
        return block;
    }

    // Lowers one statement into the enclosing block. Nested statement blocks
    // flatten into the same HIR block — slots are monotonic per function, so
    // only the name scope stack needs pushing and popping.
    void lower_statement(const ast::Stmt& statement, std::vector<Stmt>& items) {
        if (dynamic_cast<const ast::EmptyStmt*>(&statement) != nullptr) {
            return;
        }

        if (const auto* block = dynamic_cast<const ast::BlockStmt*>(&statement)) {
            state_->scopes.emplace_back();
            for (const auto& nested : block->statements) {
                lower_statement(*nested, items);
            }
            state_->scopes.pop_back();
            return;
        }

        if (const auto* expression = dynamic_cast<const ast::ExprStmt*>(&statement)) {
            auto stmt = make_stmt(Stmt::Kind::Expr, expression->span);
            stmt.expression = lower_expression(*expression->expression);
            items.push_back(std::move(stmt));
            return;
        }

        if (const auto* declaration = dynamic_cast<const ast::VarDeclStmt*>(&statement)) {
            // `let` destructuring: the initializer goes into a hidden temp,
            // then each binding in preorder becomes a VarLocal statement.
            // Irrefutable patterns carry no variant/literal tests.
            if (declaration->pattern != nullptr) {
                const auto temp_slot =
                    state_->declare("$let" + std::to_string(pattern_counter_++), fn());

                auto temp = make_stmt(Stmt::Kind::VarLocal, declaration->span);
                temp.slot = temp_slot;
                temp.expression = declaration->initializer != nullptr
                                      ? lower_expression(*declaration->initializer)
                                      : make_expr(Expr::Kind::Unit, declaration->span);
                items.push_back(std::move(temp));

                std::vector<MatchTest> tests;
                PatternTerm scrutinee_term;
                scrutinee_term.value_slot = temp_slot;
                lower_pattern_tests(*declaration->pattern, scrutinee_term, tests, false);

                for (auto& test : tests) {
                    if (test.kind != MatchTest::Kind::Bind) {
                        continue;
                    }
                    auto bind = make_stmt(Stmt::Kind::VarLocal, declaration->span);
                    bind.slot = test.slot;
                    bind.expression = term_expression(test, declaration->span);
                    items.push_back(std::move(bind));
                }
                return;
            }

            // Slot assignment happens after the initializer lowers, matching
            // the old emission order (initializer code cannot see the slot).
            auto stmt = make_stmt(Stmt::Kind::VarLocal, declaration->span);
            if (declaration->initializer != nullptr) {
                stmt.expression = lower_expression(*declaration->initializer);
            } else {
                stmt.expression = make_expr(Expr::Kind::Unit, declaration->span);
            }
            stmt.slot = state_->declare(declaration->name, fn());
            items.push_back(std::move(stmt));
            return;
        }

        if (const auto* assignment = dynamic_cast<const ast::AssignStmt*>(&statement)) {
            items.push_back(lower_assignment(*assignment));
            return;
        }

        if (const auto* return_statement = dynamic_cast<const ast::ReturnStmt*>(&statement)) {
            if (return_statement->value != nullptr) {
                items.push_back(
                    make_return(lower_expression(*return_statement->value),
                                return_statement->span));
            } else {
                items.push_back(make_return(make_expr(Expr::Kind::Unit,
                                                      return_statement->span),
                                            return_statement->span));
            }
            return;
        }

        if (const auto* if_statement = dynamic_cast<const ast::IfStmt*>(&statement)) {
            auto stmt = make_stmt(Stmt::Kind::If, if_statement->span);
            stmt.expression = lower_expression(*if_statement->condition);
            stmt.then_branch = lower_branch(*if_statement->then_branch);
            if (if_statement->else_branch != nullptr) {
                stmt.else_branch = lower_branch(*if_statement->else_branch);
            }
            items.push_back(std::move(stmt));
            return;
        }

        if (const auto* while_statement = dynamic_cast<const ast::WhileStmt*>(&statement)) {
            auto stmt = make_stmt(Stmt::Kind::While, while_statement->span);
            stmt.expression = lower_expression(*while_statement->condition);
            stmt.body = lower_branch(*while_statement->body);
            items.push_back(std::move(stmt));
            return;
        }

        if (dynamic_cast<const ast::BreakStmt*>(&statement) != nullptr) {
            items.push_back(make_stmt(Stmt::Kind::Break, statement.span));
            return;
        }

        if (dynamic_cast<const ast::ContinueStmt*>(&statement) != nullptr) {
            items.push_back(make_stmt(Stmt::Kind::Continue, statement.span));
            return;
        }
    }

    // Lowers an if/while branch: a statement block keeps its name scope;
    // a single statement lands alone in the branch block.
    BlockPtr lower_branch(const ast::Stmt& statement) {
        auto branch = std::make_unique<Block>();
        if (const auto* block = dynamic_cast<const ast::BlockStmt*>(&statement)) {
            state_->scopes.emplace_back();
            for (const auto& nested : block->statements) {
                lower_statement(*nested, branch->items);
            }
            state_->scopes.pop_back();
        } else {
            lower_statement(statement, branch->items);
        }
        return branch;
    }

    Stmt lower_assignment(const ast::AssignStmt& assignment) {
        const auto& binding = assignment.target->binding;
        auto stmt = make_stmt(Stmt::Kind::Assign, assignment.span);
        stmt.expression = lower_expression(*assignment.value);

        if (binding.kind == BindingKind::Variable && binding.field != nullptr) {
            // A bare name resolved to an instance field: write through the
            // receiver.
            auto target = make_expr(Expr::Kind::Field, assignment.target->span);
            target->object = load_receiver(assignment.target->span);
            target->field_slot = field_slot(binding.field);
            stmt.target = std::move(target);
            return stmt;
        }

        if (binding.kind == BindingKind::Variable) {
            const auto slot = state_->find_local(local_name(*assignment.target));
            if (slot >= 0) {
                auto target = make_expr(Expr::Kind::Local, assignment.target->span);
                target->slot = static_cast<std::uint32_t>(slot);
                stmt.target = std::move(target);
            }
            // Assigning a captured (by-value) variable is not supported:
            // a null target makes the emitter discard the value.
            return stmt;
        }

        if (binding.kind == BindingKind::Index) {
            // arr[i] = v: target is an ArrayGet-shaped node (object + index).
            const auto* index_target = static_cast<const ast::IndexExpr*>(
                assignment.target.get());
            auto target = make_expr(Expr::Kind::ArrayGet, assignment.target->span);
            target->object = lower_expression(*index_target->object);
            if (index_target->index != nullptr) {
                target->args.push_back(lower_expression(*index_target->index));
            }
            stmt.target = std::move(target);
            return stmt;
        }

        if (binding.kind == BindingKind::Field) {
            const auto* member = static_cast<const ast::MemberExpr*>(assignment.target.get());
            auto target = make_expr(Expr::Kind::Field, assignment.target->span);
            target->object = lower_expression(*member->object);
            target->field_slot = field_slot(binding.field);
            stmt.target = std::move(target);
            return stmt;
        }

        // Dynamic or unresolved target.
        if (const auto* member =
                dynamic_cast<const ast::MemberExpr*>(assignment.target.get())) {
            auto target = make_expr(Expr::Kind::FieldDynamic, assignment.target->span);
            target->object = lower_expression(*member->object);
            target->field_name = member->name;
            stmt.target = std::move(target);
        }
        // Anything else discards the value, matching the old fallback.
        return stmt;
    }

    std::string local_name(const ast::Expr& expression) const {
        const auto* identifier = dynamic_cast<const ast::IdentifierExpr*>(&expression);
        return identifier != nullptr && !identifier->path.empty() ? identifier->path.front()
                                                                 : std::string{};
    }

    // The receiver: a local of the current function, a capture of an outer
    // one, or unit when there is nothing to load (error recovery).
    ExprPtr load_receiver(const SourceSpan& span) {
        return load_named_local_or_capture(kReceiverSlot, span);
    }

    ExprPtr load_named_local_or_capture(const std::string& name, const SourceSpan& span) {
        const auto slot = state_->find_local(name);
        if (slot >= 0) {
            auto expr = make_expr(Expr::Kind::Local, span);
            expr->slot = static_cast<std::uint32_t>(slot);
            return expr;
        }
        const auto capture = resolve_upvalue(name);
        if (capture >= 0) {
            auto expr = make_expr(Expr::Kind::Capture, span);
            expr->slot = static_cast<std::uint32_t>(capture);
            return expr;
        }
        return make_expr(Expr::Kind::Unit, span);
    }

    // ------------------------------------------------------------------
    // Expressions
    // ------------------------------------------------------------------

    ExprPtr lower_expression(const ast::Expr& expression) {
        const auto& binding = expression.binding;

        if (dynamic_cast<const ast::ErrorExpr*>(&expression) != nullptr) {
            return make_expr(Expr::Kind::Unit, expression.span);
        }

        if (const auto* literal = dynamic_cast<const ast::IntLiteralExpr*>(&expression)) {
            auto expr = make_expr(Expr::Kind::Int, literal->span);
            expr->int_value = std::stoll(literal->text);
            return expr;
        }

        if (const auto* literal = dynamic_cast<const ast::FloatLiteralExpr*>(&expression)) {
            auto expr = make_expr(Expr::Kind::Float, literal->span);
            expr->float_value = std::stod(literal->text);
            return expr;
        }

        if (const auto* literal = dynamic_cast<const ast::StringLiteralExpr*>(&expression)) {
            auto expr = make_expr(Expr::Kind::String, literal->span);
            expr->string_value = literal->value;
            return expr;
        }

        if (const auto* literal = dynamic_cast<const ast::BoolLiteralExpr*>(&expression)) {
            auto expr = make_expr(Expr::Kind::Bool, literal->span);
            expr->bool_value = literal->value;
            return expr;
        }

        if (dynamic_cast<const ast::UnitLiteralExpr*>(&expression) != nullptr ||
            dynamic_cast<const ast::WildcardExpr*>(&expression) != nullptr) {
            return make_expr(Expr::Kind::Unit, expression.span);
        }

        if (const auto* template_string =
                dynamic_cast<const ast::TemplateStringExpr*>(&expression)) {
            auto expr = make_expr(Expr::Kind::BuildString, template_string->span);
            for (const auto& part : template_string->parts) {
                if (part.expression && part.expression_value != nullptr) {
                    expr->parts.push_back(lower_expression(*part.expression_value));
                } else {
                    auto text = make_expr(Expr::Kind::String, template_string->span);
                    text->string_value = part.text;
                    expr->parts.push_back(std::move(text));
                }
            }
            return expr;
        }

        if (const auto* identifier = dynamic_cast<const ast::IdentifierExpr*>(&expression)) {
            return lower_identifier(*identifier);
        }

        if (const auto* unary = dynamic_cast<const ast::UnaryExpr*>(&expression)) {
            return lower_unary(*unary);
        }

        if (const auto* binary = dynamic_cast<const ast::BinaryExpr*>(&expression)) {
            return lower_binary(*binary);
        }

        if (const auto* downcast = dynamic_cast<const ast::DowncastExpr*>(&expression)) {
            auto operand = lower_expression(*downcast->operand);
            if (binding.type == nullptr) {
                return operand; // unchecked; the operand value passes through
            }
            auto expr = make_expr(Expr::Kind::Downcast, downcast->span);
            out_.downcast_targets.push_back(binding.type);
            expr->downcast_target = binding.type;
            expr->fn_id = static_cast<std::uint32_t>(out_.downcast_targets.size() - 1);
            expr->object = std::move(operand);
            return expr;
        }

        if (const auto* call = dynamic_cast<const ast::CallExpr*>(&expression)) {
            return lower_call(*call);
        }

        if (const auto* member = dynamic_cast<const ast::MemberExpr*>(&expression)) {
            return lower_member(*member);
        }

        if (const auto* index = dynamic_cast<const ast::IndexExpr*>(&expression)) {
            if (binding.kind == BindingKind::Index) {
                auto expr = make_expr(Expr::Kind::ArrayGet, index->span);
                expr->object = lower_expression(*index->object);
                if (index->index != nullptr) {
                    expr->args.push_back(lower_expression(*index->index));
                }
                return expr;
            }
            return make_expr(Expr::Kind::Unit, expression.span); // error recovery
        }

        if (const auto* array = dynamic_cast<const ast::ArrayLiteralExpr*>(&expression)) {
            auto expr = make_expr(Expr::Kind::ArrayNew, array->span);
            for (const auto& item : array->elements) {
                expr->args.push_back(lower_expression(*item));
            }
            return expr;
        }

        if (const auto* lambda = dynamic_cast<const ast::LambdaExpr*>(&expression)) {
            return lower_lambda(*lambda);
        }

        if (const auto* match = dynamic_cast<const ast::MatchExpr*>(&expression)) {
            return lower_match(*match);
        }

        if (const auto* block = dynamic_cast<const ast::BlockExpr*>(&expression)) {
            return lower_block_expr(*block);
        }

        if (const auto* resume = dynamic_cast<const ast::ResumeExpr*>(&expression)) {
            return lower_resume(*resume);
        }

        if (const auto* handle = dynamic_cast<const ast::HandleExpr*>(&expression)) {
            auto expr = make_expr(Expr::Kind::Handle, handle->span);
            expr->table_ref = register_handler_table("", handle->cases);
            const auto lowered_body = lower_handle_body(*handle->body);
            expr->body_fn = lowered_body.fn;
            for (auto& capture : lowered_body.captures) {
                expr->captures.push_back(CaptureRef{capture.is_local, capture.index});
            }
            return expr;
        }

        if (dynamic_cast<const ast::IfExpr*>(&expression) != nullptr) {
            // The parser only produces if statements (ast::IfStmt); this
            // node kind is dead defensive support in the AST, so unit is as
            // far as it ever got.
            return make_expr(Expr::Kind::Unit, expression.span);
        }

        return make_expr(Expr::Kind::Unit, expression.span);
    }

    ExprPtr lower_identifier(const ast::IdentifierExpr& identifier) {
        const auto& binding = identifier.binding;
        const auto& name = identifier.path.empty() ? std::string{}
                                                   : identifier.path.front();

        if (binding.kind == BindingKind::EnumVariant) {
            auto expr = make_expr(Expr::Kind::EnumVariant, identifier.span);
            expr->variant_id = variant_id(binding.variant);
            return expr;
        }

        if (binding.kind == BindingKind::Variable) {
            if (binding.field != nullptr) {
                auto expr = make_expr(Expr::Kind::Field, identifier.span);
                expr->object = load_receiver(identifier.span);
                expr->field_slot = field_slot(binding.field);
                return expr;
            }
            return load_named_local_or_capture(name, identifier.span);
        }

        if (binding.kind == BindingKind::StaticMethod) {
            if (binding.function != nullptr && !binding.function->is_host) {
                return make_closure(out_.function_ids.at(binding.function),
                                    identifier.span);
            }
            return make_expr(Expr::Kind::Unit, identifier.span);
        }

        // Function symbols (and anything unresolved) only make sense as call
        // targets; referenced directly they evaluate to unit.
        return make_expr(Expr::Kind::Unit, identifier.span);
    }

    ExprPtr lower_unary(const ast::UnaryExpr& expression) {
        auto operand = lower_expression(*expression.operand);

        if (expression.binding.kind == BindingKind::Operator) {
            auto expr = make_expr(Expr::Kind::OperatorCall, expression.span);
            expr->operator_id = operator_id(expression.binding.op);
            expr->args.push_back(std::move(operand));
            return expr;
        }

        if (expression.op == "-") {
            auto expr = make_expr(Expr::Kind::IntrinsicUnary, expression.span);
            expr->unary_op = UnaryOp::Neg;
            expr->object = std::move(operand);
            return expr;
        }
        if (expression.op == "!") {
            auto expr = make_expr(Expr::Kind::IntrinsicUnary, expression.span);
            expr->unary_op = UnaryOp::Not;
            expr->object = std::move(operand);
            return expr;
        }
        // Unary '+' is the identity: the operand expression passes through.
        return operand;
    }

    ExprPtr lower_binary(const ast::BinaryExpr& expression) {
        const auto& op = expression.op;

        if (op == "&&" || op == "||") {
            auto expr = make_expr(Expr::Kind::ShortCircuit, expression.span);
            expr->short_circuit_is_and = op == "&&";
            expr->lhs = lower_expression(*expression.left);
            expr->rhs = lower_expression(*expression.right);
            return expr;
        }

        auto lhs = lower_expression(*expression.left);
        auto rhs = lower_expression(*expression.right);

        if (expression.binding.kind == BindingKind::Operator) {
            auto expr = make_expr(Expr::Kind::OperatorCall, expression.span);
            expr->operator_id = operator_id(expression.binding.op);
            expr->args.push_back(std::move(lhs));
            expr->args.push_back(std::move(rhs));
            return expr;
        }

        auto expr = make_expr(Expr::Kind::IntrinsicBinary, expression.span);
        expr->binary_op = op == "+" ? BinaryOp::Add : intrinsic_binary(op);
        expr->lhs = std::move(lhs);
        expr->rhs = std::move(rhs);
        return expr;
    }

    ExprPtr lower_member(const ast::MemberExpr& member) {
        const auto& binding = member.binding;

        if (binding.kind == BindingKind::Field) {
            auto expr = make_expr(Expr::Kind::Field, member.span);
            expr->object = lower_expression(*member.object);
            expr->field_slot = field_slot(binding.field);
            return expr;
        }

        if (binding.kind == BindingKind::StaticMethod) {
            if (binding.function != nullptr && !binding.function->is_host) {
                return make_closure(out_.function_ids.at(binding.function), member.span);
            }
            return make_expr(Expr::Kind::Unit, member.span);
        }

        // Dynamic member access.
        auto expr = make_expr(Expr::Kind::FieldDynamic, member.span);
        expr->object = lower_expression(*member.object);
        expr->field_name = member.name;
        return expr;
    }

    ExprPtr lower_resume(const ast::ResumeExpr& expression) {
        // Continuation: a handler-case local, or captured by nested lambdas.
        auto expr = make_expr(Expr::Kind::Resume, expression.span);
        expr->continuation = load_named_local_or_capture(kContinuationSlot,
                                                         expression.span);
        if (!expression.wildcard && expression.value != nullptr) {
            expr->resume_value = lower_expression(*expression.value);
        } else {
            expr->resume_value = make_expr(Expr::Kind::Unit, expression.span);
        }
        return expr;
    }

    ExprPtr lower_lambda(const ast::LambdaExpr& lambda) {
        const auto fn_index = new_function("(lambda)", "(lambda)", Origin::Lambda);

        FuncState state;
        state.fn_index = fn_index;
        state.enclosing = state_;
        state.scopes.emplace_back();
        state_ = &state;
        Function& fn = this->fn();

        for (const auto& parameter : lambda.parameters) {
            fn.param_names.push_back(parameter.name);
            state.declare(parameter.name, fn);
        }
        fn.param_count = state.local_count;

        if (lambda.body != nullptr) {
            fn.body = lower_body(*lambda.body, lambda.span);
        } else {
            fn.body = unit_return_body(lambda.span);
        }

        auto captures = std::move(state.captures);
        state_ = state.enclosing;

        return make_closure(fn_index, lambda.span, std::move(captures));
    }

    ExprPtr make_closure(std::uint32_t fn_index, const SourceSpan& span,
                         std::vector<Capture> captures = {}) {
        auto expr = make_expr(Expr::Kind::Closure, span);
        expr->closure_fn = fn_index;
        for (auto& capture : captures) {
            expr->captures.push_back(CaptureRef{capture.is_local, capture.index});
        }
        fn(fn_index).capture_count = static_cast<std::uint32_t>(expr->captures.size());
        return expr;
    }

    // ------------------------------------------------------------------
    // Pattern matching
    // ------------------------------------------------------------------

    // A statement block in expression position (`=> { ... }`) compiles as a
    // zero-argument closure called immediately — the arm's value is the
    // block's tail value (or a returned value flowing out as the block's
    // value). Captures work through the regular lambda machinery.
    ExprPtr lower_block_expr(const ast::BlockExpr& block) {
        const auto fn_index = new_function("(block)", "(block)", Origin::Block);

        FuncState state;
        state.fn_index = fn_index;
        state.enclosing = state_;
        state.scopes.emplace_back();
        state_ = &state;

        if (block.body != nullptr) {
            fn().body = lower_body(*block.body, block.span);
        } else {
            fn().body = unit_return_body(block.span);
        }

        auto captures = std::move(state.captures);
        state_ = state.enclosing;

        auto call = make_expr(Expr::Kind::CallValue, block.span);
        call->object = make_closure(fn_index, block.span, std::move(captures));
        return call;
    }

    // Walks a pattern in preorder, appending the arm's test plan. Variant and
    // literal tests are only emitted when `test_variants` (a `let` pattern is
    // irrefutable, so its variant tests would always pass).
    void lower_pattern_tests(const ast::Pattern& pattern, const PatternTerm& term,
                             std::vector<MatchTest>& tests, bool test_variants) {
        if (dynamic_cast<const ast::WildcardPattern*>(&pattern) != nullptr ||
            dynamic_cast<const ast::ErrorPattern*>(&pattern) != nullptr) {
            return;
        }

        if (const auto* binding = dynamic_cast<const ast::BindingPattern*>(&pattern)) {
            if (binding->variant != nullptr) {
                // Bare payload-less variant name resolved by the checker.
                if (test_variants) {
                    MatchTest test;
                    test.kind = MatchTest::Kind::Variant;
                    fill_term(test, term);
                    test.variant_id = variant_id(binding->variant);
                    tests.push_back(std::move(test));
                }
                return;
            }
            MatchTest test;
            test.kind = MatchTest::Kind::Bind;
            fill_term(test, term);
            test.slot = state_->declare(binding->name, fn());
            tests.push_back(std::move(test));
            return;
        }

        if (const auto* literal = dynamic_cast<const ast::LiteralPattern*>(&pattern)) {
            if (test_variants && literal->value != nullptr) {
                MatchTest test;
                test.kind = MatchTest::Kind::Literal;
                fill_term(test, term);
                test.literal = lower_expression(*literal->value);
                tests.push_back(std::move(test));
            }
            return;
        }

        if (const auto* variant_pattern = dynamic_cast<const ast::VariantPattern*>(&pattern)) {
            if (test_variants && variant_pattern->variant != nullptr) {
                MatchTest test;
                test.kind = MatchTest::Kind::Variant;
                fill_term(test, term);
                test.variant_id = variant_id(variant_pattern->variant);
                tests.push_back(std::move(test));
            }
            // Bind the enum value into an anonymous local so payload terms
            // can reference it, then recurse into the payload patterns.
            MatchTest bind;
            bind.kind = MatchTest::Kind::Bind;
            fill_term(bind, term);
            bind.slot =
                state_->declare("$pat" + std::to_string(pattern_counter_++), fn());
            const auto value_slot = bind.slot;
            tests.push_back(std::move(bind));

            for (std::size_t i = 0; i < variant_pattern->payload.size(); ++i) {
                PatternTerm payload_term;
                payload_term.is_payload = true;
                payload_term.value_slot = value_slot;
                payload_term.payload_index = static_cast<std::uint32_t>(i);
                lower_pattern_tests(*variant_pattern->payload[i], payload_term, tests,
                                    test_variants);
            }
            return;
        }
    }

    void fill_term(MatchTest& test, const PatternTerm& term) {
        test.term = term.is_payload ? MatchTest::Term::Payload : MatchTest::Term::Scrutinee;
        test.value_slot = term.value_slot;
        test.payload_index = term.payload_index;
    }

    // The HIR expression reading a test's term value (for `let` bindings,
    // which lower as plain VarLocal statements).
    ExprPtr term_expression(const MatchTest& test, const SourceSpan& span) {
        if (test.term == MatchTest::Term::Scrutinee) {
            auto expr = make_expr(Expr::Kind::Local, span);
            expr->slot = test.value_slot;
            return expr;
        }
        auto expr = make_expr(Expr::Kind::EnumPayload, span);
        auto object = make_expr(Expr::Kind::Local, span);
        object->slot = test.value_slot;
        expr->object = std::move(object);
        expr->payload_index = test.payload_index;
        return expr;
    }

    ExprPtr lower_match(const ast::MatchExpr& match) {
        auto expr = make_expr(Expr::Kind::Match, match.span);
        expr->scrutinee = lower_expression(*match.scrutinee);
        expr->scrutinee_slot =
            state_->declare("$match" + std::to_string(pattern_counter_++), fn());

        for (const auto& arm : match.arms) {
            state_->scopes.emplace_back();
            hir::MatchArm hir_arm;
            hir_arm.span = arm.span;

            PatternTerm scrutinee_term;
            scrutinee_term.value_slot = expr->scrutinee_slot;
            lower_pattern_tests(*arm.pattern, scrutinee_term, hir_arm.tests, true);

            if (arm.guard != nullptr) {
                hir_arm.guard = lower_expression(*arm.guard);
            }
            hir_arm.body = arm.body != nullptr
                               ? lower_expression(*arm.body)
                               : make_expr(Expr::Kind::Unit, arm.span);

            state_->scopes.pop_back();
            expr->match_arms.push_back(std::move(hir_arm));
        }
        return expr;
    }

    // ------------------------------------------------------------------
    // Arrays
    // ------------------------------------------------------------------

    // std::array::get lowers to `handle Some(receiver[index]) with { => None }`
    // using the existing handle desugaring: a cached handler-case function
    // returning None plus a per-callsite body function, so `resume` in a
    // user-supplied OutOfBounds handler flows through unchanged.
    ExprPtr lower_array_get(const ast::Expr& receiver, const ast::Expr& index,
                            const SourceSpan& span) {
        auto expr = make_expr(Expr::Kind::Handle, span);
        expr->table_ref = array_none_table();
        expr->body_fn = lower_array_get_body(receiver, index, span, expr->captures);
        return expr;
    }

    std::uint32_t lower_array_get_body(const ast::Expr& receiver, const ast::Expr& index,
                                       const SourceSpan& span,
                                       std::vector<CaptureRef>& captures) {
        const auto fn_index =
            new_function("(array get)", "std::array::get", Origin::HandleBody);

        FuncState state;
        state.fn_index = fn_index;
        state.enclosing = state_;
        state.scopes.emplace_back();
        state_ = &state;

        auto get = make_expr(Expr::Kind::ArrayGet, span);
        get->object = lower_expression(receiver);
        get->args.push_back(lower_expression(index));

        auto some = make_expr(Expr::Kind::EnumNew, span);
        some->variant_id = variant_id(some_variant_);
        some->args.push_back(std::move(get));

        Block body;
        body.items.push_back(make_return(std::move(some), span));
        fn().body = std::move(body);

        for (auto& capture : state.captures) {
            captures.push_back(CaptureRef{capture.is_local, capture.index});
        }
        fn(fn_index).capture_count = static_cast<std::uint32_t>(captures.size());
        state_ = state.enclosing;
        return fn_index;
    }

    std::uint32_t array_none_table() {
        if (!array_none_table_ready_) {
            array_none_table_ready_ = true;
            array_none_table_ = register_array_none_table();
        }
        return array_none_table_;
    }

    std::uint32_t register_array_none_table() {
        const auto fn_index =
            new_function("(array get none)", "OutOfBounds::index", Origin::HandlerCase);

        FuncState state;
        state.fn_index = fn_index;
        state.enclosing = state_;
        state.scopes.emplace_back();
        state_ = &state;
        Function& fn = this->fn();

        fn.param_names = {"size", "index"};
        state.declare("size", fn);
        state.declare("index", fn);
        fn.param_count = state.local_count;
        fn.resume_slot = state.declare(kContinuationSlot, fn);

        auto none = make_expr(Expr::Kind::EnumVariant, {});
        none->variant_id = variant_id(none_variant_);

        Block body;
        body.items.push_back(make_return(std::move(none), {}));
        fn.body = std::move(body);

        state_ = state.enclosing;

        HandlerTable table;
        HandlerEntry entry;
        entry.effect = "OutOfBounds";
        entry.operation = "index";
        entry.function = fn_index;
        table.entries.push_back(std::move(entry));
        out_.handler_tables.push_back(std::move(table));
        return static_cast<std::uint32_t>(out_.handler_tables.size() - 1);
    }

    ExprPtr lower_call(const ast::CallExpr& call) {
        const auto& binding = call.callee->binding;

        if (binding.kind != BindingKind::Function) {
            // First-class function value or dynamic callee.
            auto expr = make_expr(Expr::Kind::CallValue, call.span);
            expr->object = lower_expression(*call.callee);
            for (const auto& argument : call.arguments) {
                expr->args.push_back(lower_expression(*argument));
            }
            return expr;
        }

        const auto* function = binding.function;

        // The builtin `abort` is a fatal runtime error.
        if (function->name == "abort" && function->owner == nullptr &&
            function->declaration == nullptr) {
            auto expr = make_expr(Expr::Kind::Abort, call.span);
            lower_call_arguments(*expr, binding, call);
            return expr;
        }

        // Enum constructors (script and host) build enum values directly.
        if (function->owner != nullptr && function->owner->kind == TypeSymbol::Kind::Enum &&
            function->owner->variants.contains(function->name)) {
            auto expr = make_expr(Expr::Kind::EnumNew, call.span);
            expr->variant_id = variant_id(function->owner->variants.at(function->name));
            lower_call_arguments(*expr, binding, call);
            return expr;
        }

        // std::array::get: synthesized `handle Some(arr[i]) with { => None }`.
        if (function->owner != nullptr && function->owner->name == "array" &&
            function->name == "get" && function->declaration == nullptr) {
            const auto* member = dynamic_cast<const ast::MemberExpr*>(call.callee.get());
            if (member == nullptr || call.arguments.size() != 1 || some_variant_ == nullptr) {
                return make_expr(Expr::Kind::Unit, call.span); // error recovery
            }
            return lower_array_get(*member->object, *call.arguments[0], call.span);
        }

        if (function->is_effect_operation) {
            if (function->is_host) {
                host_function_id(function); // registered for the unhandled-op fallback
            }
            auto expr = make_expr(Expr::Kind::Raise, call.span);
            expr->effect =
                function->owner != nullptr ? function->owner->qualified_name : "";
            expr->operation = function->name;
            lower_call_arguments(*expr, binding, call);
            return expr;
        }

        if (function->is_host) {
            auto expr = make_expr(Expr::Kind::Call, call.span);
            expr->call_target = Expr::CallTarget::HostFn;
            expr->fn_id = host_function_id(function);
            lower_call_arguments(*expr, binding, call);
            return expr;
        }

        const auto fn_it = out_.function_ids.find(function);
        if (fn_it == out_.function_ids.end()) {
            // No compiled body (should not happen after typechecking).
            return make_expr(Expr::Kind::Unit, call.span);
        }

        auto expr = make_expr(Expr::Kind::Call, call.span);
        expr->call_target = Expr::CallTarget::ScriptFn;
        expr->fn_id = fn_it->second;
        lower_call_arguments(*expr, binding, call);
        return expr;
    }

    // Lowers the receiver (implicit `this` or the `.method()` object) and the
    // argument list, in that order, into a call-shaped node.
    void lower_call_arguments(Expr& expr, const Binding& binding, const ast::CallExpr& call) {
        if (binding.receiver_type != nullptr) {
            if (const auto* member =
                    dynamic_cast<const ast::MemberExpr*>(call.callee.get())) {
                expr.receiver = lower_expression(*member->object);
            } else {
                expr.receiver = load_receiver(call.callee->span);
            }
        }
        for (const auto& argument : call.arguments) {
            expr.args.push_back(lower_expression(*argument));
        }
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
        fn(state->fn_index).capture_count =
            static_cast<std::uint32_t>(state->captures.size());
        return static_cast<std::int32_t>(state->captures.size() - 1);
    }

    std::uint32_t variant_id(const EnumVariantSymbol* variant) {
        for (std::uint32_t i = 0; i < out_.enum_variants.size(); ++i) {
            if (out_.enum_variants[i] == variant) {
                return i;
            }
        }
        out_.enum_variants.push_back(variant);
        return static_cast<std::uint32_t>(out_.enum_variants.size() - 1);
    }

    std::uint32_t operator_id(const OperatorInfo* info) {
        for (std::uint32_t i = 0; i < out_.operators.size(); ++i) {
            if (out_.operators[i] == info) {
                return i;
            }
        }
        out_.operators.push_back(info);
        return static_cast<std::uint32_t>(out_.operators.size() - 1);
    }

    std::uint32_t host_function_id(const FunctionSymbol* function) {
        for (std::uint32_t i = 0; i < out_.host_functions.size(); ++i) {
            if (out_.host_functions[i] == function) {
                return i;
            }
        }
        out_.host_functions.push_back(function);
        return static_cast<std::uint32_t>(out_.host_functions.size() - 1);
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

    // ------------------------------------------------------------------
    // Node constructors
    // ------------------------------------------------------------------

    static ExprPtr make_expr(Expr::Kind kind, const SourceSpan& span) {
        auto expr = std::make_unique<Expr>();
        expr->kind = kind;
        expr->span = span;
        return expr;
    }

    static Stmt make_stmt(Stmt::Kind kind, const SourceSpan& span) {
        Stmt stmt;
        stmt.kind = kind;
        stmt.span = span;
        return stmt;
    }

    static Stmt make_return(ExprPtr value, const SourceSpan& span) {
        Stmt stmt = make_stmt(Stmt::Kind::Return, span);
        stmt.expression = std::move(value);
        return stmt;
    }

    const glscript::Program& program_;
    hir::Program out_;
    FuncState* state_ = nullptr;
    std::uint32_t pattern_counter_ = 0; // hidden locals: $match/$let/$patN
    const EnumVariantSymbol* some_variant_ = nullptr; // std Option
    const EnumVariantSymbol* none_variant_ = nullptr;
    bool array_none_table_ready_ = false; // cached `get` handler table
    std::uint32_t array_none_table_ = 0;
};

} // namespace

Program lower(const glscript::Program& program) {
    Lowerer lowerer(program);
    return lowerer.lower();
}

} // namespace glscript::hir
