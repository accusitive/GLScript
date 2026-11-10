#pragma once

#include "glscript/bytecode.hpp"
#include "glscript/diagnostic.hpp"
#include "glscript/host.hpp"
#include "glscript/value.hpp"

#include <string>
#include <vector>

namespace glscript {

// The bytecode interpreter. All machine state lives in interpreter-owned
// vectors, so capturing a delimited continuation is a copy of the frames and
// stack above a handler's delimiter — and because the captured snapshot is
// never mutated, one continuation can be resumed any number of times.
class Runtime {
public:
    Runtime(const Program& program, const CompiledProgram& bytecode,
            const HostRegistry& host, DiagnosticSink& diagnostics);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Runs the entry module's init function (top-level handler declarations),
    // then calls the named function with the given arguments.
    Value run(const std::string& module_name, const std::string& function_name,
              std::vector<Value> arguments = {});

    // Forces a collection (the engine can call this between frames).
    void gc_collect();

    // Value construction for host thunks.
    Value make_string(std::string value);
    Value make_object(const TypeSymbol* type, std::vector<Value> fields, void* host = nullptr);
    Value make_enum(const EnumVariantSymbol* variant, std::vector<Value> payload);
    Value make_closure(std::uint32_t function, std::vector<Value> captures);

    // Renders any value as a string (template strings, concat, println).
    std::string stringify(const Value& value);

private:
    struct Frame {
        std::uint32_t function = 0;
        const FunctionBytecode* fn = nullptr;
        std::uint32_t ip = 0;
        std::uint32_t base = 0;
        Value closure; // ClosureObj when invoked as a first-class value
    };

    struct HandlerFrame {
        std::uint32_t table = 0;       // index into handler_tables
        std::uint32_t frame_index = 0; // delimiter: frame that pushed the handler
        std::uint32_t stack_base = 0;
    };

    void execute();
    void call_function(std::uint32_t index, std::uint32_t argc, Value closure);
    void raise_effect(const std::string& effect, const std::string& operation,
                      std::uint32_t argc, const SourceSpan& span);
    void invoke_handler(const HandlerFrame& handler, const HandlerEntry& entry,
                        std::uint32_t handler_index, std::uint32_t argc,
                        const SourceSpan& span);
    bool resume_continuation(const Value& continuation_value, Value result);
    bool dispatch_host_call(const FunctionSymbol* function, std::uint32_t argc,
                            const SourceSpan& span);
    bool downcast_matches(const TypePtr& target, const Value& value) const;
    void runtime_error(const SourceSpan& span, const std::string& message);

    // Arithmetic/comparison with int/float/string/dynamic dispatch.
    Value binary_op(Op op, const Value& left, const Value& right, const SourceSpan& span);
    static bool truthy(const Value& value);
    static bool values_equal(const Value& left, const Value& right);

    const FunctionBytecode* current_fn() const {
        return frames_.back().fn;
    }

    Value pop() {
        auto value = stack_.back();
        stack_.pop_back();
        return value;
    }

    const Program& program_;
    const CompiledProgram& bytecode_;
    const HostRegistry& host_;
    DiagnosticSink& diagnostics_;
    Heap heap_;

    std::vector<Value> stack_;
    std::vector<Frame> frames_;
    std::vector<HandlerFrame> handlers_;
    std::vector<std::vector<Value>> constants_; // per function, strings interned
    Value result_;
    bool failed_ = false;
};

} // namespace glscript
