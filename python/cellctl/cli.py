"""The cellctl command line (typer). Thin glue: parsing, exit codes, printing.

Conventions:
  * stdout carries results only (tables, verdicts, JSON); diagnostics go to stderr.
  * Expected failures (broker unreachable, command rejected, nothing to kill)
    print one line and set the exit code - never a traceback.
  * Exit codes: 0 success, 1 failure/unhealthy, 2 usage or configuration error.
"""
from __future__ import annotations

import functools
import json
import math
import os
import signal
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from types import FrameType
from typing import Any, Optional, TypeVar

import typer
from rich.console import Console
from rich.live import Live

from cell_proto import cell_pb2

from . import __version__, config as cfg, ops, render, report
from .client import CellClient, CommandResult
from .errors import CellctlError
from .supervisor import Supervisor, SupervisorOptions, parse_overrides

F = TypeVar("F", bound=Callable[..., Any])

NON_TTY_WIDTH = 110  # table width when stdout is a pipe or file and COLUMNS is unset

app = typer.Typer(
    name="cellctl",
    help="Diagnostics and operations CLI for the cell runtime.\n\n"
         "Talks to the broker: telemetry on its PUB socket (broker_sub), commands on its "
         "ROUTER socket (broker_cmd). Global options go before the subcommand, e.g. "
         "`cellctl --broker-sub tcp://cell-host:5556 status`.\n\n"
         "Exit codes: 0 success, 1 failure (unhealthy, broker unreachable, command "
         "rejected, nothing killed), 2 usage or configuration error.",
    no_args_is_help=True,
    add_completion=False,
    pretty_exceptions_show_locals=False,
)
cmd_app = typer.Typer(
    help="Send a command to a device through the broker and print the reply. "
         "Exit code 0 only if the device answered ok.",
    no_args_is_help=True)
arm_app = typer.Typer(help="Commands for the robot arm (arm_sim).", no_args_is_help=True)
plc_app = typer.Typer(help="Commands for the conveyor PLC (via plc_bridge).",
                      no_args_is_help=True)
fault_app = typer.Typer(
    help="Fault injection: simulated device outages and killing processes, to "
         "demonstrate detection and recovery.",
    no_args_is_help=True)
cmd_app.add_typer(arm_app, name="arm")
cmd_app.add_typer(plc_app, name="plc")
app.add_typer(cmd_app, name="cmd")
app.add_typer(fault_app, name="fault")

# Numbers such as -0.5 must reach the argument list instead of being parsed as options.
_NEGATIVE_NUMBERS_OK = {"ignore_unknown_options": True}


# ------------------------------------------------------------------ context ---


@dataclass
class Settings:
    """Everything a command needs from the global options (stored in ``ctx.obj``)."""

    config: dict[str, Any]
    json_output: bool = False  # set by the `cmd` and `fault` groups

    @property
    def broker_sub(self) -> str:
        return cfg.get(self.config, "broker_sub", str)

    @property
    def broker_cmd(self) -> str:
        return cfg.get(self.config, "broker_cmd", str)

    def ms(self, key: str) -> int:
        value = cfg.get(self.config, key, int)
        if value < 0:
            raise cfg.ConfigError(f"config key '{key}': must be >= 0")
        return value

    @property
    def processes(self) -> list[str]:
        return cfg.get_str_list(self.config, "processes")

    def client(self) -> CellClient:
        return CellClient(self.broker_sub, self.broker_cmd, self.ms("command_timeout_ms"))


def _settings(ctx: typer.Context) -> Settings:
    settings = ctx.obj
    assert isinstance(settings, Settings)
    return settings


def _err(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def _emit_json(document: Any) -> None:
    sys.stdout.write(json.dumps(document) + "\n")
    sys.stdout.flush()


def _console(width: int | None = None) -> Console:
    if width is None and not sys.stdout.isatty() and "COLUMNS" not in os.environ:
        width = NON_TTY_WIDTH
    return Console(width=width, highlight=False)


def _guard(func: F) -> F:
    """Turn expected failures into a one-line error and an exit code."""

    @functools.wraps(func)
    def wrapper(*args: Any, **kwargs: Any) -> Any:
        try:
            return func(*args, **kwargs)
        except CellctlError as exc:
            _err(f"cellctl: error: {exc}")
            raise typer.Exit(exc.exit_code) from None

    return wrapper  # type: ignore[return-value]


def _interrupt_on_sigterm() -> None:
    """Make SIGTERM behave like Ctrl-C so long-running views exit cleanly."""

    def handler(signum: int, frame: FrameType | None) -> None:
        del signum, frame
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, handler)


