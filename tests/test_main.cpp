#include "glscript/checker.hpp"
#include "glscript/bytecode.hpp"
#include "glscript/diagnostic.hpp"
#include "glscript/disasm.hpp"
#include "glscript/host.hpp"
#include "glscript/runtime.hpp"

#include "engine_bindings.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
int passes = 0;

void fail(const std::string& name, const std::string& message) {
    ++failures;
    std::cerr << "FAIL: " << name << "\n  " << message << "\n";
}

void pass(const std::string& name) {
    ++passes;
    std::cout << "ok: " << name << "\n";
}

struct CheckResult {
    bool ok = false;
    std::string diagnostics;
    std::size_t errors = 0;
};

CheckResult check_source(const std::string& name, const std::string& source,
                         glscript::ModuleLoader loader = {}) {
    glscript::HostRegistry host;
    glscript::register_engine_bindings(host);

    glscript::DiagnosticSink diagnostics;
    glscript::Frontend frontend(host, diagnostics, std::move(loader));

    CheckResult result;
    result.ok = frontend.check_source(name, name + ".gl", source);
    result.diagnostics = diagnostics.format();
    result.errors = diagnostics.error_count();
    return result;
}

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void expect_ok(const std::string& name, const std::string& source,
               glscript::ModuleLoader loader = {}) {
    const auto result = check_source(name, source, std::move(loader));
    if (!result.ok || result.errors != 0) {
        fail(name, result.diagnostics.empty() ? "typecheck failed" : result.diagnostics);
        return;
    }
    pass(name);
}

void expect_error(const std::string& name, const std::string& source,
                  const std::string& expected_substring,
                  glscript::ModuleLoader loader = {}) {
    const auto result = check_source(name, source, std::move(loader));
    if (result.ok || result.errors == 0) {
        fail(name, "expected a typecheck error, but the source was accepted");
        return;
    }
    if (!expected_substring.empty() &&
        result.diagnostics.find(expected_substring) == std::string::npos) {
        fail(name, "diagnostics did not contain '" + expected_substring + "':\n" +
                       result.diagnostics);
        return;
    }
    pass(name);
}

// ---------------------------------------------------------------------------
// Runtime tests: typecheck, compile, execute, capture println output
// ---------------------------------------------------------------------------

struct RunResult {
    bool ran = false;
    std::string diagnostics;
    std::string output;
};

std::string trim(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

RunResult run_source(const std::string& name, const std::string& source,
                     glscript::ModuleLoader loader = {}) {
    glscript::HostRegistry host;
    RunResult result;
    glscript::register_engine_bindings(host, [&](const std::string& line) {
        result.output += line;
        result.output += "\n";
    });

    glscript::DiagnosticSink diagnostics;
    glscript::Frontend frontend(host, diagnostics, std::move(loader));
    if (!frontend.check_source(name, name + ".gl", source)) {
        result.diagnostics = diagnostics.format();
        return result;
    }

    const auto& program = frontend.program();
    auto bytecode = glscript::compile_program(program);
    glscript::Runtime runtime(program, bytecode, host, diagnostics);
    runtime.run(name, "main");

    result.diagnostics = diagnostics.format();
    result.ran = !diagnostics.has_errors();
    return result;
}

void expect_run(const std::string& name, const std::string& source,
                const std::string& expected_output,
                glscript::ModuleLoader loader = {}) {
    const auto result = run_source(name, source, std::move(loader));
    if (!result.ran) {
        fail(name, result.diagnostics.empty() ? "run failed" : result.diagnostics);
        return;
    }
    if (trim(result.output) != trim(expected_output)) {
        fail(name, "output mismatch:\n  expected: [" + trim(expected_output) +
                       "]\n  actual:   [" + trim(result.output) + "]");
        return;
    }
    pass(name);
}

void expect_run_error(const std::string& name, const std::string& source,
                      const std::string& expected_substring) {
    const auto result = run_source(name, source);
    if (result.ran) {
        fail(name, "expected a runtime error, but the script ran cleanly");
        return;
    }
    if (!expected_substring.empty() &&
        result.diagnostics.find(expected_substring) == std::string::npos) {
        fail(name, "diagnostics did not contain '" + expected_substring + "':\n" +
                       result.diagnostics);
        return;
    }
    pass(name);
}

const char* kEntitySource = R"GL(
import input::{Input, Axis};
import math::Vector3;

export class PlayerMovement extends EntityBehaviour {
    #[serialize]
    float move_speed = 5.0f;

    export function update() {
        println("inside PlayerMovement");
        Runtime::println("also inside PlayerMovement");
        float move_x = Input::get_axis(Axis::Horizontal);
        float move_z = Input::get_axis(Axis::Vertical);
        Vector3<float> move_direction = Vector3::new(move_x, 0.0f, move_z);

        get_object().get_component<Transform>().translate(move_direction * move_speed * dt());
        dynamic test = 100;

        int d_test = downcast::<int> test;
        String d_test_fail = downcast::<String> test;
    }
}
)GL";

const char* kRuntimeSource = R"GL(
effect RuntimeError {
    #[abort]
    function not_defined(name: String, span: Span);
}

