// The effects showcase runner: compiles examples/effects/wave.gl once and
// runs the same battle script under each rulebook (handler stack) in turn.
// See the header of wave.gl for what each mode demonstrates.

#include "engine_bindings.hpp"

#include "glscript/bytecode.hpp"
#include "glscript/checker.hpp"
#include "glscript/diagnostic.hpp"
#include "glscript/host.hpp"
#include "glscript/runtime.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr const char* kModes[] = {
    "play_normal", "play_armored", "play_god",
    "play_layered", "play_quiet", "play_surrender",
};

} // namespace

int main(int argc, char** argv) {
    std::string script_path = "examples/effects/wave.gl";
    if (argc > 1) {
        script_path = argv[1];
    }

    glscript::HostRegistry host;
    glscript::register_engine_bindings(host); // println / log fallbacks

    glscript::DiagnosticSink diagnostics;
    glscript::Frontend frontend(host, diagnostics);
    if (!frontend.check_file(script_path)) {
        std::cerr << diagnostics.format();
        return 1;
    }

    auto bytecode = glscript::compile_program(frontend.program());
    glscript::Runtime runtime(frontend.program(), bytecode, host, diagnostics);

    const std::string module_name = std::filesystem::path(script_path).stem().string();

    for (const auto* mode : kModes) {
        std::cout << "=== " << mode << " ===\n";
        runtime.run(module_name, mode, {});
        if (diagnostics.has_errors()) {
            std::cerr << diagnostics.format();
            return 1;
        }
    }

    std::cout << "=== all rulebooks ran the same wave() ===\n";
    return 0;
}
