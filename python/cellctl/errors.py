"""Expected failures. They become a one-line message on stderr, never a traceback."""
from __future__ import annotations


class CellctlError(Exception):
    """An expected, user-facing failure (broker unreachable, nothing to kill, ...)."""

    exit_code = 1


class ConfigError(CellctlError):
    """Bad config file, bad ``--set`` override, or a missing/invalid key."""

    exit_code = 2