def _version(value: bool) -> None:
    if value:
        print(f"cellctl {__version__}")
        raise typer.Exit(0)


@app.callback()
@_guard
def main_callback(
    ctx: typer.Context,
    config: Optional[Path] = typer.Option(
        None, "--config", "-c", metavar="PATH",
        help="JSON config file. Default: $CELL_CONFIG_DIR/cellctl.json, else "
             "./config/cellctl.json; if that file does not exist, built-in defaults are used."),
    overrides: list[str] = typer.Option(
        [], "--set", metavar="KEY=VALUE",
        help="Override a config value: dotted.key=value (repeatable). The value is parsed "
             "as JSON when possible, otherwise taken as a string."),
    broker_sub: Optional[str] = typer.Option(
        None, "--broker-sub", metavar="ENDPOINT", envvar="CELLCTL_BROKER_SUB",
        help="Broker telemetry endpoint (overrides config broker_sub), "
             "e.g. tcp://127.0.0.1:5556."),
    broker_cmd: Optional[str] = typer.Option(
        None, "--broker-cmd", metavar="ENDPOINT", envvar="CELLCTL_BROKER_CMD",
        help="Broker command endpoint (overrides config broker_cmd), "
             "e.g. tcp://127.0.0.1:5557."),
    version: bool = typer.Option(
        False, "--version", callback=_version, is_eager=True, help="Print the version and exit."),
) -> None:
    del version
    if ctx.resilient_parsing:
        return
    loaded = cfg.load(config, overrides)
    if broker_sub:
        loaded["broker_sub"] = broker_sub
    if broker_cmd:
        loaded["broker_cmd"] = broker_cmd
    ctx.obj = Settings(config=loaded)


# ------------------------------------------------------------------- status ---


@app.command()
@_guard
def status(
    ctx: typer.Context,
    once: bool = typer.Option(
        False, "--once",
        help="Print a single snapshot and exit: waits until both cell.state and cell.stats "
             "have arrived, or --timeout-ms has passed. Exit code 1 if no cell.state "
             "was received."),
    duration: Optional[float] = typer.Option(
        None, "--duration", metavar="SECONDS", min=0.0,
        help="Exit after this many seconds (default: run until Ctrl-C)."),
    refresh_hz: Optional[float] = typer.Option(
        None, "--refresh-hz", metavar="HZ", min=0.5, max=30.0,
        help="Screen refresh rate (default: config refresh_hz, 5)."),
    timeout_ms: Optional[int] = typer.Option(
        None, "--timeout-ms", metavar="N", min=0,
        help="With --once: how long to wait for data (default: config health_timeout_ms)."),
    width: Optional[int] = typer.Option(
        None, "--width", metavar="COLUMNS", min=60,
        help=f"Render width (default: the terminal width, or {NON_TTY_WIDTH} when stdout "
             "is not a terminal)."),
) -> None:
    """Live dashboard of the whole cell.

    Shows the overall verdict, every process (status, alive, uptime, last
    heartbeat), the arm, conveyor and sensor, and per-stream rate, latency
    percentiles, sequence gaps and drops. Shows "waiting for broker" until the
    first data arrives and marks the data stale if cell.state stops arriving.
    Ctrl-C exits.
    """
    settings = _settings(ctx)
    stale_after_s = settings.ms("stale_after_ms") / 1000.0
    hz = refresh_hz if refresh_hz is not None else float(cfg.get(settings.config, "refresh_hz",
                                                                 (int, float)))
    period_s = 1.0 / min(max(hz, 0.5), 30.0)
    wait_s = (timeout_ms if timeout_ms is not None else settings.ms("health_timeout_ms")) / 1000.0
    console = _console(width)

    with settings.client() as client:
        snapshot = client.snapshot

        def frame() -> Any:
            return render.dashboard(snapshot, time.monotonic(), time.strftime("%H:%M:%S"),
                                    settings.broker_sub, stale_after_s)

        if once:
            client.pump(wait_s, until=lambda: snapshot.state is not None
                        and snapshot.stats is not None)
            console.print(frame())
            raise typer.Exit(0 if snapshot.state is not None else 1)

        _interrupt_on_sigterm()
        end = None if duration is None else time.monotonic() + duration
        try:
            with Live(frame(), console=console, screen=console.is_terminal,
                      auto_refresh=False) as live:
                while end is None or time.monotonic() < end:
                    client.pump(period_s if end is None
                                else max(0.0, min(period_s, end - time.monotonic())))
                    live.update(frame(), refresh=True)
        except KeyboardInterrupt:
            pass
    raise typer.Exit(0)