handle RuntimeError {
    function not_defined(name: String, span: Span) {
        println(`Name ${name} not found!`);
        abort();
    }
}
)GL";

void test_entity_sketch() {
    expect_ok("entity.gl sketch", kEntitySource);
}

void test_runtime_sketch() {
    expect_ok("rt.gl sketch", kRuntimeSource);
}

void test_unknown_identifier() {
    expect_error("unknown identifier",
                 "function update() { println(missing_name); }\n",
                 "unknown identifier 'missing_name'");
}

void test_wrong_argument_type() {
    expect_error("wrong argument type",
                 "function update() { println(42); }\n",
                 "argument 1 of 'println' expects String, got int");
}

void test_downcast() {
    expect_ok("downcast from dynamic",
              "function update() {\n"
              "  dynamic value = 10;\n"
              "  int number = downcast::<int> value;\n"
              "  String text = downcast::<String> value;\n"
              "}\n");
}

void test_generic_inference() {
    expect_ok("generic method inference",
              "import math::Vector3;\n"
              "function update() {\n"
              "  Vector3<float> v = Vector3::new(1.0f, 2.0f, 3.0f);\n"
              "  get_object().get_component<Transform>().translate(v * 2.0f);\n"
              "}\n");
}

void test_effect_handler() {
    expect_ok("effect handler resume",
              "effect Failure {\n"
              "  function fail(message: String) -> int;\n"
              "}\n"
              "function safe() {\n"
              "  handle (1 + 2) with {\n"
              "    Failure::fail(message: String) -> int { resume 0 }\n"
              "  };\n"
              "}\n");
}

void test_multi_shot_handler() {
    expect_ok("multi-shot handler typechecks",
              "effect Ask { function ask() -> int; }\n"
              "function run() -> int {\n"
              "  return handle (1) with {\n"
              "    Ask::ask() -> int { resume 1; resume 2; }\n"
              "  };\n"
              "}\n");
}

void test_generic_class_method() {
    expect_ok("generic class method explicit type argument",
              "class Box<T> {\n"
              "  T value;\n"
              "  function map<U>(other: U) -> U { return other; }\n"
              "}\n"
              "function update() {\n"
              "  dynamic raw = 0;\n"
              "  Box<int> box = downcast::<Box<int>> raw;\n"
              "  String text = box.map<String>(\"hello\");\n"
              "}\n");
}

void test_enum_payload() {
    expect_ok("generic enum payload",
              "enum Expr<C> {\n"
              "  Const(C),\n"
              "  Add(Expr<C>, Expr<C>)\n"
              "}\n"
              "function update() {\n"
              "  Expr<int> value = Expr::Const(1);\n"
              "}\n");
}

void test_missing_resume_type() {
    expect_error("resume value type",
                 "effect Failure { function fail() -> int; }\n"
                 "function safe() {\n"
                 "  handle (1) with {\n"
                 "    Failure::fail() -> int { resume \"wrong\" }\n"
                 "  };\n"
                 "}\n",
                 "resume value expects int, got String");
}

void test_fn_is_rejected() {
    expect_error("fn is rejected", "fn update() {}\n", "'fn' is not supported");
}

void test_import_visibility() {
    auto loader = [](const std::string& module_name) -> std::optional<glscript::ModuleSource> {
        if (module_name == "private_mod") {
            return glscript::ModuleSource{
                "private_mod", "private_mod.gl",
                "export class Hidden {}\n"
                "pub class Visible {}\n"
                "pub export class Both {}\n"
                "export function hidden_function() {}\n"
                "pub function visible_function() {}\n"
                "pub export function both_function() {}\n"};
        }
        return std::nullopt;
    };

    expect_ok("pub import succeeds",
              "import private_mod::{Visible, visible_function};\n"
              "function update() { visible_function(); }\n",
              loader);

    expect_ok("pub export import succeeds",
              "import private_mod::{Both, both_function};\n"
              "function update() {\n"
              "  dynamic raw = 0;\n"
              "  Both value = downcast::<Both> raw;\n"
              "  both_function();\n"
              "}\n",
              loader);

    expect_error("export import fails",
                 "import private_mod::{Hidden};\n",
                 "is not importable", loader);

    expect_error("local import fails",
                 "import private_mod::{hidden_function};\n",
                 "", loader);
}

void test_multi_level_inheritance() {
    expect_ok("multi-level inheritance members",
              "class Grandparent {\n"
              "  int base_value = 1;\n"
              "  function describe() -> int { return base_value; }\n"
              "}\n"
              "class Parent extends Grandparent { int parent_value = 2; }\n"
              "class Child extends Parent { int child_value = 3; }\n"
              "function update() {\n"
              "  dynamic raw = 0;\n"
              "  Child child = downcast::<Child> raw;\n"
              "  int a = child.base_value;\n"
              "  child.parent_value = 4;\n"
              "  child.base_value = child.describe();\n"
              "}\n");
}

