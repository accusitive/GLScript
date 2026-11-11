#include "glscript/runtime.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace glscript {
namespace {

constexpr const char* kInvalidDowncastOperation = "invalid_downcast";

std::string format_float(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.10g", value);
    return buffer;
}

// Absolute field slot for an object: base-chain fields first (root first).
std::int32_t find_field_slot(const ObjectInstance* object, const std::string& name) {
    std::vector<const TypeSymbol*> chain;
    for (auto* type = object->type; type != nullptr; type = type->base) {
        chain.push_back(type);
    }

    std::uint32_t slot = 0;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const auto field = (*it)->fields.find(name);
        if (field != (*it)->fields.end()) {
            return static_cast<std::int32_t>(slot + field->second.index);
        }
        slot += static_cast<std::uint32_t>((*it)->fields.size());
    }
    return -1;
}

const TypeSymbol* value_type_symbol(const Value& value) {
    if (!value.is_heap()) {
        return nullptr;
    }
    switch (value.as_heap()->kind) {
    case GcObject::Kind::Object:
        return as_object(value)->type;
    case GcObject::Kind::Enum:
        return as_enum(value)->variant->owner;
    default:
        return nullptr;
    }
}

} // namespace

Runtime::Runtime(const Program& program, const CompiledProgram& bytecode,
                 const HostRegistry& host, DiagnosticSink& diagnostics)
    : program_(program),
      bytecode_(bytecode),
      host_(host),
      diagnostics_(diagnostics) {
    // Intern constants: strings become heap objects rooted by this table.
    constants_.reserve(bytecode_.functions.size());
    for (const auto& fn : bytecode_.functions) {
        std::vector<Value> values;
        values.reserve(fn.constants.size());
        for (const auto& constant : fn.constants) {
            if (std::holds_alternative<std::string>(constant.value)) {
                values.push_back(make_string(std::get<std::string>(constant.value)));
            } else if (std::holds_alternative<bool>(constant.value)) {
                values.push_back(Value::boolean(std::get<bool>(constant.value)));
            } else if (std::holds_alternative<std::int64_t>(constant.value)) {
                values.push_back(Value::integer(std::get<std::int64_t>(constant.value)));
            } else if (std::holds_alternative<double>(constant.value)) {
                values.push_back(Value::floating(std::get<double>(constant.value)));
            } else {
                values.push_back(Value::unit());
            }
        }
        constants_.push_back(std::move(values));
    }
}

Runtime::~Runtime() = default;

Value Runtime::make_string(std::string value) {
    return Value::heap(heap_.allocate<StringObj>(std::move(value)));
}

Value Runtime::make_object(const TypeSymbol* type, std::vector<Value> fields, void* host) {
    auto* object = heap_.allocate<ObjectInstance>();
    object->type = type;
    object->fields = std::move(fields);
    object->host = host;
    return Value::heap(object);
}

Value Runtime::make_enum(const EnumVariantSymbol* variant, std::vector<Value> payload) {
    auto* object = heap_.allocate<EnumObj>();
    object->variant = variant;
    object->payload = std::move(payload);
    return Value::heap(object);
}

Value Runtime::make_closure(std::uint32_t function, std::vector<Value> captures) {
    auto* object = heap_.allocate<ClosureObj>();
    object->function = function;
    object->captures = std::move(captures);
    return Value::heap(object);
}

const TypeSymbol* Runtime::find_type(const std::string& qualified_name) const {
    return program_.find_type(qualified_name);
}

std::string Runtime::stringify(const Value& value) {
    if (value.is_unit()) {
        return "unit";
    }
    if (value.is_bool()) {
        return value.as_bool() ? "true" : "false";
    }
    if (value.is_int()) {
        return std::to_string(value.as_int());
    }
    if (value.is_float()) {
        return format_float(value.as_float());
    }
    if (!value.is_heap()) {
        return "unknown";
    }

    switch (value.as_heap()->kind) {
    case GcObject::Kind::String:
        return as_string(value)->value;
    case GcObject::Kind::Enum: {
        const auto* enum_value = as_enum(value);
        std::string text = enum_value->variant->name;
        if (!enum_value->payload.empty()) {
            text += "(";
            for (std::size_t i = 0; i < enum_value->payload.size(); ++i) {
                if (i > 0) {
                    text += ", ";
                }
                text += stringify(enum_value->payload[i]);
            }
            text += ")";
        }
        return text;
    }
    case GcObject::Kind::Object:
        return "<" + as_object(value)->type->qualified_name + ">";
    case GcObject::Kind::Closure:
        return "<function>";
    case GcObject::Kind::Continuation:
        return "<continuation>";
    }
    return "unknown";
}