# ------------------------------------------------------------------- health ---


@app.command()
@_guard
def health(
    ctx: typer.Context,
    json_output: bool = typer.Option(
        False, "--json",
        help="Print one JSON object on stdout and nothing else: {healthy, broker_reachable, "
             "processes: [{name, status, alive, uptime_ms, last_seen_age_ms, detail}], reason}."),
    timeout_ms: Optional[int] = typer.Option(
        None, "--timeout-ms", metavar="N", min=1,
        help="How long to wait for a first cell.state before declaring the broker "
             "unreachable (default: config health_timeout_ms, 3000)."),
    wait_healthy_ms: int = typer.Option(
        0, "--wait-healthy-ms", metavar="N", min=0,
        help="Keep watching for up to N ms and succeed as soon as the cell reports healthy, "
             "instead of judging the first sample. Use right after a restart or upgrade, "
             "while processes are still starting."),
) -> None:
    """Health check for scripts: exit code 0 if the cell is healthy, 1 otherwise.

    Healthy is the broker's verdict: every expected process is alive and
    reports OK. Exit code 1 means unhealthy, or that no cell.state arrived
    within the timeout (broker down or unreachable). Prints one line per
    process and a verdict, or a JSON object with --json.
    """
    settings = _settings(ctx)
    first_timeout_ms = timeout_ms if timeout_ms is not None else settings.ms("health_timeout_ms")
    stale_after_ms = settings.ms("stale_after_ms")

    with settings.client() as client:
        start = time.monotonic()
        first_deadline = start + first_timeout_ms / 1000.0
        healthy_deadline = start + wait_healthy_ms / 1000.0
        state: cell_pb2.CellState | None = None
        try:
            while state is None or not state.healthy:
                # Until something arrives the broker gets the longer of the two
                # waits; after that only the --wait-healthy-ms window applies.
                deadline = (healthy_deadline if state is not None
                            else max(first_deadline, healthy_deadline))
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                fresh = client.wait_state(min(remaining, 0.25))
                if fresh is not None:
                    state = fresh
        except KeyboardInterrupt:
            _err("cellctl: interrupted")
            raise typer.Exit(130) from None
        waited_ms = round((time.monotonic() - start) * 1000.0)
        age_s = client.snapshot.state_age_s(time.monotonic())

    document = report.health_report(state, settings.broker_sub, waited_ms)
    if (state is not None and not state.healthy and age_s is not None
            and age_s * 1000.0 > stale_after_ms):
        # The broker went quiet while we were waiting for it to turn healthy.
        document["broker_reachable"] = False
        document["reason"] = (f"broker stopped publishing cell.state {age_s * 1000.0:.0f} ms ago; "
                              f"last report: {document['reason']}")

    if json_output:
        _emit_json(document)
    else:
        console = _console()
        for line in render.health_lines(document):
            console.print(line, soft_wrap=True)
    raise typer.Exit(0 if document["healthy"] else 1)


# -------------------------------------------------------------------- stats ---


