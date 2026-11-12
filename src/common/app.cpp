#include "common/app.hpp"

#include <CLI/CLI.hpp>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <vector>

#include "common/logging.hpp"
#include "common/signals.hpp"
#include "common/version.hpp"

namespace cell {
namespace {

std::string default_config_path(const std::string& name) {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): called before any thread is started.
  const char* dir = std::getenv("CELL_CONFIG_DIR");
  const std::filesystem::path base = dir != nullptr ? dir : "config";
  return (base / (name + ".json")).string();
}

}  // namespace

int run_app(int argc, char** argv, const AppInfo& info,
            const std::function<int(const Json& config)>& body) {
  CLI::App cli{info.description, info.name};
  std::string config_path = default_config_path(info.name);
  std::vector<std::string> overrides;
  std::string log_level;
  cli.add_option("-c,--config", config_path, "JSON config file")->capture_default_str();
  cli.add_option("--set", overrides, "Override a config value: dotted.key=value (repeatable)");
  cli.add_option("--log-level", log_level, "trace|debug|info|warn|error|critical|off");
  cli.set_version_flag("--version", std::string(info.name) + " " + kVersion);
  CLI11_PARSE(cli, argc, argv);

  Json config;
  try {
    config = load_config_file(config_path);
    for (const auto& o : overrides) {
      cfg_apply_override(config, o);
    }
    if (log_level.empty()) {
      log_level = cfg_get_or<std::string>(config, "log_level", "info");
    }
  } catch (const ConfigError& e) {
    init_logging(info.name, "info");
    spdlog::critical("{}", e.what());
    return 2;
  }

  init_logging(info.name, log_level);
  install_shutdown_handlers();
  spdlog::info("{} {} starting (config: {})", info.name, kVersion, config_path);

  int rc = 1;
  try {
    rc = body(config);
  } catch (const ConfigError& e) {
    spdlog::critical("configuration error: {}", e.what());
    rc = 2;
  } catch (const std::exception& e) {
    spdlog::critical("fatal: {}", e.what());
    rc = 1;
  }
  spdlog::info("{} exiting with code {}", info.name, rc);
  spdlog::shutdown();
  return rc;
}

}  // namespace cell