void test_source_module_import() {
    auto loader = [](const std::string& module_name) -> std::optional<glscript::ModuleSource> {
        if (module_name == "helpers") {
            return glscript::ModuleSource{
                "helpers", "helpers.gl",
                "pub function twice(value: float) -> float { return value * 2.0f; }\n"};
        }
        return std::nullopt;
    };

    expect_ok("source module import",
              "import helpers::twice;\n"
              "function update() { float result = twice(2.0f); }\n",
              loader);
}

void test_control_flow() {
    expect_ok("control flow",
              "function update() {\n"
              "  int total = 0;\n"
              "  if (total == 0) { total = 1; }\n"
              "  while (total < 10) { total = total + 1; if (total == 5) { break; } }\n"
              "}\n");
}

void test_assignment_type_error() {
    expect_error("assignment type error",
                 "function update() { int value = 0; value = \"text\"; }\n",
                 "cannot assign value of type String to int");
}

void test_sketch_files_on_disk() {
    const std::filesystem::path root(GLSCRIPT_SOURCE_DIR);
    const auto entity = read_file((root / "entity.gl").string());
    const auto runtime = read_file((root / "rt.gl").string());

    if (entity.empty()) {
        fail("entity.gl on disk", "could not read entity.gl");
    } else {
        expect_ok("entity.gl on disk", entity);
    }

    if (runtime.empty()) {
        fail("rt.gl on disk", "could not read rt.gl");
    } else {
        expect_ok("rt.gl on disk", runtime);
    }
}


// ---------------------------------------------------------------------------
// Runtime tests
// ---------------------------------------------------------------------------

void test_run_arithmetic() {
    expect_run("run_arithmetic", R"GL(
function main() {
    println(`sum=${1 + 2} float=${1.5f + 2.5f} mix=${1 + 2 * 3}`);
    println(`div=${10 / 4} fdiv=${10.0f / 4.0f} mod=${17 % 5}`);
    println(`neg=${-(3 + 4)}`);
}
)GL",
              "sum=3 float=4 mix=7\ndiv=2 fdiv=2.5 mod=2\nneg=-7");
}

void test_run_strings() {
    expect_run("run_strings", R"GL(
function main() {
    String greeting = "he" + "llo";
    int count = 2;
    println(greeting + ` x${count}`);
}
)GL",
              "hello x2");
}

void test_run_control_flow() {
    expect_run("run_control_flow", R"GL(
function main() {
    int total = 0;
    int i = 0;
    while (i < 10) {
        i = i + 1;
        if (i == 3) {
            continue;
        }
        if (i > 7) {
            break;
        }
        total = total + i;
    }
    println(`total=${total}`);
    if (total == 25) {
        println("odd sum correct");
    } else {
        println("unexpected");
    }
}
)GL",
              "total=25\nodd sum correct");
}

void test_run_recursion() {
    expect_run("run_recursion", R"GL(
function fib(n: int) -> int {
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

function main() {
    println(`fib(10)=${fib(10)}`);
}
)GL",
              "fib(10)=55");
}

void test_run_closures() {
    expect_run("run_closures", R"GL(
function adder(offset: int) -> int {
    return ((x: int) => x + offset)(5);
}

function nested() -> int {
    int outer = 10;
    return ((y: int) => ((x: int) => x + y + outer)(2))(1);
}

function main() {
    println(`adder=${adder(3)}`);
    println(`nested=${nested()}`);
}
)GL",
              "adder=8\nnested=13");
}

void test_run_enums() {
    expect_run("run_enums", R"GL(
enum Color {
    Red,
    Green,
}

enum Expr {
    Const(int),
    Pair(int, int),
}

function main() {
    Color c = Color::Green;
    if (c == Color::Green) {
        println("green");
    }
    Expr e = Expr::Const(7);
    println(`${e}`);
    Expr p = Expr::Pair(1, 2);
    if (e == Expr::Const(7)) {
        println("const match");
    }
    println(`${p}`);
}
)GL",
              "green\nConst(7)\nconst match\nPair(1, 2)");
}

void test_run_dynamic_downcast() {
    expect_run("run_dynamic_downcast", R"GL(
function main() {
    dynamic value = 100;
    int number = downcast::<int> value;
    dynamic text = "hello";
    String message = downcast::<String> text;
    println(`n=${number} s=${message}`);
}
)GL",
              "n=100 s=hello");
}

void test_run_downcast_error() {
    expect_run_error("run_downcast_error", R"GL(
function main() {
    dynamic value = 100;
    String text = downcast::<String> value;
    println(text);
}
)GL",
                     "invalid_downcast");
}

void test_run_handler() {
    expect_run("run_handler", R"GL(
effect Counter {
    function count() -> int;
}

function work(total: int) -> int {
    return total + Counter::count() + Counter::count();
}

function main() {
    int result = handle (work(10)) with {
        Counter::count() -> int {
            resume(5);
        }
    };
    println(`result=${result}`);
}
)GL",
              "result=20");
}

