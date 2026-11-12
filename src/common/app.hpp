#pragma once

#include <functional>
#include <string>

#include "common/config.hpp"

namespace cell {

struct AppInfo {
  std::string name;         // process name; also default config file stem
  std::string description;  // shown in --help
};

// Shared entry point for every C++ process. Handles, in order:
//   --config/-c FILE   JSON config (default: $CELL_CONFIG_DIR/<name>.json,
//                      else ./config/<name>.json)
//   --set key=value    dotted-path override, repeatable, applied after the file
//   --log-level LEVEL  overrides config "log_level"
//   --version
// then installs logging and SIGINT/SIGTERM handlers and calls `body(config)`.
// Exceptions escaping `body` are logged and turned into exit code 1; config
// errors exit with code 2.
int run_app(int argc, char** argv, const AppInfo& info,
            const std::function<int(const Json& config)>& body);

}  // namespace cell
