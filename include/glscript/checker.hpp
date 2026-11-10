#pragma once

#include "glscript/diagnostic.hpp"
#include "glscript/host.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace glscript {

struct ModuleSource {
    std::string name;
    std::string path;
    std::string text;
};

using ModuleLoader = std::function<std::optional<ModuleSource>(const std::string& module_name)>;

// Front end: parses a root script and all of its imports, then typechecks them.
// The host registry supplies engine-defined modules, global types, and effects.
class Frontend {
public:
    Frontend(const HostRegistry& host, DiagnosticSink& diagnostics, ModuleLoader loader = {});
    ~Frontend();

    Frontend(const Frontend&) = delete;
    Frontend& operator=(const Frontend&) = delete;
    Frontend(Frontend&&) noexcept;
    Frontend& operator=(Frontend&&) noexcept;

    bool check_source(const std::string& module_name, const std::string& path,
                      std::string text);
    bool check_file(const std::string& path);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace glscript