@app.command()
@_guard
def stats(
    ctx: typer.Context,
    json_output: bool = typer.Option(
        False, "--json",
        help="JSON instead of a table (latencies in microseconds). One object; with "
             "--watch, one object per line."),
    watch: bool = typer.Option(
        False, "--watch", "-w",
        help="Keep printing every window until Ctrl-C (or until --count windows)."),
    count: Optional[int] = typer.Option(
        None, "--count", "-n", metavar="N", min=1,
        help="Number of windows. Without --watch the N windows are aggregated into one "
             "result (counts summed; p99/p999/max are the worst window's, p50 the "
             "count-weighted mean). With --watch, stop after N windows."),
    timeout_ms: Optional[int] = typer.Option(
        None, "--timeout-ms", metavar="N", min=1,
        help="Longest wait for each window (default: config stats_timeout_ms, 3000)."),
) -> None:
    """Latency and throughput statistics per stream, as computed by the broker.

    Prints the next stats window (the broker publishes one per second): message
    rate, p50/p99/p999/max latency from device send to broker receive, sequence
    gaps and ring-buffer drops for the window and since broker start. Exit code
    1 if no stats arrive within the timeout.
    """
    settings = _settings(ctx)
    wait_s = (timeout_ms if timeout_ms is not None else settings.ms("stats_timeout_ms")) / 1000.0
    wanted = count if count is not None else (None if watch else 1)
    console = _console()

    def emit(window: cell_pb2.CellStats, windows: int) -> None:
        if json_output:
            _emit_json(report.stats_report(window, windows))
        else:
            console.print(render.streams_table(window, windows))

    collected: list[cell_pb2.CellStats] = []
    with settings.client() as client:
        try:
            while wanted is None or len(collected) < wanted:
                window = client.next_stats(wait_s)
                if window is None:
                    raise CellctlError(
                        f"no cell.stats received from {settings.broker_sub} within "
                        f"{wait_s * 1000.0:.0f} ms"
                        + (f" (after {len(collected)} windows)" if collected else
                           " (broker unreachable?)"))
                if watch:
                    emit(window, 1)
                    collected = collected[-1:] + [window] if wanted is None else collected + [window]
                    if wanted is None:
                        collected = collected[-1:]  # only the count matters; keep memory flat
                else:
                    collected.append(window)
        except KeyboardInterrupt:
            raise typer.Exit(0) from None
    if not watch:
        emit(report.aggregate_stats(collected) if len(collected) > 1 else collected[0],
             len(collected))
    raise typer.Exit(0)


# ---------------------------------------------------------------- commands ---


def _report_command(settings: Settings, label: str, result: CommandResult) -> None:
    if settings.json_output:
        latency = None if result.latency_ms is None else round(result.latency_ms, 3)
        _emit_json({"ok": result.ok, "error": result.error, "latency_ms": latency})
    elif result.ok:
        print(f"{label}: ok ({result.latency_ms:.1f} ms)", flush=True)
    else:
        _err(f"{label}: FAILED: {result.error}")
    raise typer.Exit(0 if result.ok else 1)


def _send(ctx: typer.Context, label: str, command: cell_pb2.Command) -> None:
    settings = _settings(ctx)
    with settings.client() as client:
        result = client.send_command(command)
    _report_command(settings, label, result)


_JSON_REPLY_HELP = ('Print the reply as one JSON object on stdout: '
                    '{"ok": bool, "error": str, "latency_ms": number|null}.')


@cmd_app.callback()
def cmd_callback(
    ctx: typer.Context,
    json_output: bool = typer.Option(False, "--json", help=_JSON_REPLY_HELP),
) -> None:
    if ctx.resilient_parsing or ctx.obj is None:
        return
    _settings(ctx).json_output = json_output


@arm_app.command("move-to", context_settings=_NEGATIVE_NUMBERS_OK)
@_guard
def arm_move_to(
    ctx: typer.Context,
    joints: list[float] = typer.Argument(
        ..., metavar="J1 J2 J3 J4 J5 J6",
        help="Target joint positions in radians, one per joint (six for the default arm). "
             "Negative values are fine: `cellctl cmd arm move-to 0.4 -0.3 0.2 0.1 -0.2 0`."),
) -> None:
    """Move the arm to the given joint positions (trapezoidal velocity profile)."""
    if any(not math.isfinite(value) for value in joints):
        raise CellctlError("joint positions must be finite numbers")
    _send(ctx, "arm move-to", cell_pb2.Command(target="arm",
                                               move_to=cell_pb2.MoveTo(joints=joints)))


