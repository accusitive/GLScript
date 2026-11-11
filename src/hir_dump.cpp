#include "glscript/hir.hpp"

#include <ostream>
#include <string>

namespace glscript::hir {
namespace {

const char* origin_name(Function::Origin origin) {
    switch (origin) {
    case Function::Origin::Function: return "function";
    case Function::Origin::Method: return "method";
    case Function::Origin::Lambda: return "lambda";
    case Function::Origin::Block: return "block";
    case Function::Origin::HandlerCase: return "handler case";
    case Function::Origin::HandleBody: return "handle body";
    case Function::Origin::ModuleInit: return "module init";
    }
    return "?";
}

const char* binary_op_name(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Sub: return "-";
    case BinaryOp::Mul: return "*";
    case BinaryOp::Div: return "/";
    case BinaryOp::Mod: return "%";
    case BinaryOp::Eq: return "==";
    case BinaryOp::Ne: return "!=";
    case BinaryOp::Lt: return "<";
    case BinaryOp::Le: return "<=";
    case BinaryOp::Gt: return ">";
    case BinaryOp::Ge: return ">=";
    }
    return "?";
}

class Dumper {
public:
    explicit Dumper(std::ostream& out) : out_(out) {}

    void dump(const Program& program) {
        for (std::uint32_t i = 0; i < program.functions.size(); ++i) {
            dump_function(i, *program.functions[i]);
        }
    }

private:
    void indent() {
        for (int i = 0; i < depth_; ++i) {
            out_ << "  ";
        }
    }

    void dump_function(std::uint32_t index, const Function& function) {
        out_ << "fn " << index << " " << function.qualified_name << " ("
             << origin_name(function.origin);
        if (function.has_receiver) {
            out_ << ", receiver";
        }
        out_ << ", params=" << function.param_count
             << ", locals=" << function.local_count
             << ", captures=" << function.capture_count;
        if (function.resume_slot != kNoSlot) {
            out_ << ", resume_slot=" << function.resume_slot;
        }
        out_ << ")\n";

        ++depth_;
        for (const auto& statement : function.body.items) {
            dump_statement(statement);
        }
        --depth_;
    }

    void dump_statement(const Stmt& statement) {
        indent();
        switch (statement.kind) {
        case Stmt::Kind::Expr:
            dump_expression(*statement.expression);
            out_ << ";\n";
            break;
        case Stmt::Kind::VarLocal:
            out_ << "var local " << statement.slot << " = ";
            dump_expression(*statement.expression);
            out_ << "\n";
            break;
        case Stmt::Kind::Assign:
            out_ << "assign ";
            if (statement.target != nullptr) {
                dump_expression(*statement.target);
            } else {
                out_ << "(discard)";
            }
            out_ << " = ";
            dump_expression(*statement.expression);
            out_ << "\n";
            break;
        case Stmt::Kind::Return:
            out_ << "return ";
            dump_expression(*statement.expression);
            out_ << "\n";
            break;
        case Stmt::Kind::If:
            out_ << "if ";
            dump_expression(*statement.expression);
            out_ << " {\n";
            dump_block(*statement.then_branch);
            if (statement.else_branch != nullptr) {
                indent();
                out_ << "} else {\n";
                dump_block(*statement.else_branch);
            }
            indent();
            out_ << "}\n";
            break;
        case Stmt::Kind::While:
            out_ << "while ";
            dump_expression(*statement.expression);
            out_ << " {\n";
            dump_block(*statement.body);
            indent();
            out_ << "}\n";
            break;
        case Stmt::Kind::Break:
            out_ << "break\n";
            break;
        case Stmt::Kind::Continue:
            out_ << "continue\n";
            break;
        }
    }

    void dump_block(const Block& block) {
        ++depth_;
        for (const auto& statement : block.items) {
            dump_statement(statement);
        }
        --depth_;
    }

    void dump_call_args(const Expr& expression) {
        if (expression.receiver != nullptr) {
            out_ << "recv=";
            dump_expression(*expression.receiver);
            if (!expression.args.empty()) {
                out_ << ", ";
            }
        }
        for (std::size_t i = 0; i < expression.args.size(); ++i) {
            if (i != 0) {
                out_ << ", ";
            }
            dump_expression(*expression.args[i]);
        }
    }

    // The value a match test operates on: the scrutinee local, or a payload
    // slot of an enum value bound by an enclosing variant test.
    void dump_match_term(const MatchTest& test) {
        out_ << "local " << test.value_slot;
        if (test.term == MatchTest::Term::Payload) {
            out_ << "[" << test.payload_index << "]";
        }
    }

