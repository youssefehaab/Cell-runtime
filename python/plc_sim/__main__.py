"""Command-line entry point: ``python -m plc_sim`` / the ``plc-sim`` script."""
from __future__ import annotations

import argparse
import asyncio
import logging
import os
import signal
import sys
from collections.abc import Sequence

from . import config as cfg

log = logging.getLogger("plc_sim")

_LEVELS = {
    "trace": logging.DEBUG,
    "debug": logging.DEBUG,
    "info": logging.INFO,
    "warn": logging.WARNING,
    "warning": logging.WARNING,
    "error": logging.ERROR,
    "critical": logging.CRITICAL,
    "off": logging.CRITICAL + 1,
}


def _default_config_path() -> str:
    # Same lookup as the C++ processes: $CELL_CONFIG_DIR/<name>.json, else ./config.
    return os.path.join(os.environ.get("CELL_CONFIG_DIR", "config"), "plc_sim.json")


def _init_logging(level: str) -> None:
    if level.lower() not in _LEVELS:
        raise cfg.ConfigError(f"unknown log level '{level}'")
    logging.basicConfig(
        stream=sys.stderr,
        level=_LEVELS[level.lower()],
        format="[%(asctime)s.%(msecs)03d] [plc_sim] [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
        force=True,
    )
    # pymodbus logs every refused frame at warning/error; keep ours readable.
    logging.getLogger("pymodbus").setLevel(max(_LEVELS[level.lower()], logging.ERROR))


async def _run(config: dict) -> int:
    from . import app  # imported late so --help works without pymodbus/pyzmq

    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for signo in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(signo, stop.set)
    return await app.run(config, stop)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="plc_sim", description="Simulated conveyor PLC")
    parser.add_argument("-c", "--config", default=_default_config_path(),
                        help="JSON config file (default: %(default)s)")
    parser.add_argument("--set", dest="overrides", action="append", default=[],
                        metavar="KEY=VALUE",
                        help="override a config value: dotted.key=value (repeatable)")
    parser.add_argument("--log-level", default=None,
                        help="trace|debug|info|warn|error|critical|off")
    args = parser.parse_args(argv)

    try:
        config = cfg.load_config_file(args.config)
        for override in args.overrides:
            cfg.apply_override(config, override)
        _init_logging(args.log_level or cfg.get(config, "log_level", str, "info"))
    except cfg.ConfigError as exc:
        _init_logging("info")
        log.critical("%s", exc)
        return 2

    log.info("plc_sim starting (config: %s)", args.config)
    try:
        exit_code = asyncio.run(_run(config))
    except cfg.ConfigError as exc:
        log.critical("configuration error: %s", exc)
        exit_code = 2
    except Exception:  # pylint: disable=broad-except
        log.critical("fatal error", exc_info=True)
        exit_code = 1
    log.info("plc_sim exiting with code %d", exit_code)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
