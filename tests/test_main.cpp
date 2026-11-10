#include "glscript/checker.hpp"
#include "glscript/bytecode.hpp"
#include "glscript/diagnostic.hpp"
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
    int x = count();
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

void test_run_string_allocation_gc() {
    // Enough allocations to trigger several collections.
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

int main() {  // NOLINT(readability-function-size)

    test_entity_sketch();
    test_runtime_sketch();
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

    std::cout << "\n" << passes << " passed, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