void test_run_multi_shot() {
    // The same continuation is resumed twice by one handler activation:
    // the body re-runs from the raise point both times.
    expect_run("run_multi_shot", R"GL(
effect Counter {
    function count() -> int;
}

function main() {
    int result = handle (Counter::count() + Counter::count()) with {
        Counter::count() -> int {
            resume(1) + resume(2);
        }
    };
    println(`result=${result}`);
}
)GL",
              "result=4");
}

void test_run_nested_handlers() {
    expect_run("run_nested_handlers", R"GL(
effect A {
    function a() -> int;
}
effect B {
    function b() -> int;
}

function inner() -> int {
    return A::a() * 10 + B::b();
}

function main() {
    int result = handle (handle (inner()) with {
        B::b() -> int {
            resume(3);
        }
    }) with {
        A::a() -> int {
            resume(4);
        }
    };
    println(`result=${result}`);
}
)GL",
              "result=43");
}

void test_run_ambient_handler() {
    // A top-level `handle Effect { ... }` is installed for the whole script;
    // a non-resuming case's value becomes the operation result.
    expect_run("run_ambient_handler", R"GL(
effect Noise {
    function ping() -> int;
}

handle Noise {
    function ping() -> int {
        42;
    }
}

function main() {
    println(`ping=${Noise::ping()}`);
}
)GL",
              "ping=42");
}

void test_run_unhandled_op() {
    expect_run_error("run_unhandled_op", R"GL(
effect Counter {
    function count() -> int;
}

function main() {
    int x = Counter::count();
    println(`${x}`);
}
)GL",
                     "unhandled operation");
}

void test_run_handler_decl_evaluates_body() {
    expect_run("run_handler_decl_body", R"GL(
effect Counter {
    function count() -> int;
}

handle (Counter::count() + Counter::count()) with {
    Counter::count() -> int {
        resume(10);
    }
}

function main() {
    println("main ran");
}
)GL",
              "main ran");
}

void test_run_host_calls() {
    expect_run("run_host_calls", R"GL(
import input::{Input, Axis};
import math::Vector3;

function main() {
    float axis = Input::get_axis(Axis::Horizontal);
    Vector3<float> v = Vector3::new(1.0f, 2.0f, 3.0f);
    Vector3<float> scaled = v * 2.0f;
    Vector3<float> shifted = v + Vector3::new(10.0f, 20.0f, 30.0f);
    println(`axis=${axis}`);
    println(`scaled=${scaled.x} ${scaled.y} ${scaled.z}`);
    println(`shifted=${shifted.x} ${shifted.y} ${shifted.z}`);
    println(`dt=${dt()}`);
    println(`script=${get_script_name()}`);
}
)GL",
              "axis=1\nscaled=2 4 6\nshifted=11 22 33\ndt=0.016\nscript=script");
}

// ---------------------------------------------------------------------------
// Pattern matching: match expressions and let destructuring
// ---------------------------------------------------------------------------

void test_match_enums() {
    expect_run("match enums", R"GL(
enum Shape {
    Circle(float),
    Rect(float, float),
    Point,
}

function area(s: Shape) -> float {
    return match s {
        Shape::Circle(r) => 3.14 * r * r,
        Shape::Rect(w, h) => { let p = w + h; p * 2.0 },
        Shape::Point => 0.0,
    };
}

function describe(s: Shape) -> String {
    return match s {
        Point => "point",
        Shape::Circle(_) => "circle",
        Shape::Rect(_, _) => "rect",
    };
}

function main() {
    println(`circle: ${area(Shape::Circle(2.0))}`);
    println(`rect: ${area(Shape::Rect(3.0, 4.0))}`);
    println(`point: ${area(Shape::Point)}`);
    println(describe(Shape::Point));
    println(describe(Shape::Circle(1.0)));
    println(describe(Shape::Rect(1.0, 2.0)));
}
)GL",
              "circle: 12.56\nrect: 14\npoint: 0\npoint\ncircle\nrect");
}

void test_match_literals_and_guards() {
    expect_run("match literals and guards", R"GL(
function classify(value: int) -> String {
    return match value {
        0 => "zero",
        -1 => "minus one",
        n if n < 0 => "negative",
        n if n < 10 => "small",
        _ => "large",
    };
}

function flag(b: bool) -> String {
    return match b {
        true => "yes",
        false => "no",
    };
}

function greet(name: String) -> String {
    return match name {
        "world" => "hello world",
        other => `hi ${other}`,
    };
}

function unit_value(u: unit) -> String {
    return match u {
        () => "unit",
    };
}

function main() {
    println(classify(0));
    println(classify(-1));
    println(classify(-5));
    println(classify(3));
    println(classify(42));
    println(flag(true));
    println(flag(false));
    println(greet("world"));
    println(greet("glscript"));
    println(unit_value(()));
}
)GL",
              "zero\nminus one\nnegative\nsmall\nlarge\nyes\nno\nhello world\nhi glscript\nunit");
}

