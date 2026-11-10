#pragma once

#include "glscript/host.hpp"

#include <functional>
#include <string>

namespace glscript {

// Where Runtime::println / Runtime::log output goes. Defaults to stdout.
using PrintSink = std::function<void(const std::string&)>;

// Registers the example game-engine modules used by the sketch and the CLI.
// A real engine would provide its own equivalent registration function.
// Runtime effect operations carry working thunks so scripts run end to end.
void register_engine_bindings(HostRegistry& host, PrintSink print = {});

} // namespace glscript