@arm_app.command("stop")
@_guard
def arm_stop(ctx: typer.Context) -> None:
    """Decelerate to rest at the normal rate and return to IDLE."""
    _send(ctx, "arm stop", cell_pb2.Command(target="arm", stop=cell_pb2.Stop()))


@arm_app.command("estop")
@_guard
def arm_estop(ctx: typer.Context) -> None:
    """Emergency stop: brake hard and latch ESTOP until `cellctl cmd arm reset`."""
    _send(ctx, "arm estop", cell_pb2.Command(target="arm", estop=cell_pb2.EStop()))


@arm_app.command("reset")
@_guard
def arm_reset(ctx: typer.Context) -> None:
    """Clear a latched ESTOP (refused while the arm is still decelerating)."""
    _send(ctx, "arm reset", cell_pb2.Command(target="arm", reset=cell_pb2.Reset()))


@plc_app.command("speed", context_settings=_NEGATIVE_NUMBERS_OK)
@_guard
def plc_speed(
    ctx: typer.Context,
    speed: float = typer.Argument(..., metavar="SPEED", help="Conveyor speed setpoint in m/s."),
) -> None:
    """Set the conveyor speed setpoint (m/s); the belt ramps to it."""
    if not math.isfinite(speed):
        raise CellctlError("speed must be a finite number")
    _send(ctx, "plc speed", cell_pb2.Command(
        target="plc", set_conveyor_speed=cell_pb2.SetConveyorSpeed(speed=speed)))


# ------------------------------------------------------------------- faults ---


@fault_app.callback()
def fault_callback(
    ctx: typer.Context,
    json_output: bool = typer.Option(
        False, "--json",
        help="For the device faults (not `kill`): " + _JSON_REPLY_HELP[0].lower()
             + _JSON_REPLY_HELP[1:]),
) -> None:
    if ctx.resilient_parsing or ctx.obj is None:
        return
    _settings(ctx).json_output = json_output


def _fault_command(target: str, kind: int, duration_ms: int = 0) -> cell_pb2.Command:
    return cell_pb2.Command(target=target, fault_inject=cell_pb2.FaultInject(
        kind=kind, duration_ms=duration_ms))


def _duration(settings: Settings, duration_ms: int | None) -> int:
    return duration_ms if duration_ms is not None else settings.ms("fault_duration_ms")


_DURATION_HELP = "Outage length in ms (default: config fault_duration_ms, 3000)."


@fault_app.command("disconnect-sensor")
@_guard
def fault_disconnect_sensor(
    ctx: typer.Context,
    duration_ms: Optional[int] = typer.Option(
        None, "--duration-ms", metavar="N", min=0,
        help=_DURATION_HELP + " 0 = stay disconnected until `cellctl fault reconnect-sensor`."),
) -> None:
    """Make sensor_sim stop publishing frames, as if the sensor was unplugged.

    The process stays alive and reports DEGRADED, so the cell turns unhealthy
    and recovers by itself when the outage ends.
    """
    _send(ctx, "fault disconnect-sensor", _fault_command(
        "sensor", cell_pb2.FaultInject.DISCONNECT, _duration(_settings(ctx), duration_ms)))


@fault_app.command("reconnect-sensor")
@_guard
def fault_reconnect_sensor(ctx: typer.Context) -> None:
    """End a sensor disconnect early."""
    _send(ctx, "fault reconnect-sensor", _fault_command("sensor", cell_pb2.FaultInject.RECONNECT))


@fault_app.command("plc-down")
@_guard
def fault_plc_down(
    ctx: typer.Context,
    duration_ms: Optional[int] = typer.Option(
        None, "--duration-ms", metavar="N", min=1, help=_DURATION_HELP),
) -> None:
    """Take the PLC off the network: plc_bridge tells plc_sim to drop its Modbus link.

    plc_bridge reports DEGRADED (link down) and reconnects with exponential
    backoff once the PLC is back.
    """
    _send(ctx, "fault plc-down", _fault_command(
        "plc", cell_pb2.FaultInject.DISCONNECT, _duration(_settings(ctx), duration_ms)))