    void dump_match(const Expr& expression) {
        out_ << "match local " << expression.scrutinee_slot << " = ";
        dump_expression(*expression.scrutinee);
        out_ << " {\n";
        ++depth_;
        for (std::size_t i = 0; i < expression.match_arms.size(); ++i) {
            const auto& arm = expression.match_arms[i];
            indent();
            out_ << "arm " << i << ":";
            for (const auto& test : arm.tests) {
                switch (test.kind) {
                case MatchTest::Kind::Variant:
                    out_ << " variant " << test.variant_id << "?";
                    break;
                case MatchTest::Kind::Literal:
                    out_ << " == ";
                    dump_expression(*test.literal);
                    break;
                case MatchTest::Kind::Bind:
                    out_ << " bind local " << test.slot << " = ";
                    dump_match_term(test);
                    break;
                }
            }
            if (arm.guard != nullptr) {
                out_ << " if ";
                dump_expression(*arm.guard);
            }
            out_ << " => ";
            dump_expression(*arm.body);
            out_ << "\n";
        }
        --depth_;
        indent();
        out_ << "}";
    }

    void dump_expression(const Expr& expression) {
        switch (expression.kind) {
        case Expr::Kind::Unit: out_ << "unit"; break;
        case Expr::Kind::Bool: out_ << (expression.bool_value ? "true" : "false"); break;
        case Expr::Kind::Int: out_ << expression.int_value; break;
        case Expr::Kind::Float: out_ << expression.float_value; break;
        case Expr::Kind::String:
            out_ << '"' << expression.string_value << '"';
            break;
        case Expr::Kind::Local: out_ << "local " << expression.slot; break;
        case Expr::Kind::Capture: out_ << "capture " << expression.slot; break;
        case Expr::Kind::Field:
            dump_expression(*expression.object);
            out_ << ".field " << expression.field_slot;
            break;
        case Expr::Kind::FieldDynamic:
            dump_expression(*expression.object);
            out_ << ".(" << expression.field_name << ")";
            break;
        case Expr::Kind::EnumVariant:
            out_ << "enum_variant " << expression.variant_id;
            break;
        case Expr::Kind::EnumPayload:
            out_ << "payload[";
            dump_expression(*expression.object);
            out_ << ", " << expression.payload_index << "]";
            break;
        case Expr::Kind::Match:
            dump_match(expression);
            break;
        case Expr::Kind::IntrinsicBinary:
            out_ << "(";
            dump_expression(*expression.lhs);
            out_ << " " << binary_op_name(expression.binary_op) << " ";
            dump_expression(*expression.rhs);
            out_ << ")";
            break;
        case Expr::Kind::IntrinsicUnary:
            out_ << (expression.unary_op == UnaryOp::Neg ? "-" : "!");
            dump_expression(*expression.object);
            break;
        case Expr::Kind::ShortCircuit:
            out_ << "(";
            dump_expression(*expression.lhs);
            out_ << (expression.short_circuit_is_and ? " && " : " || ");
            dump_expression(*expression.rhs);
            out_ << ")";
            break;
        case Expr::Kind::OperatorCall:
            out_ << "operator " << expression.operator_id << "(";
            dump_call_args(expression);
            out_ << ")";
            break;
        case Expr::Kind::Call:
            if (expression.call_target == Expr::CallTarget::HostFn) {
                out_ << "host_fn " << expression.fn_id;
            } else {
                out_ << "fn " << expression.fn_id;
            }
            out_ << "(";
            dump_call_args(expression);
            out_ << ")";
            break;
        case Expr::Kind::CallValue:
            out_ << "call_value(";
            dump_expression(*expression.object);
            out_ << "; ";
            dump_call_args(expression);
            out_ << ")";
            break;
        case Expr::Kind::Raise:
            out_ << "raise " << expression.effect << "::" << expression.operation << "(";
            dump_call_args(expression);
            out_ << ")";
            break;
        case Expr::Kind::EnumNew:
            out_ << "enum_new " << expression.variant_id << "(";
            dump_call_args(expression);
            out_ << ")";
            break;
        case Expr::Kind::Downcast:
            out_ << "downcast " << expression.fn_id << "(";
            dump_expression(*expression.object);
            out_ << ")";
            break;
        case Expr::Kind::BuildString:
            out_ << "build_string(";
            for (std::size_t i = 0; i < expression.parts.size(); ++i) {
                if (i != 0) {
                    out_ << ", ";
                }
                dump_expression(*expression.parts[i]);
            }
            out_ << ")";
            break;
        case Expr::Kind::Closure:
            out_ << "closure fn " << expression.closure_fn
                 << " captures=" << expression.captures.size();
            break;
        case Expr::Kind::Resume:
            out_ << "resume(";
            dump_expression(*expression.continuation);
            out_ << ", ";
            dump_expression(*expression.resume_value);
            out_ << ")";
            break;
        case Expr::Kind::Handle:
            out_ << "handle table " << expression.table_ref
                 << " body_fn " << expression.body_fn;
            break;
        case Expr::Kind::Abort:
            out_ << "abort(";
            dump_call_args(expression);
            out_ << ")";
            break;
        }
    }

    std::ostream& out_;
    int depth_ = 0;
};

} // namespace

void dump(const Program& program, std::ostream& out) {
    Dumper(out).dump(program);
}

} // namespace glscript::hir
