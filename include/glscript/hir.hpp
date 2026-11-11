#pragma once

#include "glscript/bytecode.hpp"
#include "glscript/source.hpp"
#include "glscript/symbols.hpp"
#include "glscript/type.hpp"

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace glscript::hir {

// ---------------------------------------------------------------------------
// HIR
//
// Fully resolved, typed intermediate representation between the checker and
// the bytecode emitter. Every name the bytecode compiler used to re-resolve
// (locals vs captures vs fields, script vs host vs effect-op calls, enum
// constructors, operator overloads) is decided once during lowering and
// recorded explicitly in the node. The emitter is then a mechanical walk.
//
// Lowering also performs the desugarings:
//   - handle/handle-with bodies become functions (Origin::HandleBody), so a
//     raise is always exactly one frame above the handler's delimiter;
//   - handler cases become functions (Origin::HandlerCase) with an implicit
//     continuation parameter recorded in resume_slot;
//   - the tail expression of every block body becomes an explicit Return;
//   - logical operators become ShortCircuit nodes; unary `+` is dropped.
// ---------------------------------------------------------------------------

constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;

enum class BinaryOp {
    Add, Sub, Mul, Div, Mod,
    Eq, Ne, Lt, Le, Gt, Ge,
};

enum class UnaryOp {
    Neg, Not,
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

// One entry of a Closure node: where the emitter loads the captured value
// from in the enclosing function (a local slot or an outer capture index).
struct CaptureRef {
    bool is_local = false;
    std::uint32_t index = 0;
};

// ---------------------------------------------------------------------------
// Pattern dispatch (Expr::Kind::Match)
//
// A pattern is a conjunction of primitive tests and bindings — nested payload
// patterns just append more conjuncts — so each arm lowers to a flat test
// plan. A test operates on a *term*: the scrutinee local, or a payload slot
// of an enum-value local bound by an enclosing variant test.
// ---------------------------------------------------------------------------

struct MatchTest {
    enum class Kind {
        Variant,  // term's variant must equal variant_id
        Literal,  // term must equal the pre-built literal value node
        Bind,     // store the term's value into slot
    };
    enum class Term {
        Scrutinee,  // the match's scrutinee local
        Payload,    // payload[payload_index] of the enum value in value_slot
    };

    Kind kind = Kind::Bind;
    Term term = Term::Scrutinee;
    std::uint32_t value_slot = 0;   // Payload: the enum-value local
    std::uint32_t payload_index = 0;
    std::uint32_t variant_id = 0;   // Variant
    ExprPtr literal;                // Literal: pre-built value node
    std::uint32_t slot = 0;         // Bind: destination local
};

struct MatchArm {
    std::vector<MatchTest> tests;   // all must pass
    ExprPtr guard;                  // null when absent
    ExprPtr body;                   // pre-lowered; leaves the arm's value
    SourceSpan span;
};

struct Expr {
    enum class Kind {
        // Literals — values inline in the node.
        Unit, Bool, Int, Float, String,
        // Variables — slot/index assigned at lowering.
        Local, Capture,
        // Member access — absolute slot computed once at lowering.
        Field,          // object + field_slot
        FieldDynamic,   // object + field_name
        EnumVariant,    // payload-less variant value
        // Operators.
        IntrinsicBinary, IntrinsicUnary,
        ShortCircuit,    // && / || — emitter does the Dup/Jump dance
        OperatorCall,    // overloaded operator thunk, arity 1 or 2
        // Calls — one decision site for what used to be compile_call's tree.
        Call,            // ScriptFn | HostFn, optional receiver
        CallValue,       // first-class closure call
        Raise,           // effect operation (script or host)
        EnumNew,         // enum constructor call
        Downcast,        // checked narrowing
        BuildString,     // template string, parts flattened at lowering
        Closure,         // lambda value
        Resume,          // continuation + value on stack
        Handle,          // PushHandler / call body function / PopHandler
        Match,           // pattern dispatch; emitter does the jump patching
        EnumPayload,     // payload[index] of an enum value (pattern bindings)
        ArrayGet,        // arr[index]: element or OutOfBounds raise
        ArrayNew,        // args on stack -> array value (literals)
        Abort,           // builtin `abort(...)`: fatal, never returns
    };

    Kind kind;
    SourceSpan span;

    // Literals.
    bool bool_value = false;
    std::int64_t int_value = 0;
    double float_value = 0.0;
    std::string string_value;

    // Local / Capture / VarLocal.
    std::uint32_t slot = 0;

    // Field / FieldDynamic.
    ExprPtr object;
    std::uint32_t field_slot = 0;
    std::string field_name;

    // EnumVariant / EnumNew.
    std::uint32_t variant_id = 0;

    // IntrinsicBinary / IntrinsicUnary / ShortCircuit / OperatorCall.
    BinaryOp binary_op = BinaryOp::Add;
    UnaryOp unary_op = UnaryOp::Neg;
    bool short_circuit_is_and = false;
    std::uint32_t operator_id = 0;
    ExprPtr lhs;
    ExprPtr rhs;

    // Call.
    enum class CallTarget {
        ScriptFn,  // fn_id indexes Program::functions
        HostFn,    // fn_id indexes Program::host_functions
    };
    CallTarget call_target = CallTarget::ScriptFn;
    std::uint32_t fn_id = 0;
    ExprPtr receiver;          // implicit `this` or the `.method()` object

    // Raise.
    std::string effect;        // qualified effect name ("" for builtins)
    std::string operation;

    // Downcast.
    TypePtr downcast_target;

    // Closure.
    std::uint32_t closure_fn = 0;
    std::vector<CaptureRef> captures;

    // Resume.
    ExprPtr continuation;      // Local or Capture node
    ExprPtr resume_value;      // null = unit value

    // Handle.
    std::uint32_t body_fn = 0;
    std::uint32_t table_ref = 0;
    // `captures` (shared with Closure) carries the body function's capture
    // refs: the emitter builds the body as a closure so the handled
    // expression can reference outer locals.

    // EnumPayload.
    std::uint32_t payload_index = 0;

    // Match.
    ExprPtr scrutinee;              // evaluated once, stored into scrutinee_slot
    std::uint32_t scrutinee_slot = 0;
    std::vector<MatchArm> match_arms;

    // Argument lists / operands.
    std::vector<ExprPtr> args;   // Call, CallValue, Raise, EnumNew, OperatorCall
    std::vector<ExprPtr> parts;  // BuildString: String nodes for literal parts
};

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;
struct Block;
using BlockPtr = std::unique_ptr<Block>;

struct Stmt {
    enum class Kind {
        Expr,       // evaluate, discard the value
        VarLocal,   // declare slot with initial value (slot assigned at lowering)
        Assign,     // target: Local | Field | FieldDynamic; null target discards
                    // the value (assignment to captured variables is unsupported)
        Return,     // explicit; lowering inserts unit returns
        If, While,  // structured — the emitter does the jump patching
        Break, Continue,
    };

    Kind kind;
    SourceSpan span;

    ExprPtr expression;    // Expr / Return / VarLocal / If condition / While condition
    std::uint32_t slot = 0;          // VarLocal
    ExprPtr target;                  // Assign
    BlockPtr then_branch;            // If
    BlockPtr else_branch;            // If, null when absent
    BlockPtr body;                   // While
};

struct Block {
    std::vector<Stmt> items;
};

struct Function {
    std::string name;
    std::string qualified_name;
    enum class Origin {
        Function, Method, Lambda, Block, HandlerCase, HandleBody, ModuleInit,
    };
    Origin origin;
    bool has_receiver = false;           // methods: slot 0 is the receiver
    std::uint32_t param_count = 0;
    std::vector<std::string> param_names; // debug/dump only
    std::uint32_t capture_count = 0;
    std::uint32_t local_count = 0;
    std::uint32_t resume_slot = kNoSlot;  // HandlerCase only: continuation param slot
    Block body;
    SourceSpan span;
};

struct HandlerEntry {
    std::string effect;          // qualified effect name
    std::string operation;
    std::uint32_t function = 0;  // HandlerCase function id
};

struct HandlerTable {
    std::vector<HandlerEntry> entries;
};

// Mirrors the checker Program's tables minus the bytecode. Pointer targets
// live in the checker's Program, which must outlive the HIR and the bytecode.
struct Program {
    // Script functions and methods are pre-registered (indices stable before
    // any body lowers, so forward calls work); lambdas, handler cases, and
    // handle bodies are appended during lowering.
    std::vector<std::unique_ptr<Function>> functions;
    std::vector<HandlerTable> handler_tables;
    std::vector<const EnumVariantSymbol*> enum_variants;
    std::vector<const OperatorInfo*> operators;
    std::vector<const FunctionSymbol*> host_functions;
    std::vector<TypePtr> downcast_targets;
    std::unordered_map<const FunctionSymbol*, std::uint32_t> function_ids;
    std::unordered_map<std::string, std::uint32_t> module_inits;
    // Top-level `handle Effect { ... }` declarations: ambient handlers
    // installed for the module's whole execution.
    std::unordered_map<std::string, std::vector<std::uint32_t>> module_ambient_handlers;
};

// Lowers a checked program (AST + bindings) into HIR. Takes the checker's
// glscript::Program; the parameter is spelled fully because `Program` inside
// this namespace means hir::Program.
Program lower(const glscript::Program& program);

// Emits bytecode from a lowered program (implemented in src/compiler.cpp).
CompiledProgram emit(const Program& program);

// Prints a human-readable, indented dump of a lowered program.
void dump(const Program& program, std::ostream& out);

} // namespace glscript::hir
