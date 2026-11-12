#include "common/logging.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>

namespace cell {

void init_logging(const std::string& process_name, const std::string& level) {
  auto logger = spdlog::get(process_name);
  if (!logger) {
    logger = spdlog::stderr_color_mt(process_name);
  }
  logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
  logger->set_level(spdlog::level::from_str(level));
  logger->flush_on(spdlog::level::warn);
  spdlog::set_default_logger(std::move(logger));
}

}  // namespace cell