void test_match_nested_and_closures() {
    expect_run("match nested patterns", R"GL(
enum Inner {
    Inner(int),
    Empty,
}

enum Outer {
    Outer(Inner, int),
    Nothing,
}

function deep(o: Outer) -> int {
    return match o {
        Outer::Outer(Inner::Inner(v), tag) => v * 100 + tag,
        Outer::Outer(Inner::Empty, _) => -1,
        Nothing => 0,
    };
}

function main() {
    println(`deep1: ${deep(Outer::Outer(Inner::Inner(3), 7))}`);
    println(`deep2: ${deep(Outer::Outer(Inner::Empty, 9))}`);
    println(`deep3: ${deep(Outer::Nothing)}`);

    // Lambdas inside arms capture the pattern bindings.
    let result = match Outer::Outer(Inner::Inner(4), 1) {
        Outer::Outer(Inner::Inner(v), tag) => { let f = () => v + tag; f() },
        _ => -1,
    };
    println(`closure arm: ${result}`);
}
)GL",
              "deep1: 307\ndeep2: -1\ndeep3: 0\nclosure arm: 5");
}

void test_match_tail_and_result_value() {
    expect_run("match as value", R"GL(
enum Op {
    Add,
    Mul,
}

function apply(op: Op, a: int, b: int) -> int {
    let result = match op {
        Op::Add => a + b,
        Op::Mul => a * b,
    };
    return result;
}

function main() {
    println(`add: ${apply(Op::Add, 2, 3)}`);
    println(`mul: ${apply(Op::Mul, 2, 3)}`);

    // match nested inside a larger expression.
    println(`nested: ${match Op::Mul { Op::Add => 10, Op::Mul => 20 }}`);

    int doubled = match 21 {
        x => x * 2,
    };
    println(`doubled: ${doubled}`);
}
)GL",
              "add: 5\nmul: 6\nnested: 20\ndoubled: 42");
}

void test_let_destructuring() {
    expect_run("let destructuring", R"GL(
enum Inner {
    Inner(int, int),
}

enum Slot {
    Slot(Inner, int),
}

function main() {
    let Slot::Slot(inner, z) = Slot::Slot(Inner::Inner(4, 9), 100);
    let Inner::Inner(a, b) = inner;
    println(`a=${a} b=${b} z=${z} sum=${a + b + z}`);

    // Nested destructuring in one pattern.
    let Slot::Slot(Inner::Inner(x, y), w) = Slot::Slot(Inner::Inner(1, 2), 3);
    println(`x=${x} y=${y} w=${w}`);

    let n: int = 5;
    println(`n=${n}`);

    // Binding with annotation destructuring a payload.
    let Slot::Slot(p: Inner, q) = Slot::Slot(Inner::Inner(7, 8), 9);
    let Inner::Inner(p0, p1) = p;
    println(`p0=${p0} p1=${p1} q=${q}`);
}
)GL",
              "a=4 b=9 z=100 sum=113\nx=1 y=2 w=3\nn=5\np0=7 p1=8 q=9");
}

void test_match_type_errors() {
    expect_error("match missing variant", R"GL(
enum Color {
    Red,
    Green,
    Blue,
}

function main() {
    let c = Color::Green;
    let label = match c {
        Color::Red => "r",
        Color::Green => "g",
    };
    println(label);
}
)GL",
                  "non-exhaustive match: missing variant");

    expect_error("match non-exhaustive int", R"GL(
function main() {
    let label = match 5 {
        1 => "one",
        2 => "two",
    };
    println(label);
}
)GL",
                  "non-exhaustive match");

    expect_error("let refutable pattern", R"GL(
enum Maybe {
    Some(int),
    None,
}

function main() {
    let Maybe::Some(v) = Maybe::Some(3);
    println(v);
}
)GL",
                  "refutable pattern in 'let'");

    expect_error("let refutable literal", R"GL(
function main() {
    let 5 = 5;
    println("unreachable");
}
)GL",
                  "refutable pattern in 'let'");

    expect_error("variant payload arity", R"GL(
enum Pair {
    Pair(int, int),
}

function main() {
    let Pair::Pair(a) = Pair::Pair(1, 2);
    println(a);
}
)GL",
                  "expects 2 payload pattern(s), got 1");

    expect_error("literal pattern on enum", R"GL(
enum Color {
    Red,
}

function main() {
    let c = Color::Red;
    match c {
        1 => println("no"),
        _ => println("ok"),
    }
}
)GL",
                  "literal pattern cannot match type");

    expect_error("guarded arm not exhaustive", R"GL(
enum Color {
    Red,
    Green,
    Blue,
}

function main() {
    let c = Color::Red;
    let label = match c {
        Color::Red if c == Color::Green => "r?",
        Color::Green => "g",
        Color::Blue => "b",
    };
    println(label);
}
)GL",
                  "non-exhaustive match: missing variant");

    expect_error("match guard not bool", R"GL(
function main() {
    match 5 {
        n if n + 1 => println("bad"),
        _ => println("ok"),
    }
}
)GL",
                  "match guard must be bool");

    expect_error("match arm type mismatch", R"GL(
function main() {
    let v = match 5 {
        1 => "one",
        _ => 2,
    };
    println(v);
}
)GL",
                  "match arms have incompatible types");

    expect_error("break inside arm block", R"GL(
function main() {
    while (true) {
        match 5 {
            _ => { break; },
        }
    }
}
)GL",
                  "'break' inside a match arm block");

    expect_error("return inside arm block", R"GL(
function main() {
    match 5 {
        _ => { return 1; },
    }
}
)GL",
                  "'return' inside a match arm block");
}

