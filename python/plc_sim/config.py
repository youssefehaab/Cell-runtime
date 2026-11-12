"""JSON-with-comments config loading and ``--set`` overrides.

Mirrors the C++ side (``src/common/config.hpp``): the file may contain ``//``
and ``/* */`` comments, and an override is ``dotted.path=value`` where the
value is parsed as JSON when possible and kept as a plain string otherwise.
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any


class ConfigError(Exception):
    """Bad config file, bad override, or a missing/invalid key."""


def strip_comments(text: str) -> str:
    """Remove // and /* */ comments, leaving string literals untouched.

    A regex is not enough here: values such as "ipc:///tmp/cell/x" contain
    "//" inside a string, so the scanner has to track whether it is in one.
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
