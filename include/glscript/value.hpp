#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace glscript {

struct TypeSymbol;
struct EnumVariantSymbol;
struct FunctionSymbol;
struct FieldSymbol;
struct OperatorInfo;

// ---------------------------------------------------------------------------
// Runtime values
//
// Scalars are stored inline; every compound value lives on the GC heap and is
// referenced through a GcObject pointer. Values are copied freely; heap
// objects are shared (reference semantics) except that continuation snapshots
// are never mutated, which is what makes multi-shot resume safe.
// ---------------------------------------------------------------------------

struct GcObject;
struct StringObj;
struct EnumObj;
struct ObjectInstance;
struct ClosureObj;
struct ContinuationObj;

struct Value {
    std::variant<std::monostate, bool, int64_t, double, GcObject*> data;

    Value() = default;

    static Value unit() { return Value{}; }
    static Value boolean(bool value) { auto v = Value{}; v.data = value; return v; }
    static Value integer(int64_t value) { auto v = Value{}; v.data = value; return v; }
    static Value floating(double value) { auto v = Value{}; v.data = value; return v; }
    static Value heap(GcObject* object) { auto v = Value{}; v.data = object; return v; }

    bool is_unit() const { return std::holds_alternative<std::monostate>(data); }
    bool is_bool() const { return std::holds_alternative<bool>(data); }
    bool is_int() const { return std::holds_alternative<int64_t>(data); }
    bool is_float() const { return std::holds_alternative<double>(data); }
    bool is_heap() const { return std::holds_alternative<GcObject*>(data); }

    bool as_bool() const { return std::get<bool>(data); }
    int64_t as_int() const { return std::get<int64_t>(data); }
    double as_float() const { return std::get<double>(data); }
    GcObject* as_heap() const { return std::get<GcObject*>(data); }

    // Type name for diagnostics ("unit", "bool", "int", "float", "String",
    // the enum/class qualified name, "function", "continuation").
    std::string type_name() const;
};

// A host-callable thunk. Receives the arguments (the receiver is args[0] for
// instance methods); the registry owns the thunk and outlives the runtime.
using HostCall = std::function<Value(std::vector<Value>& args)>;

// ---------------------------------------------------------------------------
// GC objects
// ---------------------------------------------------------------------------

struct GcObject {
    enum class Kind {
        String,
        Enum,
        Object,
        Closure,
        Continuation,
    };

    Kind kind;
    bool marked = false;

    explicit GcObject(Kind kind) : kind(kind) {}
    virtual ~GcObject() = default;

    // Reports every heap value reachable from this object.
    virtual void trace(const std::function<void(GcObject*)>& visit) = 0;
};

struct StringObj : GcObject {
    std::string value;

    explicit StringObj(std::string value)
        : GcObject(Kind::String), value(std::move(value)) {}

    void trace(const std::function<void(GcObject*)>&) override {}
};

struct EnumObj : GcObject {
    const EnumVariantSymbol* variant = nullptr;
    std::vector<Value> payload;

    void trace(const std::function<void(GcObject*)>& visit) override {
        for (auto& value : payload) {
            if (value.is_heap()) {
                visit(value.as_heap());
            }
        }
    }
};

// Instances of classes. Script-defined classes store their field slots here;
// engine-backed instances additionally carry an opaque host pointer that the
// host owns (never traced, never collected). A host class with no script
// fields is simply an instance with an empty field vector.
struct ObjectInstance : GcObject {
    const TypeSymbol* type = nullptr;
    std::vector<Value> fields;
    void* host = nullptr;

    void trace(const std::function<void(GcObject*)>& visit) override {
        for (auto& value : fields) {
            if (value.is_heap()) {
                visit(value.as_heap());
            }
        }
    }
};

struct ClosureObj : GcObject {
    std::uint32_t function = 0;
    std::vector<Value> captures;

    void trace(const std::function<void(GcObject*)>& visit) override {
        for (auto& value : captures) {
            if (value.is_heap()) {
                visit(value.as_heap());
            }
        }
    }
};

// A delimited continuation: the captured machine state above a handler's
// delimiter. Frames and stack slices are stored exactly as captured; resume
// splices shifted copies into the live machine, so one continuation object
// can be resumed any number of times.
struct ContinuationFrame {
    std::uint32_t function = 0; // index into CompiledProgram::functions
    std::uint32_t ip = 0;
    std::uint32_t base = 0;     // stack base at capture
};

struct ContinuationHandler {
    std::uint32_t table = 0;    // index into CompiledProgram::handler_tables
    std::uint32_t frame_index = 0;
    std::uint32_t stack_base = 0;
};

struct ContinuationObj : GcObject {
    std::uint32_t stack_base = 0; // stack slice start (delimiter frame's base) at capture
    std::vector<Value> stack;
    std::vector<ContinuationFrame> frames;
    std::vector<ContinuationHandler> handlers;

    void trace(const std::function<void(GcObject*)>& visit) override {
        for (auto& value : stack) {
            if (value.is_heap()) {
                visit(value.as_heap());
            }
        }
    }
};

// Convenience accessors for heap values.
StringObj* as_string(const Value& value);
EnumObj* as_enum(const Value& value);
ObjectInstance* as_object(const Value& value);
ClosureObj* as_closure(const Value& value);
ContinuationObj* as_continuation(const Value& value);

// ---------------------------------------------------------------------------
// Heap: non-moving mark-sweep collector
//
// The heap owns every GcObject. Collection is triggered by the interpreter
// when the object count crosses a threshold; roots are provided by the
// interpreter (value stack + globals). Host pointers inside ObjectInstance
// are foreign memory and are never traced.
// ---------------------------------------------------------------------------

class Heap {
public:
    template <typename T, typename... Args>
    T* allocate(Args&&... args) {
        auto object = std::make_unique<T>(std::forward<Args>(args)...);
        auto* raw = object.get();
        objects_.push_back(std::move(object));
        return raw;
    }

    // Marks from the given roots and destroys every unmarked object.
    void collect(const std::vector<Value*>& roots);

    std::size_t object_count() const { return objects_.size(); }

    // Number of live objects after the last collection (for sizing the next
    // trigger threshold).
    void set_threshold(std::size_t objects) { threshold_ = objects; }
    std::size_t threshold() const { return threshold_; }

private:
    std::vector<std::unique_ptr<GcObject>> objects_;
    std::size_t threshold_ = 1024;
};

} // namespace glscript