void test_match_dynamic_scrutinee() {
    expect_run("match dynamic", R"GL(
function main() {
    dynamic value = 10;
    let label = match value {
        10 => "ten",
        s: String => `text ${s}`,
        _ => "other",
    };
    println(label);

    dynamic text = "abc";
    println(match text {
        s: String => `len ${s}`,
        _ => "not text",
    });
}
)GL",
              "ten\nlen abc");
}

void test_match_effects_and_captures() {
    expect_run("match inside handlers and captures", R"GL(
effect Ask {
    function ask(n: int) -> int;
}

enum Op {
    Add,
    Mul,
}

function compute(op: Op, x: int, y: int) -> int {
    return match op {
        Op::Add => x + y,
        Op::Mul => handle Ask::ask(x) with {
            function Ask::ask(n: int) -> int {
                // resume transfers: the parenthesized value is delivered to
                // the raise site and the delimiter receives the
                // continuation's result (code after a resume does not run).
                return match n {
                    0 => resume 0,
                    k => resume (k * 2),
                };
            }
        },
    };
}

function main() {
    println(`compute: ${compute(Op::Add, 2, 5)} ${compute(Op::Mul, 0, 7)} ${compute(Op::Mul, 3, 7)}`);
    println(`shadowing: ${match 2 { n => { let n = n * 10; n }, _ => 0 }}`);
}
)GL",
              "compute: 7 0 6\nshadowing: 20");
}

// ---------------------------------------------------------------------------
// Arrays: array<T>, effectful indexing, Option<T>.get
// ---------------------------------------------------------------------------

void test_array_basics() {
    expect_run("array basics", R"GL(
function main() {
    let numbers = [10, 20, 30];
    println(`arr=${numbers} len=${numbers.len()}`);
    println(`first=${numbers[0]} last=${numbers[2]}`);

    numbers[1] = 99;
    println(`mutated=${numbers}`);

    let stack: array<int> = [];
    stack.push(5);
    stack.push(6);
    println(`stack=${stack} len=${stack.len()}`);

    let words = ["alpha", "beta"];
    println(`word=${words[1]}`);
}
)GL",
              "arr=[10, 20, 30] len=3\nfirst=10 last=30\nmutated=[10, 99, 30]\n"
              "stack=[5, 6] len=2\nword=beta");
}

void test_array_dynamic_and_nested() {
    expect_run("array dynamic and nested", R"GL(
function main() {
    let mixed: array<dynamic> = [1, "two", true, [3, 4]];
    println(`len=${mixed.len()} mid=${mixed[1]} flag=${mixed[2]}`);
    println(`nested=${mixed[3]}`);

    let wide = [1, 2.5, "three"];
    println(`wide=${wide} wide1=${wide[1]}`);

    let typed: array<dynamic> = [7, 8];
    println(`coerced=${typed}`);
}
)GL",
              "len=4 mid=two flag=true\nnested=[3, 4]\n"
              "wide=[1, 2.5, three] wide1=2.5\ncoerced=[7, 8]");
}

void test_array_get_option() {
    expect_run("array get Option", R"GL(
function describe(opt: Option<int>) -> String {
    return match opt {
        Option::Some(v) => `some ${v}`,
        Option::None => "none",
    };
}

function main() {
    let a = [10, 20, 30];
    println(describe(a.get(0)));
    println(describe(a.get(2)));
    println(describe(a.get(3)));
    println(describe(a.get(-1)));

    let empty: array<int> = [];
    println(describe(empty.get(0)));

    let words = ["x", "y"];
    let w = words.get(1);
    match w {
        Option::Some(text) => println(`got ${text}`),
        Option::None => println("missing"),
    }
}
)GL",
              "some 10\nsome 30\nnone\nnone\nnone\ngot y");
}

void test_array_bounds_effect() {
    expect_run("array bounds effect", R"GL(
function with_fallback(a: array<int>) -> int {
    return handle a[10] with {
        function OutOfBounds::index(size: int, got: int) -> int {
            return 0;
        }
    };
}

function clamped(a: array<int>, i: int) -> int {
    return handle a[i] with {
        function OutOfBounds::index(size: int, got: int) -> int {
            // resume substitutes the element value (parenthesized so the
            // arithmetic is part of the resumed value, not dead code).
            return resume (size - 1);
        }
    };
}

function main() {
    let a = [7, 8, 9];
    println(`fallback=${with_fallback(a)}`);
    println(`clamped=${clamped(a, 99)}`);
    println(`direct=${clamped(a, 1)}`);
}
)GL",
              "fallback=0\nclamped=2\ndirect=8");
}

void test_array_unhandled_bounds() {
    expect_run_error("array unhandled bounds", R"GL(
function main() {
    let a = [1, 2, 3];
    let boom = a[42];
    println(`never ${boom}`);
}
)GL",
              "aborted by host operation 'index'");
}

