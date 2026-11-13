#include "glscript/dbg_eval.hpp"

#include "glscript/ast.hpp"
#include "glscript/diagnostic.hpp"
#include "glscript/parser.hpp"
#include "glscript/runtime.hpp"

#include <cstdlib>
#include <optional>

namespace glscript {
namespace {

// Failure propagation: every node visitor returns nullopt after reporting
// through the error out-param, so evaluation unwinds without exceptions.
struct Evaluator {
    Runtime& runtime;
    const DebugFrame& frame;
    std::string& error;

    std::optional<Value> fail(const std::string& message) {
        error = message;
        return std::nullopt;
    }

    std::optional<std::int64_t> as_int(const Value& value, const std::string& op) {
        if (value.is_int()) {
            return value.as_int();
        }
        fail(op + ": expected an int, got " + value.type_name());
        return std::nullopt;
    }

    std::optional<double> as_float(const Value& value, const std::string& op) {
        if (value.is_int() || value.is_float()) {
            return as_number(value);
        }
        fail(op + ": expected a number, got " + value.type_name());
        return std::nullopt;
    }

    std::optional<Value> eval(const ast::Expr& expression) {
        // ErrorExpr is the parser's recovery placeholder; the sink already
        // carries the diagnostics.
        if (dynamic_cast<const ast::ErrorExpr*>(&expression) != nullptr) {
            return fail("parse error in expression");
        }
        if (auto* node = dynamic_cast<const ast::IntLiteralExpr*>(&expression)) {
            return Value::integer(std::strtoll(node->text.c_str(), nullptr, 10));
        }
        if (auto* node = dynamic_cast<const ast::FloatLiteralExpr*>(&expression)) {
            return Value::floating(std::strtod(node->text.c_str(), nullptr));
        }
        if (auto* node = dynamic_cast<const ast::BoolLiteralExpr*>(&expression)) {
            return Value::boolean(node->value);
        }
        if (auto* node = dynamic_cast<const ast::StringLiteralExpr*>(&expression)) {
            return runtime.make_string(node->value);
        }
        if (dynamic_cast<const ast::UnitLiteralExpr*>(&expression) != nullptr) {
            return Value::unit();
        }
        if (auto* node = dynamic_cast<const ast::ArrayLiteralExpr*>(&expression)) {
            std::vector<Value> elements;
            elements.reserve(node->elements.size());
            for (const auto& element : node->elements) {
                auto value = eval(*element);
                if (!value) {
                    return std::nullopt;
                }
                elements.push_back(*value);
            }
            return runtime.make_array(std::move(elements));
        }
        if (auto* node = dynamic_cast<const ast::IdentifierExpr*>(&expression)) {
            return resolve_identifier(node->path);
        }
        if (auto* node = dynamic_cast<const ast::UnaryExpr*>(&expression)) {
            return eval_unary(*node);
        }
        if (auto* node = dynamic_cast<const ast::BinaryExpr*>(&expression)) {
            return eval_binary(*node);
        }
        if (auto* node = dynamic_cast<const ast::MemberExpr*>(&expression)) {
            return eval_member(*node);
        }
        if (auto* node = dynamic_cast<const ast::IndexExpr*>(&expression)) {
            return eval_index(*node);
        }
        return fail("unsupported expression kind in evaluate "
                    "(no calls, closures, matches, or effects)");
    }

    std::optional<Value> resolve_identifier(const std::vector<std::string>& path) {
        if (path.size() != 1) {
            return fail("qualified paths are not supported in evaluate");
        }
        const auto& name = path.front();
        for (std::uint32_t slot = 0; slot < frame.local_count; ++slot) {
            if (slot >= frame.fn->local_names.size() ||
                frame.fn->local_names[slot] != name) {
                continue;
            }
            return runtime.debug_stack(frame.base + slot);
        }
        return fail("no local named '" + name +
                    "' in this frame (globals and calls are not supported)");
    }

    std::optional<Value> eval_unary(const ast::UnaryExpr& node) {
        auto operand = eval(*node.operand);
        if (!operand) {
            return std::nullopt;
        }
        if (node.op == "!") {
            return Value::boolean(!truthy(*operand));
        }
        if (node.op == "-") {
            if (operand->is_int()) {
                return Value::integer(-operand->as_int());
            }
            if (auto number = as_float(*operand, "-")) {
                return Value::floating(-*number);
            }
            return std::nullopt;
        }
        return fail("unsupported unary operator '" + node.op + "'");
    }

