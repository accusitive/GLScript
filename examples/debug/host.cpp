// A minimal C++ host for debugging a glscript script over the TCP protocol.
//
// It embeds the frontend, bytecode compiler, interpreter and DebugServer; it
// also exposes a tiny `hostdemo::Game` module so the script can call back into
// C++. Everything after "waiting for debugger" happens under breakpoint/step
// control like glc -r --debug.
//
// Usage:
//   ./debugger_host [script.gl] [port]
//
// Defaults: examples/debug/host_demo.gl and 127.0.0.1:9000.

#include "engine_bindings.hpp"

#include "glscript/checker.hpp"
#include "glscript/debug_server.hpp"
#include "glscript/diagnostic.hpp"
#include "glscript/host.hpp"
#include "glscript/runtime.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

#ifndef GLSCRIPT_SOURCE_DIR
#define GLSCRIPT_SOURCE_DIR "."
#endif

void register_demo_bindings(glscript::HostRegistry& host,
                            const glscript::PrintSink& print) {
    auto demo = host.module("hostdemo");
    auto game = demo.class_type("Game");

    // A deterministic "engine" value so breakpoint walks repeat.
    game.static_method("frames", glscript::TypeRef::integer())
        .call([](glscript::Runtime&, std::vector<glscript::Value>&) {
            return glscript::Value::integer(8);
        });

    // A host callback the script calls each iteration; output goes to both
    // stdout and the debugger's output event stream.
    game.static_method("log", glscript::TypeRef::unit())
        .parameter("message", glscript::TypeRef::string())
        .call([print](glscript::Runtime& runtime,
                      std::vector<glscript::Value>& args) {
            if (!args.empty()) {
                print(runtime.stringify(args[0]));
            }
            return glscript::Value::unit();
        });
}

bool parse_args(int argc, char** argv, std::string& script,
                std::uint16_t& port) {
    if (argc > 1) {
        script = argv[1];
    }
    if (argc > 2) {
        const auto value = std::strtoul(argv[2], nullptr, 10);
        if (value == 0 || value > 65535) {
            std::cerr << "debugger_host: port must be 1..65535\n";
            return false;
        }
        port = static_cast<std::uint16_t>(value);
    }
    if (argc > 3) {
        std::cerr << "usage: debugger_host [script.gl] [port]\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string script = GLSCRIPT_SOURCE_DIR "/examples/debug/host_demo.gl";
    std::uint16_t port = 9000;
    if (!parse_args(argc, argv, script, port)) {
        return 2;
    }
    script = std::filesystem::absolute(script).lexically_normal().string();

    glscript::HostRegistry host;
    glscript::DiagnosticSink diagnostics;

    // The server is created after the front end; host callbacks reach it
    // through this pointer once the script starts.
    glscript::DebugServer* debug_server = nullptr;
    const glscript::PrintSink print = [&](const std::string& line) {
        std::cout << line << '\n';
        if (debug_server != nullptr) {
            debug_server->emit_output(line + "\n");
        }
    };

    glscript::register_engine_bindings(host, print); // println/log
    register_demo_bindings(host, print);

    glscript::Frontend frontend(host, diagnostics);
    if (!frontend.check_file(script)) {
        std::cerr << diagnostics.format();
        std::cerr << "debugger_host: typecheck failed for " << script << '\n';
        return 1;
    }
    if (diagnostics.warning_count() > 0) {
        std::cerr << diagnostics.format();
    }

    const auto& program = frontend.program();
    const auto bytecode = glscript::compile_program(program);
    glscript::Runtime runtime(program, bytecode, host, diagnostics);

    glscript::DebugServer server(program, bytecode, port);
    if (!server.start()) {
        return 1;
    }

    std::cout << "debugger_host: " << script << '\n'
              << "debugger_host: listening on 127.0.0.1:" << port << '\n'
              << "debugger_host: waiting for the NSH debugger (tools/debugger)...\n"
              << std::flush;

    server.wait_for_client();
    runtime.set_debug_hook(&server);
    debug_server = &server;

    const auto module_name = std::filesystem::path(script).stem().string();
    const auto result = runtime.run(module_name, "main");
    server.finish(runtime.debug_failed(), result);

    std::cout << "debugger_host: session ended\n";
    return diagnostics.has_errors() ? 1 : 0;
}
