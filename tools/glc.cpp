#include "glscript/checker.hpp"
#include "glscript/diagnostic.hpp"
#include "glscript/host.hpp"
#include "glscript/hir.hpp"

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
    bool help = false;
};

void print_usage(std::ostream& output) {
    output << "glc - glscript typechecker\n\n"
           << "Usage: glc [options] <file.gl>\n\n"
           << "Options:\n"
           << "  -I <dir>      Add a module search directory\n"
           << "  --dump-hir    Print the lowered HIR after a successful check\n"
           << "  -h, --help    Show this help\n";
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
    glscript::register_engine_bindings(host);

    glscript::DiagnosticSink diagnostics;

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

    std::cout << "glc: checked " << root_path.string() << " successfully ("
              << diagnostics.warning_count() << " warning(s))\n";
    return 0;
}