void Runtime::gc_collect() {
    std::vector<Value*> roots;
    roots.reserve(stack_.size() + frames_.size() + continuations_.size() +
                  gc_roots_.size());
    for (auto& value : stack_) {
        roots.push_back(&value);
    }
    for (auto& frame : frames_) {
        roots.push_back(&frame.closure);
    }
    for (auto& continuation : continuations_) {
        roots.push_back(&continuation);
    }
    for (auto& value : gc_roots_) {
        roots.push_back(&value);
    }
    for (auto& function_constants : constants_) {
        for (auto& value : function_constants) {
            roots.push_back(&value);
        }
    }
    heap_.collect(roots);
}

void Runtime::runtime_error(const SourceSpan& span, const std::string& message) {
    diagnostics_.error(span, "runtime: " + message);
    failed_ = true;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

Value Runtime::run(const std::string& module_name, const std::string& function_name,
                   std::vector<Value> arguments) {
    failed_ = false;
    result_ = Value::unit();
    stack_.clear();
    frames_.clear();
    handlers_.clear();

    const auto init_it = bytecode_.module_inits.find(module_name);
    if (init_it != bytecode_.module_inits.end()) {
        push_frame(init_it->second, 0, Value::unit());
        execute();
        stack_.clear();
        frames_.clear();
        handlers_.clear();
        failed_ = false; // init failures don't block the entry call
    }

    const auto* module = program_.find_module(module_name);
    if (module == nullptr) {
        diagnostics_.error(SourceSpan{}, "runtime: unknown module '" + module_name + "'");
        return Value::unit();
    }

    const auto function_it = module->functions.find(function_name);
    if (function_it == module->functions.end()) {
        diagnostics_.error(SourceSpan{}, "runtime: module '" + module_name +
                                             "' has no function named '" + function_name +
                                             "'");
        return Value::unit();
    }

    const auto fn_it = bytecode_.function_ids.find(function_it->second);
    if (fn_it == bytecode_.function_ids.end()) {
        diagnostics_.error(SourceSpan{}, "runtime: function '" + function_name +
                                             "' has no compiled body");
        return Value::unit();
    }

    for (auto& argument : arguments) {
        stack_.push_back(argument);
    }
    push_frame(fn_it->second, static_cast<std::uint32_t>(arguments.size()), Value::unit());

    // Ambient `handle Effect { ... }` declarations delimit the whole entry
    // function: a non-resuming handler case's value becomes the operation's
    // result, and `resume` continues the script normally.
    if (const auto ambient_it = bytecode_.module_ambient_handlers.find(module_name);
        ambient_it != bytecode_.module_ambient_handlers.end()) {
        for (const auto table : ambient_it->second) {
            handlers_.push_back(HandlerFrame{table, 0,
                                             static_cast<std::uint32_t>(stack_.size())});
        }
    }

    execute();

    return result_;
}

Value Runtime::call_method(const Value& receiver, const std::string& name,
                           std::vector<Value> arguments) {
    failed_ = false;
    result_ = Value::unit();

    if (!receiver.is_heap() || receiver.as_heap()->kind != GcObject::Kind::Object) {
        runtime_error(SourceSpan{}, "runtime: call_method expects an object receiver");
        return Value::unit();
    }

    // Dispatch on the instance's runtime type, walking its base chain; only
    // script methods have compiled bodies.
    const FunctionSymbol* method = nullptr;
    for (auto* type = as_object(receiver)->type; type != nullptr && method == nullptr;
         type = type->base) {
        const auto it = type->methods.find(name);
        if (it != type->methods.end() && !it->second->is_host) {
            method = it->second;
        }
    }
    if (method == nullptr) {
        runtime_error(SourceSpan{},
                      "runtime: type '" + as_object(receiver)->type->qualified_name +
                          "' has no script method '" + name + "'");
        return Value::unit();
    }
    const auto fn_it = bytecode_.function_ids.find(method);
    if (fn_it == bytecode_.function_ids.end()) {
        runtime_error(SourceSpan{}, "runtime: method '" + name + "' has no compiled body");
        return Value::unit();
    }

    // The receiver is argument slot 0, matching compiled method calls.
    stack_.push_back(receiver);
    const auto argc = static_cast<std::uint32_t>(arguments.size());
    for (auto& argument : arguments) {
        stack_.push_back(std::move(argument));
    }
    push_frame(fn_it->second, argc + 1, Value::unit());
    execute();
    return result_;
}

Value Runtime::field(const Value& object, const std::string& name) const {
    if (!object.is_heap() || object.as_heap()->kind != GcObject::Kind::Object) {
        diagnostics_.error(SourceSpan{},
                           "runtime: cannot read field '" + name + "' of " +
                               object.type_name());
        return Value::unit();
    }
    const auto slot = find_field_slot(as_object(object), name);
    if (slot < 0) {
        diagnostics_.error(SourceSpan{},
                           "runtime: type '" + as_object(object)->type->qualified_name +
                               "' has no field '" + name + "'");
        return Value::unit();
    }
    return as_object(object)->fields[static_cast<std::size_t>(slot)];
}

void Runtime::set_field(const Value& object, const std::string& name, Value value) {
    if (!object.is_heap() || object.as_heap()->kind != GcObject::Kind::Object) {
        diagnostics_.error(SourceSpan{},
                           "runtime: cannot write field '" + name + "' of " +
                               object.type_name());
        return;
    }
    const auto slot = find_field_slot(as_object(object), name);
    if (slot < 0) {
        diagnostics_.error(SourceSpan{},
                           "runtime: type '" + as_object(object)->type->qualified_name +
                               "' has no field '" + name + "'");
        return;
    }
    as_object(object)->fields[static_cast<std::size_t>(slot)] = std::move(value);
}

void Runtime::retain(Value value) {
    gc_roots_.push_back(std::move(value));
}

void Runtime::push_frame(std::uint32_t index, std::uint32_t argc, Value closure) {
    Frame frame;
    frame.function = index;
    frame.fn = bytecode_.function(index);
    frame.ip = 0;
    frame.base = static_cast<std::uint32_t>(stack_.size()) - argc;
    frame.closure = std::move(closure);
    frames_.push_back(std::move(frame));

    // Reserve the frame's local slots; parameters (the topmost argc slots)
    // already hold the call arguments.
    const std::uint32_t total = frames_.back().fn->local_count;
    while (stack_.size() < frames_.back().base + total) {
        stack_.push_back(Value::unit());
    }
}

void Runtime::call_function(std::uint32_t index, std::uint32_t argc, Value closure) {
    push_frame(index, argc, std::move(closure));
}

bool Runtime::dispatch_host_call(const FunctionSymbol* function, std::uint32_t argc,
                                 const SourceSpan& span) {
    if (function->host_call == nullptr) {
        runtime_error(span, "host function '" + function->qualified_name +
                                "' has no implementation");
        return false;
    }

    std::vector<Value> args(stack_.end() - argc, stack_.end());
    stack_.resize(stack_.size() - argc);
    stack_.push_back(function->host_call(*this, args));
    if (function->aborting) {
        runtime_error(span, "aborted by host operation '" + function->name + "'");
        return false;
    }
    return true;
}

bool Runtime::truthy(const Value& value) {
    return value.is_bool() && value.as_bool();
}

bool Runtime::values_equal(const Value& left, const Value& right) {
    if (left.is_int() && right.is_int()) {
        return left.as_int() == right.as_int();
    }
    if ((left.is_int() || left.is_float()) && (right.is_int() || right.is_float())) {
        return as_number(left) == as_number(right);
    }
    if (left.is_bool() && right.is_bool()) {
        return left.as_bool() == right.as_bool();
    }
    if (left.is_unit() && right.is_unit()) {
        return true;
    }
    if (left.is_heap() && right.is_heap()) {
        const auto* a = left.as_heap();
        const auto* b = right.as_heap();
        if (a->kind != b->kind) {
            return false;
        }
        switch (a->kind) {
        case GcObject::Kind::String:
            return as_string(left)->value == as_string(right)->value;
        case GcObject::Kind::Enum: {
            const auto* ea = as_enum(left);
            const auto* eb = as_enum(right);
            if (ea->variant != eb->variant || ea->payload.size() != eb->payload.size()) {
                return false;
            }
            for (std::size_t i = 0; i < ea->payload.size(); ++i) {
                if (!values_equal(ea->payload[i], eb->payload[i])) {
                    return false;
                }
            }
            return true;
        }
        default:
            return a == b;
        }
    }
    return false;
}

Value Runtime::binary_op(Op op, const Value& left, const Value& right,
                         const SourceSpan& span) {
    const auto numeric = [](const Value& value) {
        return value.is_int() || value.is_float();
    };

    const auto both_strings = [&] {
        return left.is_heap() && right.is_heap() &&
               left.as_heap()->kind == GcObject::Kind::String &&
               right.as_heap()->kind == GcObject::Kind::String;
    };

    switch (op) {
    case Op::Add:
        if (left.is_int() && right.is_int()) {
            return Value::integer(left.as_int() + right.as_int());
        }
        if (numeric(left) && numeric(right)) {
            return Value::floating(as_number(left) + as_number(right));
        }
        if (both_strings()) {
            return make_string(as_string(left)->value + as_string(right)->value);
        }
        runtime_error(span, "cannot add " + left.type_name() + " and " + right.type_name());
        return Value::unit();

    case Op::Sub:
    case Op::Mul:
    case Op::Div:
    case Op::Mod: {
        if (!numeric(left) || !numeric(right)) {
            runtime_error(span, "operator expects numeric operands, got " +
                                    left.type_name() + " and " + right.type_name());
            return Value::unit();
        }
        if (op == Op::Mod) {
            if (!left.is_int() || !right.is_int()) {
                runtime_error(span, "operator '%' expects integer operands");
                return Value::unit();
            }
            if (right.as_int() == 0) {
                runtime_error(span, "integer modulo by zero");
                return Value::unit();
            }
            return Value::integer(left.as_int() % right.as_int());
        }
        if (left.is_int() && right.is_int()) {
            const auto a = left.as_int();
            const auto b = right.as_int();
            switch (op) {
            case Op::Sub: return Value::integer(a - b);
            case Op::Mul: return Value::integer(a * b);
            case Op::Div:
                if (b == 0) {
                    runtime_error(span, "integer division by zero");
                    return Value::unit();
                }
                return Value::integer(a / b);
            default: break;
            }
        }
        switch (op) {
        case Op::Sub: return Value::floating(as_number(left) - as_number(right));
        case Op::Mul: return Value::floating(as_number(left) * as_number(right));
        case Op::Div: return Value::floating(as_number(left) / as_number(right));
        default: break;
        }
        break;
    }

    case Op::Eq:
        return Value::boolean(values_equal(left, right));
    case Op::Ne:
        return Value::boolean(!values_equal(left, right));

    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge: {
        if (!numeric(left) || !numeric(right)) {
            runtime_error(span, "comparison expects numeric operands, got " +
                                    left.type_name() + " and " + right.type_name());
            return Value::unit();
        }
        bool result = false;
        if (left.is_int() && right.is_int()) {
            const auto a = left.as_int();
            const auto b = right.as_int();
            result = op == Op::Lt   ? a < b
                     : op == Op::Le ? a <= b
                     : op == Op::Gt ? a > b
                                    : a >= b;
        } else {
            const auto a = as_number(left);
            const auto b = as_number(right);
            result = op == Op::Lt   ? a < b
                     : op == Op::Le ? a <= b
                     : op == Op::Gt ? a > b
                                    : a >= b;
        }
        return Value::boolean(result);
    }

    default:
        break;
    }

    runtime_error(span, "invalid operand types");
    return Value::unit();
}

// ---------------------------------------------------------------------------
// Effects and continuations
// ---------------------------------------------------------------------------

const FunctionSymbol* find_host_operation(const std::vector<const FunctionSymbol*>& functions,
                                          const std::string& effect,
                                          const std::string& operation) {
    const FunctionSymbol* by_name = nullptr;
    for (const auto* function : functions) {
        if (!function->is_effect_operation || function->name != operation) {
            continue;
        }
        const auto qualified = function->owner != nullptr ? function->owner->qualified_name
                                                          : std::string{};
        if (effect.empty() || qualified == effect) {
            return function;
        }
        if (by_name == nullptr) {
            by_name = function;
        }
    }
    return by_name;
}

void Runtime::raise_effect(const std::string& effect, const std::string& operation,
                           std::uint32_t argc, const SourceSpan& span) {
    // Innermost matching handler wins.
    for (std::size_t h = handlers_.size(); h-- > 0;) {
        const auto& handler = handlers_[h];
        const auto& table = bytecode_.handler_tables[handler.table];
        for (const auto& entry : table.entries) {
            if (entry.operation != operation) {
                continue;
            }
            if (!effect.empty() && !entry.effect.empty() && entry.effect != effect) {
                continue;
            }
            invoke_handler(handler, entry, static_cast<std::uint32_t>(h), argc, span);
            return;
        }
    }

    // No handler: fall back to the host implementation of the operation.
    const auto* host_operation =
        find_host_operation(bytecode_.host_functions, effect, operation);
    if (host_operation != nullptr) {
        dispatch_host_call(host_operation, argc, span);
        return;
    }

    runtime_error(span, "unhandled operation '" +
                            (effect.empty() ? operation : effect + "::" + operation) + "'");
}

void Runtime::invoke_handler(const HandlerFrame& handler, const HandlerEntry& entry,
                             std::uint32_t handler_index, std::uint32_t argc,
                             const SourceSpan& span) {
    (void)span;

    // Capture the continuation: the fresh frames above the delimiter, the
    // stack from the bottom captured frame's base, and the handlers whose
    // delimiters sit in the captured region. Frames installed by a resume
    // splice stay live and paused below the handler activation — they
    // receive the handler's return value at their raise site — so the
    // continuation only needs what is not already running.
    const std::uint32_t delimiter = handler.frame_index;
    const std::uint32_t current = static_cast<std::uint32_t>(frames_.size() - 1);

    std::uint32_t capture_start = delimiter + 1;
    for (std::uint32_t i = current; i > delimiter; --i) {
        if (frames_[i].resumed_slice) {
            capture_start = i + 1;
            break;
        }
    }
    const bool has_frames = capture_start <= current;

    auto* continuation = heap_.allocate<ContinuationObj>();
    continuation->frame_base = capture_start;
    if (has_frames) {
        continuation->stack_base = frames_[capture_start].base;
        continuation->stack.assign(stack_.begin() + continuation->stack_base, stack_.end());
        for (std::uint32_t i = capture_start; i <= current; ++i) {
            ContinuationFrame frame;
            frame.function = frames_[i].function;
            frame.ip = frames_[i].ip;
            frame.base = frames_[i].base;
            frame.closure = frames_[i].closure;
            continuation->frames.push_back(frame);
        }
    }
    for (std::size_t i = handler_index + 1; i < handlers_.size(); ++i) {
        if (!has_frames || handlers_[i].frame_index >= capture_start) {
            continuation->handlers.push_back(ContinuationHandler{
                handlers_[i].table, handlers_[i].frame_index, handlers_[i].stack_base});
        }
    }
    continuations_.push_back(Value::heap(continuation)); // GC root

    // Suspend the captured region and activate the handler on top of what
    // stayed live. When the handler returns, its value lands exactly where
    // the raise site's caller expects the operation result — and for the
    // outermost handler, the value of the whole `handle` expression.
    std::vector<Value> args(stack_.end() - argc, stack_.end());
    if (has_frames) {
        stack_.resize(continuation->stack_base);
    }
    for (auto& argument : args) {
        stack_.push_back(argument);
    }
    stack_.push_back(Value::heap(continuation));

    frames_.resize(capture_start);
    while (handlers_.size() > handler_index + 1 &&
           handlers_.back().frame_index >= capture_start) {
        handlers_.pop_back(); // suspended with the captured frames
    }

    push_frame(entry.function, argc + 1, Value::unit());
}

bool Runtime::resume_continuation(const Value& continuation_value, Value result) {
    if (!continuation_value.is_heap() ||
        continuation_value.as_heap()->kind != GcObject::Kind::Continuation) {
        return false;
    }
    const auto* continuation = as_continuation(continuation_value);

    if (continuation->frames.empty()) {
        // Identity continuation: the raise happened in a frame that stayed
        // live (the delimiter frame itself, or a resumed slice). The resumed
        // value is the operation's result and the handler keeps running.
        stack_.push_back(result);
        return true;
    }

    // Transfer: the resuming activation is replaced by the captured
    // computation. The continuation's stack slice lands at the resumer's
    // base, so when the spliced computation returns, its completion takes
    // exactly the place of the resumer's return value — the resumer's own
    // remaining code is abandoned, tail-position resume semantics.
    const std::uint32_t resumer_base = frames_.back().base;
    frames_.pop_back();
    while (!handlers_.empty() && handlers_.back().frame_index >= frames_.size()) {
        handlers_.pop_back(); // delimited by the replaced activation
    }
    stack_.resize(resumer_base);

    const std::uint32_t stack_delta = resumer_base - continuation->stack_base;
    stack_.insert(stack_.end(), continuation->stack.begin(), continuation->stack.end());

    const std::uint32_t frames_delta =
        static_cast<std::uint32_t>(frames_.size()) - continuation->frame_base;
    for (const auto& captured : continuation->frames) {
        Frame frame;
        frame.function = captured.function;
        frame.fn = bytecode_.function(captured.function);
        frame.ip = captured.ip;
        frame.base = captured.base + stack_delta;
        frame.closure = captured.closure;
        frame.resumed_slice = true;
        frames_.push_back(std::move(frame));
    }
    for (const auto& captured : continuation->handlers) {
        handlers_.push_back(HandlerFrame{captured.table,
                                         captured.frame_index + frames_delta,
                                         captured.stack_base + stack_delta});
    }

    stack_.push_back(result);
    return true;
}

// ---------------------------------------------------------------------------
// Interpreter loop
// ---------------------------------------------------------------------------

void Runtime::execute() {
    while (!failed_ && !frames_.empty()) {
        if (heap_.object_count() >= heap_.threshold()) {
            gc_collect();
        }

        Frame& frame = frames_.back();
        const std::uint32_t ip = frame.ip;
        const std::uint32_t* code = frame.fn->code.data();
        const auto op = static_cast<Op>(code[ip]);
        const std::uint32_t* operands = code + ip + 1;
        frame.ip = ip + 1 + static_cast<std::uint32_t>(operand_count(op));
        const SourceSpan span = frame.fn->spans[ip];

        switch (op) {
        case Op::LoadConst:
            stack_.push_back(constants_[frame.function][operands[0]]);
            break;

        case Op::LoadLocal:
            stack_.push_back(stack_[frame.base + operands[0]]);
            break;

        case Op::StoreLocal: {
            const auto value = pop();
            stack_[frame.base + operands[0]] = value;
            break;
        }

        case Op::LoadCapture: {
            const auto* closure = as_closure(frame.closure);
            stack_.push_back(closure->captures[operands[0]]);
            break;
        }

        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod:
        case Op::Eq:
        case Op::Ne:
        case Op::Lt:
        case Op::Le:
        case Op::Gt:
        case Op::Ge: {
            const auto right = pop();
            const auto left = pop();
            stack_.push_back(binary_op(op, left, right, span));
            break;
        }

        case Op::Neg: {
            const auto value = pop();
            if (value.is_int()) {
                stack_.push_back(Value::integer(-value.as_int()));
            } else if (value.is_float()) {
                stack_.push_back(Value::floating(-value.as_float()));
            } else {
                runtime_error(span, "cannot negate " + value.type_name());
                stack_.push_back(Value::unit());
            }
            break;
        }

        case Op::Not: {
            const auto value = pop();
            if (value.is_bool()) {
                stack_.push_back(Value::boolean(!value.as_bool()));
            } else {
                runtime_error(span, "cannot negate " + value.type_name());
                stack_.push_back(Value::unit());
            }
            break;
        }

        case Op::Jump:
            frame.ip = operands[0];
            break;

        case Op::JumpIfFalse:
            if (!truthy(pop())) {
                frame.ip = operands[0];
            }
            break;

        case Op::JumpIfTrue:
            if (truthy(pop())) {
                frame.ip = operands[0];
            }
            break;

        case Op::Loop:
            frame.ip = operands[0];
            break;

        case Op::Call: {
            const auto argc = operands[0];
            const auto callee_position = stack_.size() - argc - 1;
            auto callee = stack_[callee_position];
            stack_.erase(stack_.begin() + static_cast<std::ptrdiff_t>(callee_position));
            if (callee.is_heap() && callee.as_heap()->kind == GcObject::Kind::Closure) {
                call_function(as_closure(callee)->function, argc, callee);
            } else {
                runtime_error(span, "value of type " + callee.type_name() +
                                        " is not callable");
            }
            break;
        }

        case Op::CallFunction:
            call_function(operands[0], operands[1], Value::unit());
            break;

        case Op::CallHost:
            if (!dispatch_host_call(bytecode_.host_functions[operands[0]], operands[1],
                                    span)) {
                stack_.push_back(Value::unit());
            }
            break;

        case Op::CallOperator: {
            const auto* operator_info = bytecode_.operators[operands[0]];
            if (operator_info->host_call == nullptr) {
                runtime_error(span, "operator has no host implementation");
                stack_.push_back(Value::unit());
                break;
            }
            std::vector<Value> args(stack_.end() - operands[1], stack_.end());
            stack_.resize(stack_.size() - operands[1]);
            stack_.push_back(operator_info->host_call(*this, args));
            break;
        }

        case Op::Raise: {
            const auto effect = as_string(constants_[frame.function][operands[0]])->value;
            const auto operation =
                as_string(constants_[frame.function][operands[1]])->value;
            raise_effect(effect, operation, operands[2], span);
            break;
        }

        case Op::Resume: {
            const auto value = pop();
            const auto continuation_value = pop();
            if (!resume_continuation(continuation_value, value)) {
                runtime_error(span, "resume expects a continuation");
                stack_.push_back(Value::unit());
            }
            break;
        }

        case Op::Return: {
            const auto value = pop();
            const auto base = frame.base;
            frames_.pop_back();
            stack_.resize(base);
            stack_.push_back(value);
            if (frames_.empty()) {
                result_ = value; // the entry call's return value
            }
            while (!handlers_.empty() &&
                   handlers_.back().frame_index >= frames_.size()) {
                handlers_.pop_back();
            }
            break;
        }

        case Op::PushHandler:
            handlers_.push_back(HandlerFrame{operands[0],
                                             static_cast<std::uint32_t>(frames_.size() - 1),
                                             static_cast<std::uint32_t>(stack_.size())});
            break;

        case Op::PopHandler:
            if (handlers_.empty() ||
                handlers_.back().frame_index != frames_.size() - 1) {
                runtime_error(span, "internal: handler stack mismatch");
            } else {
                handlers_.pop_back();
            }
            break;

        case Op::EnumVariant:
            stack_.push_back(make_enum(bytecode_.enum_variants[operands[0]], {}));
            break;

        case Op::EnumIsVariant: {
            const auto value = pop();
            const bool matches = value.is_heap() &&
                                 value.as_heap()->kind == GcObject::Kind::Enum &&
                                 as_enum(value)->variant == bytecode_.enum_variants[operands[0]];
            stack_.push_back(Value::boolean(matches));
            break;
        }

        case Op::EnumPayload: {
            const auto value = pop();
            if (!value.is_heap() || value.as_heap()->kind != GcObject::Kind::Enum) {
                runtime_error(span, "pattern payload access on " + value.type_name());
                stack_.push_back(Value::unit());
                break;
            }
            const auto* enum_value = as_enum(value);
            if (operands[0] >= enum_value->payload.size()) {
                runtime_error(span, "variant '" + enum_value->variant->name +
                                        "' has no payload at index " +
                                        std::to_string(operands[0]));
                stack_.push_back(Value::unit());
                break;
            }
            stack_.push_back(enum_value->payload[operands[0]]);
            break;
        }

        case Op::EnumCtor: {
            const auto argc = operands[1];
            std::vector<Value> payload(stack_.end() - argc, stack_.end());
            stack_.resize(stack_.size() - argc);
            stack_.push_back(make_enum(bytecode_.enum_variants[operands[0]],
                                       std::move(payload)));
            break;
        }

        case Op::GetField: {
            const auto object = pop();
            if (!object.is_heap() || object.as_heap()->kind != GcObject::Kind::Object) {
                runtime_error(span, "cannot read field of " + object.type_name());
                stack_.push_back(Value::unit());
                break;
            }
            const auto* instance = as_object(object);
            if (operands[0] >= instance->fields.size()) {
                runtime_error(span, "type " + instance->type->qualified_name +
                                        " has no field at slot " +
                                        std::to_string(operands[0]));
                stack_.push_back(Value::unit());
                break;
            }
            stack_.push_back(instance->fields[operands[0]]);
            break;
        }

        case Op::SetField: {
            const auto value = pop();
            const auto object = pop();
            if (!object.is_heap() || object.as_heap()->kind != GcObject::Kind::Object) {
                runtime_error(span, "cannot write field of " + object.type_name());
                break;
            }
            auto* instance = as_object(object);
            if (operands[0] >= instance->fields.size()) {
                runtime_error(span, "type " + instance->type->qualified_name +
                                        " has no field at slot " +
                                        std::to_string(operands[0]));
                break;
            }
            instance->fields[operands[0]] = value;
            break;
        }

        case Op::GetFieldByName: {
            const auto name = as_string(constants_[frame.function][operands[0]])->value;
            const auto object = pop();
            if (!object.is_heap() || object.as_heap()->kind != GcObject::Kind::Object) {
                runtime_error(span, "cannot read field '" + name + "' of " +
                                        object.type_name());
                stack_.push_back(Value::unit());
                break;
            }
            const auto slot = find_field_slot(as_object(object), name);
            if (slot < 0) {
                runtime_error(span, "type " + as_object(object)->type->qualified_name +
                                        " has no field '" + name + "'");
                stack_.push_back(Value::unit());
                break;
            }
            stack_.push_back(as_object(object)->fields[static_cast<std::size_t>(slot)]);
            break;
        }

        case Op::SetFieldByName: {
            const auto name = as_string(constants_[frame.function][operands[0]])->value;
            const auto value = pop();
            const auto object = pop();
            if (!object.is_heap() || object.as_heap()->kind != GcObject::Kind::Object) {
                runtime_error(span, "cannot write field '" + name + "' of " +
                                        object.type_name());
                break;
            }
            const auto slot = find_field_slot(as_object(object), name);
            if (slot < 0) {
                runtime_error(span, "type " + as_object(object)->type->qualified_name +
                                        " has no field '" + name + "'");
                break;
            }
            as_object(object)->fields[static_cast<std::size_t>(slot)] = value;
            break;
        }

        case Op::Downcast: {
            const auto value = pop();
            const auto& target = bytecode_.downcast_targets[operands[0]];
            if (downcast_matches(target, value)) {
                stack_.push_back(value);
            } else {
                stack_.push_back(make_string("cannot downcast " + value.type_name() +
                                             " to " + target->to_string()));
                raise_effect("", kInvalidDowncastOperation, 1, span);
            }
            break;
        }

        case Op::BuildString: {
            const auto argc = operands[0];
            std::string text;
            for (std::uint32_t i = 0; i < argc; ++i) {
                text += stringify(stack_[stack_.size() - argc + i]);
            }
            stack_.resize(stack_.size() - argc);
            stack_.push_back(make_string(std::move(text)));
            break;
        }

        case Op::Closure: {
            const auto capture_count = operands[1];
            std::vector<Value> captures(stack_.end() - capture_count, stack_.end());
            stack_.resize(stack_.size() - capture_count);
            stack_.push_back(make_closure(operands[0], std::move(captures)));
            break;
        }

        case Op::Abort:
            runtime_error(span, "aborted");
            break;

        case Op::Dup:
            stack_.push_back(stack_.back());
            break;

        case Op::Pop:
            pop();
            break;
        }
    }
}

bool Runtime::downcast_matches(const TypePtr& target, const Value& value) const {
    switch (target->kind) {
    case Type::Kind::Builtin:
        switch (target->builtin) {
        case BuiltinType::Int: return value.is_int();
        case BuiltinType::Float: return value.is_float();
        case BuiltinType::Bool: return value.is_bool();
        case BuiltinType::String:
            return value.is_heap() && value.as_heap()->kind == GcObject::Kind::String;
        case BuiltinType::Unit: return value.is_unit();
        case BuiltinType::Dynamic: return true;
        case BuiltinType::Never:
        case BuiltinType::Error: return false;
        }
        return false;

    case Type::Kind::Named: {
        const auto* value_type = value_type_symbol(value);
        if (value_type == nullptr) {
            return false;
        }
        for (auto* type = value_type; type != nullptr; type = type->base) {
            if (type->qualified_name == target->name) {
                return true;
            }
        }
        return false;
    }

    case Type::Kind::Function:
        return value.is_heap() && value.as_heap()->kind == GcObject::Kind::Closure;

    case Type::Kind::TypeParameter:
        return true;
    }
    return false;
}

} // namespace glscript