def _check_process(settings: Settings, process: str) -> None:
    known = settings.processes
    if process not in known:
        raise CellctlError(f"unknown process '{process}' (expected one of: {', '.join(known)})")


def _run_dir(settings: Settings, run_dir: Path | None) -> Path:
    return run_dir if run_dir is not None else Path(cfg.get(settings.config, "run_dir", str))


def _compose_file(settings: Settings, compose_file: Path | None) -> Path:
    return (compose_file if compose_file is not None
            else Path(cfg.get(settings.config, "compose_file", str)))


_MODE_HELP = ("Which supervisor runs the cell: local (`cellctl up`), systemd "
              "(cell-<process>.service) or docker (compose service). ")
_RUN_DIR_HELP = "Run directory of `cellctl up` (default: config run_dir, /tmp/cell/run)."
_COMPOSE_HELP = "Compose file for --mode docker (default: config compose_file)."


@fault_app.command("kill")
@_guard
def fault_kill(
    ctx: typer.Context,
    process: str = typer.Argument(
        ..., metavar="PROCESS", help="arm_sim | sensor_sim | plc_bridge | plc_sim | broker"),
    mode: str = typer.Option(
        "auto", "--mode", metavar="auto|local|systemd|docker",
        help=_MODE_HELP + "auto = systemd if the unit is active, else local."),
    run_dir: Optional[Path] = typer.Option(None, "--run-dir", metavar="DIR", help=_RUN_DIR_HELP),
    compose_file: Optional[Path] = typer.Option(None, "--compose-file", metavar="FILE",
                                                help=_COMPOSE_HELP),
    kill_all: bool = typer.Option(
        False, "--all",
        help="Local mode without a pidfile: kill every matching process instead of "
             "refusing when more than one is found."),
) -> None:
    """SIGKILL a cell process so that its supervisor has to restart it.

    local: the PID comes from <run_dir>/<process>.pid (written by `cellctl
    up`), else from an exact process-name match in /proc. systemd: `systemctl
    kill --signal=SIGKILL cell-<process>.service`. docker: `docker compose
    kill -s SIGKILL <service>`. Prints what was done; exit code 1 if nothing
    was killed.
    """
    settings = _settings(ctx)
    _check_process(settings, process)
    resolved = ops.resolve_mode(mode, process)
    if resolved == "systemd":
        result = ops.kill_systemd(process, _err)
    elif resolved == "docker":
        result = ops.kill_docker(process, _compose_file(settings, compose_file), _err)
    else:
        result = ops.kill_local(process, _run_dir(settings, run_dir), kill_all)
    print(f"[{result.mode}] {result.description}", flush=True)


# --------------------------------------------------------------------- logs ---


@app.command()
@_guard
def logs(
    ctx: typer.Context,
    process: Optional[str] = typer.Argument(
        None, metavar="[PROCESS]",
        help="arm_sim | sensor_sim | plc_bridge | plc_sim | broker (default: all)."),
    lines: int = typer.Option(50, "--lines", "-n", metavar="LINES", min=0,
                              help="Number of most recent lines to show per process."),
    follow: bool = typer.Option(False, "--follow", "-f",
                                help="Keep printing new lines until Ctrl-C."),
    mode: str = typer.Option(
        "auto", "--mode", metavar="auto|local|systemd|docker",
        help=_MODE_HELP + "auto = local if `cellctl up` logs exist, else systemd if the "
                          "unit is active."),
    run_dir: Optional[Path] = typer.Option(None, "--run-dir", metavar="DIR", help=_RUN_DIR_HELP),
    compose_file: Optional[Path] = typer.Option(None, "--compose-file", metavar="FILE",
                                                help=_COMPOSE_HELP),
) -> None:
    """Show recent log output of one process, or of all of them.

    local: reads <run_dir>/logs/<process>.log written by `cellctl up`.
    systemd: runs `journalctl -u cell-<process>.service`. docker: runs `docker
    compose logs <service>`.
    """
    settings = _settings(ctx)
    if process is not None:
        _check_process(settings, process)
    processes = [process] if process is not None else settings.processes
    directory = _run_dir(settings, run_dir)
    if mode not in ops.MODES:
        raise CellctlError(f"unknown mode '{mode}' (expected one of: {', '.join(ops.MODES)})")
    if mode == "auto":
        if any(ops.log_path(directory, p).is_file() for p in processes):
            mode = "local"
        else:
            mode = "systemd" if ops.systemd_unit_active(processes[-1]) else "local"

    _interrupt_on_sigterm()
    try:
        if mode == "systemd":
            code = ops.exec_log_command(ops.journalctl_argv(processes, lines, follow), _err)
        elif mode == "docker":
            code = ops.exec_log_command(ops.compose_logs_argv(
                processes, _compose_file(settings, compose_file), lines, follow), _err)
        else:
            ops.show_local_logs(processes, directory, lines, follow)
            code = 0
    except KeyboardInterrupt:
        code = 0
    except BrokenPipeError:
        # `cellctl logs | head`: the reader went away, which is not an error.
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        code = 0
    raise typer.Exit(code)


