#pragma once

#include "glscript/host.hpp"

namespace glscript {

// Registers the example game-engine modules used by the sketch and the CLI.
// A real engine would provide its own equivalent registration function.
void register_engine_bindings(HostRegistry& host);

} // namespace glscript
