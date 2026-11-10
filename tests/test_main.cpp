#include "glscript/checker.hpp"
#include "glscript/diagnostic.hpp"
#include "glscript/host.hpp"

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

} // namespace

int main() {
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

    std::cout << "\n" << passes << " passed, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
