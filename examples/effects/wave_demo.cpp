// The effects showcase runner: compiles examples/effects/wave.gl once and
// runs the same battle script under each rulebook (handler stack) in turn.
// See the header of wave.gl for what each mode demonstrates.
//
// Debug (waits for the NSH debugger): ./wave_demo --debug[=port] [script.gl]

#include "debug_session.hpp"
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
    glscript::demo::DebugOptions debug_options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        std::string error;
        switch (glscript::demo::parse_debug_argument(argument, debug_options, error)) {
        case glscript::demo::DebugArgResult::Parsed:
            continue;
        case glscript::demo::DebugArgResult::Invalid:
            std::cerr << "wave_demo: " << error << '\n';
            return 2;
        case glscript::demo::DebugArgResult::NotDebug:
            break;
        }
        if (!argument.empty() && argument.front() == '-') {
            std::cerr << "wave_demo: unknown option '" << argument << "'\n";
            return 2;
        }
        script_path = argument;
    }

    glscript::HostRegistry host;
    glscript::demo::DebugSession* debug_session = nullptr;
    glscript::register_engine_bindings(host, [&](const std::string& line) {
        std::cout << line << '\n';
        if (debug_session != nullptr) {
            debug_session->emit_output(line + '\n');
        }
    });

    glscript::DiagnosticSink diagnostics;
    glscript::Frontend frontend(host, diagnostics);
    if (!frontend.check_file(script_path)) {
        std::cerr << diagnostics.format();
        return 1;
    }

    auto bytecode = glscript::compile_program(frontend.program());
    glscript::Runtime runtime(frontend.program(), bytecode, host, diagnostics);

    glscript::demo::DebugSession session(runtime, frontend.program(), bytecode,
                                         debug_options);
    debug_session = &session;
    if (!session.start()) {
        return 1;
    }

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
