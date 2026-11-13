"""cellctl configuration: built-in defaults, JSON-with-comments file, ``--set`` overrides.

Same conventions as the C++ processes (``src/common/config.hpp``) and plc_sim:
the file may contain ``//`` and ``/* */`` comments, and an override is
``dotted.path=value`` where the value is parsed as JSON when possible and kept
as a plain string otherwise.
"""
from __future__ import annotations

import copy
import json
import os
from pathlib import Path
from typing import Any

from .errors import ConfigError

# Identical to config/cellctl.json (a test keeps the two in sync), so cellctl
# works on a host that has no config directory at all.
DEFAULTS: dict[str, Any] = {
    "broker_sub": "tcp://127.0.0.1:5556",
    "broker_cmd": "tcp://127.0.0.1:5557",
    "command_timeout_ms": 2000,
    "health_timeout_ms": 3000,
    "stats_timeout_ms": 3000,
    "stale_after_ms": 1000,
    "refresh_hz": 5,
    "fault_duration_ms": 3000,
    "processes": ["plc_sim", "arm_sim", "sensor_sim", "plc_bridge", "broker"],
    "run_dir": "/tmp/cell/run",
    "bin_dir": "build/bin",
    "compose_file": "deploy/docker-compose.yml",
    "supervisor": {
        "backoff_initial_ms": 200,
        "backoff_max_ms": 5000,
        "backoff_reset_ms": 10000,
        "stop_grace_ms": 5000,
    },
}

CONFIG_FILE_NAME = "cellctl.json"


def default_config_dir() -> Path:
    """``$CELL_CONFIG_DIR``, else ``./config`` - the lookup every cell process uses."""
    return Path(os.environ.get("CELL_CONFIG_DIR") or "config")


def strip_comments(text: str) -> str:
    """Remove // and /* */ comments, leaving string literals untouched.

    Endpoints such as "tcp://127.0.0.1:5556" contain "//" inside a string, so
    the scanner has to track whether it is in one.
    """
    out: list[str] = []
    i = 0
    n = len(text)
    in_string = False
    while i < n:
        ch = text[i]
        if in_string:
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if ch == '"':
                in_string = False
            i += 1
        elif ch == '"':
            in_string = True
            out.append(ch)
            i += 1
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end == -1 else end  # keep the newline for line numbers
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end == -1:
                raise ConfigError("unterminated /* comment")
            out.append("\n" * text.count("\n", i, end))
            i = end + 2
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def load_config_file(path: str | Path) -> dict[str, Any]:
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError as exc:
        raise ConfigError(f"cannot read config file '{path}': {exc.strerror}") from exc
    try:
        config = json.loads(strip_comments(text))
    except json.JSONDecodeError as exc:
        raise ConfigError(f"config file '{path}': {exc}") from exc
    if not isinstance(config, dict):
        raise ConfigError(f"config file '{path}': top level must be an object")
    return config


def apply_override(config: dict[str, Any], key_equals_value: str) -> None:
    """Apply one ``dotted.path=value`` override in place."""
    key, sep, raw = key_equals_value.partition("=")
    if not sep or not key or any(not part for part in key.split(".")):
        raise ConfigError(f"malformed override '{key_equals_value}': expected dotted.key=value")
    try:
        value = json.loads(raw)
    except json.JSONDecodeError:
        value = raw
    node = config
    parts = key.split(".")
    for part in parts[:-1]:
        child = node.get(part)
        if not isinstance(child, dict):
            child = {}
            node[part] = child
        node = child
    node[parts[-1]] = value


def _merge(base: dict[str, Any], extra: dict[str, Any]) -> None:
    for key, value in extra.items():
        if isinstance(value, dict) and isinstance(base.get(key), dict):
            _merge(base[key], value)
        else:
            base[key] = value


def load(path: str | Path | None, overrides: list[str] | None = None) -> dict[str, Any]:
    """Defaults <- config file <- ``--set`` overrides.

    With ``path=None`` the default location is used and may be absent; a path
    given explicitly must exist.
    """
    config = copy.deepcopy(DEFAULTS)
    if path is None:
        candidate = default_config_dir() / CONFIG_FILE_NAME
        if candidate.is_file():
            _merge(config, load_config_file(candidate))
    else:
        _merge(config, load_config_file(path))
    for override in overrides or []:
        apply_override(config, override)
    return config


_MISSING = object()


def get(config: dict[str, Any], dotted_path: str, kind: type | tuple[type, ...],
        default: Any = _MISSING) -> Any:
    """Typed lookup of a dotted path; required unless a default is given."""
    node: Any = config
    for part in dotted_path.split("."):
        if not isinstance(node, dict) or part not in node:
            if default is _MISSING:
                raise ConfigError(f"missing required config key '{dotted_path}'")
            return default
        node = node[part]
    # bool is an int in Python; never accept it where a number is expected.
    if isinstance(node, bool) and kind is not bool:
        raise ConfigError(f"config key '{dotted_path}': expected a number, got a boolean")
    if not isinstance(node, kind):
        raise ConfigError(f"config key '{dotted_path}': unexpected type {type(node).__name__}")
    return node


def get_str_list(config: dict[str, Any], dotted_path: str) -> list[str]:
    value = get(config, dotted_path, list)
    if not value or not all(isinstance(item, str) and item for item in value):
        raise ConfigError(f"config key '{dotted_path}': expected a non-empty list of strings")
    return list(value)
