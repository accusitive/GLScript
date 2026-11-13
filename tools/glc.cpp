#include "glscript/checker.hpp"
#include "glscript/debug_server.hpp"
#include "glscript/diagnostic.hpp"
#include "glscript/disasm.hpp"
#include "glscript/host.hpp"
#include "glscript/hir.hpp"
#include "glscript/runtime.hpp"

#include "engine_bindings.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string file;
    std::vector<std::string> module_paths;
    bool dump_hir = false;
    bool dump_bytecode = false;
    bool run = false;
    bool debug = false;
    std::uint16_t debug_port = 9000;
    bool help = false;
};

void print_usage(std::ostream& output) {
    output << "glc - glscript compiler / runner\n\n"
           << "Usage: glc [options] <file.gl>\n\n"
           << "Options:\n"
           << "  -I <dir>            Add a module search directory\n"
           << "  --dump-hir          Print the lowered HIR after a successful check\n"
           << "  --dump-bytecode     Print a disassembly of the compiled bytecode\n"
           << "  -r, --run           Run the script (entry function `main`)\n"
           << "  --debug[=port]      Run under the TCP debug server (default port 9000);\n"
           << "                      binds 127.0.0.1 and waits for a debugger to attach\n"
           << "  -h, --help          Show this help\n";
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool parse_args(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];

        if (argument == "-h" || argument == "--help") {
            options.help = true;
            continue;
        }

        if (argument == "--dump-hir") {
            options.dump_hir = true;
            continue;
        }

        if (argument == "--dump-bytecode") {
            options.dump_bytecode = true;
            continue;
        }

        if (argument == "-r" || argument == "--run") {
            options.run = true;
            continue;
        }

        if (argument == "--debug") {
            options.debug = true;
            options.run = true;
            continue;
        }

        if (argument.starts_with("--debug=")) {
            options.debug = true;
            options.run = true;
            const auto port = std::strtoul(argument.c_str() + 8, nullptr, 10);
            if (port == 0 || port > 65535) {
                std::cerr << "glc: --debug expects a port in 1..65535\n";
                return false;
            }
            options.debug_port = static_cast<std::uint16_t>(port);
            continue;
        }

        if (argument == "-I") {
            if (i + 1 >= argc) {
                std::cerr << "glc: -I expects a directory\n";
                return false;
            }
            options.module_paths.emplace_back(argv[++i]);
            continue;
        }

        if (argument.starts_with("-I") && argument.size() > 2) {
            options.module_paths.push_back(argument.substr(2));
            continue;
        }

        if (!argument.empty() && argument.front() == '-') {
            std::cerr << "glc: unknown option '" << argument << "'\n";
            return false;
        }

        if (!options.file.empty()) {
            std::cerr << "glc: only one input file is supported\n";
            return false;
        }
        options.file = argument;
    }

    return true;
}

void dump_bytecode(const glscript::CompiledProgram& program) {
    for (std::size_t i = 0; i < program.functions.size(); ++i) {
        const auto& function = program.functions[i];
        std::cout << "fn" << i << " " << function.qualified_name << " (params "
                  << function.param_count << ", locals " << function.local_count
                  << ")\n";
        for (const auto& line : glscript::disassemble(function, program)) {
            std::cout << "  " << std::to_string(line.ip) << ": " << line.text;
            if (line.line != 0) {
                std::cout << "   ; line " << line.line;
            }
            std::cout << '\n';
        }
        std::cout << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        print_usage(std::cerr);
        return 2;
    }

    if (options.help) {
        print_usage(std::cout);
        return 0;
    }

    if (options.file.empty()) {
        std::cerr << "glc: no input file\n";
        print_usage(std::cerr);
        return 2;
    }

    const auto root_path = std::filesystem::absolute(options.file);
    const auto root_directory = root_path.parent_path();

    std::vector<std::filesystem::path> search_paths;
    search_paths.push_back(root_directory);
    search_paths.push_back(std::filesystem::current_path());
    for (const auto& path : options.module_paths) {
        search_paths.push_back(std::filesystem::path(path));
    }

    glscript::HostRegistry host;
    glscript::DiagnosticSink diagnostics;

    // The debug server routes println through output events; the tap is
    // filled in after the server exists.
    glscript::DebugServer* debug_server = nullptr;
    glscript::register_engine_bindings(host, [&](const std::string& line) {
        if (debug_server != nullptr) {
            debug_server->emit_output(line + "\n");
        }
        std::cout << line << '\n';
    });

    auto loader = [search_paths](const std::string& module_name)
        -> std::optional<glscript::ModuleSource> {
        std::string relative;
        for (std::size_t i = 0; i < module_name.size(); ++i) {
            if (module_name[i] == ':' && i + 1 < module_name.size() &&
                module_name[i + 1] == ':') {
                relative.push_back('/');
                ++i;
            } else {
                relative.push_back(module_name[i]);
            }
        }

        for (const auto& directory : search_paths) {
            const auto candidate = directory / (relative + ".gl");
            if (auto text = read_file(candidate); text.has_value()) {
                return glscript::ModuleSource{module_name, candidate.string(), *text};
            }

            const auto nested = directory / relative / (module_name.substr(
                module_name.rfind("::") == std::string::npos
                    ? 0
                    : module_name.rfind("::") + 2) + ".gl");
            if (auto text = read_file(nested); text.has_value()) {
                return glscript::ModuleSource{module_name, nested.string(), *text};
            }
        }

        return std::nullopt;
    };

    glscript::Frontend frontend(host, diagnostics, loader);

    const bool ok = frontend.check_file(root_path.string());

    if (!diagnostics.diagnostics().empty()) {
        std::cerr << diagnostics.format();
    }

    if (!ok) {
        std::cerr << "glc: typecheck failed for " << root_path.string() << '\n';
        return 1;
    }

    if (options.dump_hir) {
        const auto lowered = glscript::hir::lower(frontend.program());
        glscript::hir::dump(lowered, std::cout);
        return 0;
    }

    const auto program = glscript::compile_program(frontend.program());

    if (options.dump_bytecode) {
        dump_bytecode(program);
        if (!options.run) {
            return 0;
        }
    }

    if (!options.run) {
        std::cout << "glc: checked " << root_path.string() << " successfully ("
                  << diagnostics.warning_count() << " warning(s))\n";
        return 0;
    }

    // --- run mode -----------------------------------------------------------
    glscript::Runtime runtime(frontend.program(), program, host, diagnostics);

    glscript::DebugServer server(frontend.program(), program, options.debug_port);
    if (options.debug) {
        if (!server.start()) {
            return 1;
        }
        std::fprintf(stderr, "glc: debug server listening on 127.0.0.1:%u, "
                             "waiting for a debugger to attach...\n",
                     static_cast<unsigned>(options.debug_port));
        server.wait_for_client();
        runtime.set_debug_hook(&server);
        debug_server = &server;
    }

    const auto stem = root_path.stem().string();
    const auto result = runtime.run(stem, "main");

    if (options.debug) {
        server.finish(runtime.debug_failed(), result);
    }

    if (diagnostics.has_errors()) {
        return 1;
    }
    return 0;
}