void test_array_type_errors() {
    expect_error("index non-array", R"GL(
function main() {
    let n = 5;
    println(n[0]);
}
)GL",
                  "indexing is not supported for type int");

    expect_error("float index", R"GL(
function main() {
    let a = [1, 2];
    println(a[1.5]);
}
)GL",
                  "array index must be int");

    expect_error("array element type mismatch", R"GL(
function main() {
    let a: array<int> = [1, 2];
    let b: array<String> = a;
    println(b);
}
)GL",
                  "cannot initialize");

    expect_error("push wrong element type", R"GL(
function main() {
    let a = [1, 2];
    a.push("nope");
    println(a);
}
)GL",
                  "argument 1 of 'push'");

    expect_error("bare array type", R"GL(
function main() {
    let a: array = [];
    println(a);
}
)GL",
                  "expects 1 type argument(s)");
}

void test_handler_case_capture_rejected() {
    // Handler cases run as plain functions with no closure object, so a
    // reference to an outer variable would crash the interpreter (previously
    // silently compiled and aborted at runtime).
    expect_error("handler case capture rejected", R"GL(
effect Ask {
    function ask(n: int) -> int;
}

function f(x: int, y: int) -> int {
    return handle Ask::ask(x) with {
        function Ask::ask(n: int) -> int {
            return resume n * y;
        }
    };
}
)GL",
                  "handler cases cannot reference outer variables");
}

void test_run_string_allocation_gc() {    // Enough allocations to trigger several collections.
    expect_run("run_gc", R"GL(
function main() {
    int i = 0;
    while (i < 3000) {
        String temp = `row ${i} padding ${i * 2}`;
        i = i + 1;
    }
    println("survived 3000 allocations");
}
)GL",
              "survived 3000 allocations");
}

} // namespace

void test_embed_behaviour_lifecycle() {
    // Unity-style embedding: the host constructs a behaviour instance, drives
    // its lifecycle through call_method, and reads/writes its fields.
    glscript::HostRegistry host;
    glscript::register_engine_bindings(host);
    glscript::DiagnosticSink diagnostics;
    glscript::Frontend frontend(host, diagnostics);

    const bool ok = frontend.check_source("embed", "embed.gl", R"GL(
class Counter extends EntityBehaviour {
    int count = 0;
    float factor = 2.0f;

    function on_start() {
        count = 10;
    }

    function tick(n: int) -> int {
        count = count + n;
        return count;
    }

    function scaled() -> float {
        return count * factor;
    }
}
)GL");
    if (!ok) {
        fail("embed_behaviour_lifecycle", diagnostics.format());
        return;
    }

    auto bytecode = glscript::compile_program(frontend.program());
    glscript::Runtime runtime(frontend.program(), bytecode, host, diagnostics);

    auto* module = frontend.program().find_module("embed");
    auto* type = module != nullptr ? module->types.at("Counter") : nullptr;
    if (type == nullptr) {
        fail("embed_behaviour_lifecycle", "script class Counter not found");
        return;
    }

    // The host is the "prefab": it supplies the initial field values and
    // keeps the instance alive across GC cycles.
    auto instance = runtime.make_object(type, {glscript::Value::integer(0),
                                               glscript::Value::floating(2.0)});
    runtime.retain(instance);

    if (runtime.call_method(instance, "on_start").is_unit() &&
        diagnostics.has_errors()) {
        fail("embed_behaviour_lifecycle", diagnostics.format());
        return;
    }

    auto count = runtime.field(instance, "count");
    if (!count.is_int() || count.as_int() != 10) {
        fail("embed_behaviour_lifecycle", "on_start did not set count to 10");
        return;
    }

    const auto ticked = runtime.call_method(instance, "tick",
                                            {glscript::Value::integer(5)});
    if (!ticked.is_int() || ticked.as_int() != 15) {
        fail("embed_behaviour_lifecycle", "tick returned wrong value");
        return;
    }

    runtime.set_field(instance, "factor", glscript::Value::floating(10.0));
    const auto scaled = runtime.call_method(instance, "scaled");
    if (!scaled.is_float() || scaled.as_float() != 150.0) {
        fail("embed_behaviour_lifecycle", "scaled returned wrong value");
        return;
    }

    // The retained instance must survive several forced collections.
    for (int i = 0; i < 5; ++i) {
        runtime.gc_collect();
    }
    count = runtime.field(instance, "count");
    if (!count.is_int() || count.as_int() != 15) {
        fail("embed_behaviour_lifecycle", "retained instance was collected");
        return;
    }

    pass("embed_behaviour_lifecycle");
}

