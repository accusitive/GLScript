// Shared debug-server plumbing for the example demos.
//
// A demo opts in with `--debug` or `--debug=<port>`; that makes it bind
// 127.0.0.1:<port>, wait for the NSH debugger (or any protocol client), and
// then run its script under the cooperative DebugServer. Print output is
// still written to stdout and is also forwarded as `output` events.
//
// Usage from a demo's argument loop:
//
//   glscript::demo::DebugOptions debug_options;
//   ...
//   switch (glscript::demo::parse_debug_argument(argument, debug_options,
//                                                error)) {
//   case Parsed: continue;
//   case Invalid: return 2;
//   case NotDebug: break;
//   }

#pragma once

#include "glscript/bytecode.hpp"
#include "glscript/debug_server.hpp"
#include "glscript/runtime.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace glscript::demo {

struct DebugOptions {
    bool enabled = false;
    std::uint16_t port = 9000;
};

enum class DebugArgResult {
    NotDebug,
    Parsed,
    Invalid,
};

// Parses `--debug` / `--debug=<port>`. Returns NotDebug for any other
// argument so the caller's normal option parsing can continue.
inline DebugArgResult parse_debug_argument(const std::string& argument,
                                           DebugOptions& options,
                                           std::string& error) {
    if (argument == "--debug") {
        options.enabled = true;
        return DebugArgResult::Parsed;
    }
    if (!argument.starts_with("--debug=")) {
        return DebugArgResult::NotDebug;
    }

    const auto text = argument.substr(8);
    char* end = nullptr;
    errno = 0;
    const auto port = std::strtoul(text.c_str(), &end, 10);
    if (text.empty() || errno != 0 || end == text.c_str() || *end != '\0' ||
        port == 0 || port > 65535) {
        error = "--debug expects a port in 1..65535";
        return DebugArgResult::Invalid;
    }

    options.enabled = true;
    options.port = static_cast<std::uint16_t>(port);
    return DebugArgResult::Parsed;
}

// Owns the optional DebugServer for one demo run. Start it after the Runtime
// exists and before the first script call; the destructor sends the final
// `exited` event, including on early returns from the demo's run loops.
class DebugSession {
public:
    DebugSession(Runtime& runtime, const Program& program,
                 const CompiledProgram& bytecode, const DebugOptions& options)
        : runtime_(runtime), port_(options.port) {
        if (options.enabled) {
            server_ = std::make_unique<DebugServer>(program, bytecode, options.port);
        }
    }

    ~DebugSession() { finish(); }

    DebugSession(const DebugSession&) = delete;
    DebugSession& operator=(const DebugSession&) = delete;

    bool enabled() const { return server_ != nullptr; }

    // Returns false when the server could not bind its port.
    bool start() {
        if (server_ == nullptr) {
            return true;
        }
        if (!server_->start()) {
            return false;
        }
        std::fprintf(stderr,
                     "debug server listening on 127.0.0.1:%u, waiting for a "
                     "debugger to attach...\n",
                     static_cast<unsigned>(port_));
        server_->wait_for_client();
        runtime_.set_debug_hook(server_.get());
        hooked_ = true;
        return true;
    }

    void emit_output(const std::string& text) {
        if (server_ != nullptr) {
            server_->emit_output(text);
        }
    }

    void finish() {
        if (server_ != nullptr && hooked_) {
            server_->finish(runtime_.debug_failed(), runtime_.debug_result());
            hooked_ = false;
        }
    }

private:
    Runtime& runtime_;
    std::unique_ptr<DebugServer> server_;
    std::uint16_t port_ = 0;
    bool hooked_ = false;
};

} // namespace glscript::demo
