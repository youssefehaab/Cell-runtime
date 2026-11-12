#pragma once

#include <spdlog/spdlog.h>

#include <string>

namespace cell {

// Install the process-wide default logger: stderr, "[time] [name] [level] msg".
// stderr keeps stdout free for machine-readable output and suits journald/docker.
// `level` is an spdlog level name (trace|debug|info|warn|error|critical|off).
void init_logging(const std::string& process_name, const std::string& level);

}  // namespace cell
