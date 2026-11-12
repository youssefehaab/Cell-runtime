#include "common/config.hpp"

#include <fstream>
#include <utility>

namespace cell {

Json load_config_file(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw ConfigError("cannot open config file '" + path.string() + "'");
  }
  try {
    return Json::parse(in, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
  } catch (const Json::exception& e) {
    throw ConfigError("config file '" + path.string() + "': " + e.what());
  }
}

const Json* cfg_find(const Json& root, std::string_view dotted_path) {
  const Json* node = &root;
  while (!dotted_path.empty()) {
    const auto dot = dotted_path.find('.');
    const std::string key(dotted_path.substr(0, dot));
    if (!node->is_object()) {
      return nullptr;
    }
    const auto it = node->find(key);
    if (it == node->end()) {
      return nullptr;
    }
    node = &*it;
    dotted_path = dot == std::string_view::npos ? std::string_view{} : dotted_path.substr(dot + 1);
  }
  return node;
}

void cfg_set(Json& root, std::string_view dotted_path, Json value) {
  if (dotted_path.empty()) {
    throw ConfigError("empty config key");
  }
  Json* node = &root;
  while (true) {
    const auto dot = dotted_path.find('.');
    const std::string key(dotted_path.substr(0, dot));
    if (key.empty()) {
      throw ConfigError("empty segment in config key");
    }
    if (!node->is_object()) {
      *node = Json::object();
    }
    node = &(*node)[key];
    if (dot == std::string_view::npos) {
      break;
    }
    dotted_path = dotted_path.substr(dot + 1);
  }
  *node = std::move(value);
}

void cfg_apply_override(Json& root, std::string_view key_equals_value) {
  const auto eq = key_equals_value.find('=');
  if (eq == std::string_view::npos || eq == 0) {
    throw ConfigError("override '" + std::string(key_equals_value) + "' is not key=value");
  }
  const std::string_view key = key_equals_value.substr(0, eq);
  const std::string_view raw = key_equals_value.substr(eq + 1);
  Json value = Json::parse(raw, nullptr, /*allow_exceptions=*/false);
  if (value.is_discarded()) {
    value = std::string(raw);
  }
  cfg_set(root, key, std::move(value));
}

}  // namespace cell