    std::optional<Value> eval_binary(const ast::BinaryExpr& node) {
        // Short-circuit forms read their operands lazily.
        if (node.op == "&&" || node.op == "||") {
            auto left = eval(*node.left);
            if (!left) {
                return std::nullopt;
            }
            if (node.op == "&&" && !truthy(*left)) {
                return Value::boolean(false);
            }
            if (node.op == "||" && truthy(*left)) {
                return Value::boolean(true);
            }
            auto right = eval(*node.right);
            if (!right) {
                return std::nullopt;
            }
            return Value::boolean(truthy(*right));
        }

        auto left = eval(*node.left);
        if (!left) {
            return std::nullopt;
        }
        auto right = eval(*node.right);
        if (!right) {
            return std::nullopt;
        }

        const std::string& op = node.op;
        if (op == "==") {
            return Value::boolean(scalar_equal(*left, *right));
        }
        if (op == "!=") {
            return Value::boolean(!scalar_equal(*left, *right));
        }

        if (left->is_heap() && left->as_heap()->kind == GcObject::Kind::String) {
            if (op == "+" && right->is_heap() &&
                right->as_heap()->kind == GcObject::Kind::String) {
                return runtime.make_string(as_string(*left)->value +
                                           as_string(*right)->value);
            }
            return fail("'" + op + "': unsupported for strings");
        }

        if (left->is_int() && right->is_int()) {
            return eval_int(op, left->as_int(), right->as_int());
        }

        auto left_number = as_float(*left, op);
        if (!left_number) {
            return std::nullopt;
        }
        auto right_number = as_float(*right, op);
        if (!right_number) {
            return std::nullopt;
        }
        return eval_float(op, *left_number, *right_number);
    }

    std::optional<Value> eval_int(const std::string& op, std::int64_t left,
                                  std::int64_t right) {
        if (op == "+") return Value::integer(left + right);
        if (op == "-") return Value::integer(left - right);
        if (op == "*") return Value::integer(left * right);
        if (op == "/") {
            if (right == 0) {
                return fail("/: division by zero");
            }
            return Value::integer(left / right);
        }
        if (op == "%") {
            if (right == 0) {
                return fail("%: division by zero");
            }
            return Value::integer(left % right);
        }
        if (op == "<") return Value::boolean(left < right);
        if (op == "<=") return Value::boolean(left <= right);
        if (op == ">") return Value::boolean(left > right);
        if (op == ">=") return Value::boolean(left >= right);
        return fail("unsupported operator '" + op + "'");
    }

    std::optional<Value> eval_float(const std::string& op, double left, double right) {
        if (op == "+") return Value::floating(left + right);
        if (op == "-") return Value::floating(left - right);
        if (op == "*") return Value::floating(left * right);
        if (op == "/") return Value::floating(left / right);
        if (op == "<") return Value::boolean(left < right);
        if (op == "<=") return Value::boolean(left <= right);
        if (op == ">") return Value::boolean(left > right);
        if (op == ">=") return Value::boolean(left >= right);
        return fail("unsupported operator '" + op + "'");
    }

    std::optional<Value> eval_member(const ast::MemberExpr& node) {
        auto object = eval(*node.object);
        if (!object) {
            return std::nullopt;
        }
        if (object->is_heap() && object->as_heap()->kind == GcObject::Kind::Object) {
            return runtime.field(*object, node.name);
        }
        return fail("field access '" + node.name + "' on " + object->type_name() +
                    " (only object fields are supported)");
    }

    std::optional<Value> eval_index(const ast::IndexExpr& node) {
        auto object = eval(*node.object);
        if (!object) {
            return std::nullopt;
        }
        auto index = eval(*node.index);
        if (!index) {
            return std::nullopt;
        }
        auto position = as_int(*index, "index");
        if (!position) {
            return std::nullopt;
        }
        if (!object->is_heap() || object->as_heap()->kind != GcObject::Kind::Array) {
            return fail("cannot index " + object->type_name());
        }
        const auto elements = as_array(*object)->elements;
        if (*position < 0 || static_cast<std::size_t>(*position) >= elements.size()) {
            return fail("index " + std::to_string(*position) + " out of bounds (size " +
                        std::to_string(elements.size()) + ")");
        }
        return elements[static_cast<std::size_t>(*position)];
    }

    static bool truthy(const Value& value) {
        return value.is_bool() && value.as_bool();
    }

    static bool scalar_equal(const Value& left, const Value& right) {
        if (left.data.index() != right.data.index()) {
            return false;
        }
        if (left.is_heap()) {
            return left.as_heap() == right.as_heap();
        }
        return left.data == right.data;
    }
};

} // namespace

Value evaluate_expression(Runtime& runtime, const DebugFrame& frame,
                          const std::string& expression, std::string& error) {
    const SourceFile file("<debug>", expression);
    DiagnosticSink diagnostics;
    Parser parser(file, diagnostics);
    auto parsed = parser.parse_expression_only();
    if (diagnostics.error_count() > 0) {
        error = diagnostics.format();
        return Value::unit();
    }

    Evaluator evaluator{runtime, frame, error};
    auto value = evaluator.eval(*parsed);
    if (!value) {
        return Value::unit();
    }
    error.clear();
    return *value;
}

} // namespace glscript
