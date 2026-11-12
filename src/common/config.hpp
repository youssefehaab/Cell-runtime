#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cell {

using Json = nlohmann::json;

class ConfigError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Parse a JSON file (// and /* */ comments allowed). Throws ConfigError.
Json load_config_file(const std::filesystem::path& path);

// Look up a dotted path ("modbus.port"). Returns nullptr if any segment is missing.
const Json* cfg_find(const Json& root, std::string_view dotted_path);

// Set a dotted path, creating intermediate objects as needed.
void cfg_set(Json& root, std::string_view dotted_path, Json value);

// Apply a CLI override of the form "dotted.path=value". The value is parsed as
// JSON when possible (numbers, booleans, arrays, quoted strings) and otherwise
// kept as a plain string. Throws ConfigError on a malformed override.
void cfg_apply_override(Json& root, std::string_view key_equals_value);

// Required value: throws ConfigError naming the path if missing or mistyped.
template <typename T>
T cfg_get(const Json& root, std::string_view dotted_path) {
  const Json* node = cfg_find(root, dotted_path);
  if (node == nullptr) {
    throw ConfigError("missing required config key '" + std::string(dotted_path) + "'");
  }
  try {
    return node->get<T>();
  } catch (const Json::exception& e) {
    throw ConfigError("config key '" + std::string(dotted_path) + "': " + e.what());
  }
}

// Optional value with an explicit fallback.
template <typename T>
T cfg_get_or(const Json& root, std::string_view dotted_path, T fallback) {
  const Json* node = cfg_find(root, dotted_path);
  if (node == nullptr || node->is_null()) {
    return fallback;
  }
  try {
    return node->get<T>();
  } catch (const Json::exception& e) {
    throw ConfigError("config key '" + std::string(dotted_path) + "': " + e.what());
  }
}

}  // namespace cell