# ----------------------------------------------------------------------- up ---


@app.command()
@_guard
def up(
    ctx: typer.Context,
    only: Optional[str] = typer.Option(
        None, "--only", metavar="P1,P2,...",
        help="Run only these processes (comma-separated), e.g. --only arm_sim,broker."),
    bin_dir: Optional[Path] = typer.Option(
        None, "--bin-dir", metavar="DIR", envvar="CELL_BIN_DIR",
        help="Directory with the C++ binaries (default: config bin_dir, build/bin; "
             "then PATH)."),
    config_dir: Optional[Path] = typer.Option(
        None, "--config-dir", metavar="DIR",
        help="Directory with <process>.json configs (default: $CELL_CONFIG_DIR, else ./config)."),
    run_dir: Optional[Path] = typer.Option(
        None, "--run-dir", metavar="DIR",
        help="Where pidfiles (<process>.pid) and logs (logs/<process>.log) are written "
             "(default: config run_dir, /tmp/cell/run)."),
    override: list[str] = typer.Option(
        [], "--override", metavar="PROCESS:KEY=VALUE",
        help="Config override passed to one process as its --set (repeatable), e.g. "
             "--override 'broker:pub_endpoint=tcp://*:6001'."),
) -> None:
    """Run the cell locally: start every process, restart any that dies.

    A small supervisor for development, tests and hosts without systemd. Starts
    plc_sim, arm_sim, sensor_sim, plc_bridge and broker as child processes,
    restarts a child that exits unexpectedly (exponential backoff), and on
    Ctrl-C or SIGTERM stops them all (SIGTERM, then SIGKILL after a grace
    period). Exit code 0 after a clean stop. `cellctl fault kill` and `cellctl
    logs` find the processes through the run directory.
    """
    settings = _settings(ctx)
    known = settings.processes
    selected = known
    if only is not None:
        wanted = [name.strip() for name in only.split(",") if name.strip()]
        unknown = [name for name in wanted if name not in known]
        if unknown or not wanted:
            raise CellctlError(f"--only: unknown process '{', '.join(unknown)}' "
                               f"(expected a comma-separated subset of: {', '.join(known)})")
        selected = [name for name in known if name in wanted]

    def seconds(key: str) -> float:
        return settings.ms(f"supervisor.{key}") / 1000.0

    options = SupervisorOptions(
        processes=tuple(selected),
        bin_dir=bin_dir if bin_dir is not None else Path(cfg.get(settings.config, "bin_dir", str)),
        config_dir=(config_dir if config_dir is not None else cfg.default_config_dir()).resolve(),
        run_dir=_run_dir(settings, run_dir),
        overrides=parse_overrides(override, known),
        backoff_initial_s=max(seconds("backoff_initial_ms"), 0.01),
        backoff_max_s=max(seconds("backoff_max_ms"), 0.01),
        backoff_reset_s=seconds("backoff_reset_ms"),
        stop_grace_s=seconds("stop_grace_ms"),
    )
    raise typer.Exit(Supervisor(options).run())