void test_embed_method_dispatch() {
    // call_method dispatches on the instance's runtime type: two classes
    // with the same method name get their own bodies.
    glscript::HostRegistry host;
    glscript::register_engine_bindings(host);
    glscript::DiagnosticSink diagnostics;
    glscript::Frontend frontend(host, diagnostics);

    const bool ok = frontend.check_source("dispatch", "dispatch.gl", R"GL(
class Base extends EntityBehaviour {
    function describe() -> String {
        return "base";
    }
}

class Derived extends Base {
    function describe() -> String {
        return "derived";
    }
}

class Other extends EntityBehaviour {
    function describe() -> String {
        return "other";
    }
}
)GL");
    if (!ok) {
        fail("embed_method_dispatch", diagnostics.format());
        return;
    }

    auto bytecode = glscript::compile_program(frontend.program());
    glscript::Runtime runtime(frontend.program(), bytecode, host, diagnostics);

    auto* module = frontend.program().find_module("dispatch");
    const auto make = [&](const std::string& class_name) {
        return runtime.make_object(module->types.at(class_name), {});
    };

    const auto derived = make("Derived");
    const auto base = make("Base");
    const auto other = make("Other");
    runtime.retain(derived);
    runtime.retain(base);
    runtime.retain(other);

    const auto describe = [&](const glscript::Value& instance) {
        return runtime.call_method(instance, "describe");
    };

    if (describe(derived).type_name() != "String" ||
        runtime.stringify(describe(derived)) != "derived" ||
        runtime.stringify(describe(base)) != "base" ||
        runtime.stringify(describe(other)) != "other") {
        fail("embed_method_dispatch", "dispatch did not follow the runtime type");
        return;
    }

    pass("embed_method_dispatch");
}

void test_host_module_name_collision() {
    // A source module must not silently share a host module's name: the
    // script's declarations would vanish while checking "succeeds".
    glscript::HostRegistry host;
    glscript::register_engine_bindings(host); // provides host module 'input'
    glscript::DiagnosticSink diagnostics;
    glscript::Frontend frontend(host, diagnostics);
    const bool ok = frontend.check_source("input", "input.gl", R"GL(
function helper() {
    return 1;
}
)GL");
    if (ok) {
        fail("host module name collision",
             "a source module shadowing a host module was accepted");
        return;
    }
    if (diagnostics.format().find("provided by the host") == std::string::npos) {
        fail("host module name collision", diagnostics.format());
        return;
    }
    pass("host module name collision");
}

void test_disassembler() {
    glscript::HostRegistry host;
    glscript::register_engine_bindings(host);
    glscript::DiagnosticSink diagnostics;
    glscript::Frontend frontend(host, diagnostics);
    const bool ok = frontend.check_source("disasm", "disasm.gl", R"GL(
function main() {
    let values = [1, 2, 3, 4];
    let total = 0;
    let index = 0;
    while (index < values.len()) {
        total = total + values[index];
        index = index + 1;
    }
    println(`total=${total}`);
}
)GL");
    if (!ok) {
        fail("disassembler", diagnostics.format());
        return;
    }

    const auto program = glscript::compile_program(frontend.program());
    bool found_raise = false;
    bool found_loop = false;
    std::size_t instruction_count = 0;
    for (const auto& function : program.functions) {
        for (const auto& line : glscript::disassemble(function, program)) {
            ++instruction_count;
            if (line.text.starts_with(
                    "Raise \"engine::Runtime\".\"println\" argc=1")) {
                found_raise = true;
            }
            if (line.text.starts_with("Loop -> ")) {
                found_loop = true;
            }
        }
    }

    if (instruction_count == 0 || !found_raise || !found_loop) {
        fail("disassembler", "missing instructions or decoded operands (raise=" +
                                 std::to_string(found_raise) + " loop=" +
                                 std::to_string(found_loop) + ")");
        return;
    }
    pass("disassembler");
}

int main() {  // NOLINT(readability-function-size)

    test_entity_sketch();
    test_runtime_sketch();
    test_embed_behaviour_lifecycle();
    test_embed_method_dispatch();
    test_host_module_name_collision();
    test_disassembler();
    test_unknown_identifier();
    test_wrong_argument_type();
    test_downcast();
    test_generic_inference();
    test_effect_handler();
    test_multi_shot_handler();
    test_generic_class_method();
    test_enum_payload();
    test_missing_resume_type();
    test_fn_is_rejected();
    test_import_visibility();
    test_multi_level_inheritance();
    test_source_module_import();
    test_control_flow();
    test_assignment_type_error();
    test_sketch_files_on_disk();

    test_run_arithmetic();
    test_run_strings();
    test_run_control_flow();
    test_run_recursion();
    test_run_closures();
    test_run_enums();
    test_run_dynamic_downcast();
    test_run_downcast_error();
    test_run_handler();
    test_run_multi_shot();
    test_run_nested_handlers();
    test_run_ambient_handler();
    test_run_unhandled_op();
    test_run_handler_decl_evaluates_body();
    test_run_host_calls();
    test_run_string_allocation_gc();

    test_match_enums();
    test_match_literals_and_guards();
    test_match_nested_and_closures();
    test_match_tail_and_result_value();
    test_let_destructuring();
    test_match_type_errors();
    test_match_dynamic_scrutinee();
    test_match_effects_and_captures();
    test_handler_case_capture_rejected();
    test_array_basics();
    test_array_dynamic_and_nested();
    test_array_get_option();
    test_array_bounds_effect();
    test_array_unhandled_bounds();
    test_array_type_errors();

    std::cout << "\n" << passes << " passed, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
