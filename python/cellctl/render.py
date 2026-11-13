"""Pure functions from telemetry to rich renderables.

Nothing here touches a socket or a clock: callers pass in the snapshot and the
current time, so every function can be rendered to a string in a unit test.
"""
from __future__ import annotations

from typing import Any

from rich import box
from rich.console import Group, RenderableType
from rich.panel import Panel
from rich.table import Table
from rich.text import Text

from cell_proto import cell_pb2

from .client import Snapshot
from .report import arm_mode_name, process_status_name

DASH = "-"

_STATUS_STYLES = {
    cell_pb2.PROCESS_OK: "green",
    cell_pb2.PROCESS_STARTING: "yellow",
    cell_pb2.PROCESS_DEGRADED: "yellow",
    cell_pb2.PROCESS_STOPPING: "yellow",
    cell_pb2.PROCESS_FAULT: "bold red",
}

_ARM_MODE_STYLES = {
    cell_pb2.ARM_IDLE: "green",
    cell_pb2.ARM_MOVING: "cyan",
    cell_pb2.ARM_STOPPING: "yellow",
    cell_pb2.ARM_ESTOP: "bold red",
}


# -------------------------------------------------------------- formatting ---


def fmt_duration_ms(ms: int) -> str:
    """Uptime-style duration: ``850 ms``, ``12.3 s``, ``4m 05s``, ``1h 02m``, ``3d 04h``."""
    if ms < 1000:
        return f"{ms} ms"
    seconds = ms / 1000.0
    if seconds < 60:
        return f"{seconds:.1f} s"
    whole = int(seconds)
    if whole < 3600:
        return f"{whole // 60}m {whole % 60:02d}s"
    if whole < 86400:
        return f"{whole // 3600}h {whole % 3600 // 60:02d}m"
    return f"{whole // 86400}d {whole % 86400 // 3600:02d}h"


def fmt_us(ns: int) -> str:
    """Nanoseconds as microseconds with one decimal: ``1,234.5``."""
    return f"{ns / 1000.0:,.1f}"


def fmt_bytes(n: int) -> str:
    value = float(n)
    for unit in ("B", "kB", "MB"):
        if value < 1000:
            return f"{value:.0f} B" if unit == "B" else f"{value:.2f} {unit}"
        value /= 1000.0
    return f"{value:.2f} GB"


def fmt_count(n: int) -> str:
    return f"{n:,}"


def _problem_count(n: int) -> Text:
    """Gaps/drops: quiet when zero, loud otherwise."""
    return Text(fmt_count(n), style="dim" if n == 0 else "bold red")


def _yes_no(value: bool, good: bool = True) -> Text:
    """``yes``/``no`` coloured by whether this value is the healthy one."""
    return Text("yes" if value else "no", style="green" if value == good else "bold red")


def status_text(status: int, alive: bool) -> Text:
    name = process_status_name(status)
    if not alive:
        return Text(name, style="red")
    return Text(name, style=_STATUS_STYLES.get(status, "magenta"))


# ------------------------------------------------------------------ pieces ---


def processes_table(state: cell_pb2.CellState, stale: bool = False) -> Table:
    table = Table(title="processes" + (" (stale)" if stale else ""), title_justify="left",
                  box=box.ROUNDED, expand=True, header_style="bold",
                  style="dim" if stale else "", title_style="bold")
    table.add_column("process", no_wrap=True)
    table.add_column("status", no_wrap=True)
    table.add_column("alive", no_wrap=True)
    table.add_column("uptime", justify="right", no_wrap=True)
    table.add_column("last seen", justify="right", no_wrap=True)
    table.add_column("detail", ratio=1, overflow="ellipsis", no_wrap=True)
    for proc in state.processes:
        table.add_row(
            Text(proc.name, style="bold"),
            status_text(proc.status, proc.alive),
            _yes_no(proc.alive),
            fmt_duration_ms(proc.uptime_ms),
            Text(f"{proc.last_seen_age_ms} ms", style="" if proc.alive else "red"),
            proc.detail or Text(DASH, style="dim"),
        )
    if not state.processes:
        table.add_row(Text("no processes reported", style="dim"), "", "", "", "", "")
    return table


def _key_value_grid(rows: list[tuple[str, Any]]) -> Table:
    grid = Table.grid(padding=(0, 2))
    grid.add_column(style="dim", no_wrap=True)
    grid.add_column(no_wrap=True)
    for key, value in rows:
        grid.add_row(key, value)
    return grid


def _panel(title: str, body: RenderableType, stale: bool, height: int | None = None) -> Panel:
    return Panel(body, title=f"[bold]{title}[/bold]" + (" (stale)" if stale else ""),
                 title_align="left", box=box.ROUNDED, height=height,
                 style="dim" if stale else "", padding=(0, 1))


PANEL_HEIGHT = 6  # arm, conveyor and sensor panels sit side by side


def arm_panel(state: cell_pb2.CellState, stale: bool = False) -> Panel:
    if not state.HasField("arm"):
        return _panel("arm", Text("no data", style="dim"), stale, PANEL_HEIGHT)
    arm = state.arm
    joints = Table(box=None, padding=(0, 1), header_style="dim", show_edge=False)
    joints.add_column(Text.assemble("mode ", (arm_mode_name(arm.mode),
                                              _ARM_MODE_STYLES.get(arm.mode, "magenta"))),
                      no_wrap=True, header_style="")
    count = max(len(arm.position), len(arm.velocity))
    for i in range(count):
        joints.add_column(f"J{i + 1}", justify="right", no_wrap=True)

    def row(label: str, values: Any) -> None:
        cells = [f"{values[i]:+.3f}" if i < len(values) else DASH for i in range(count)]
        joints.add_row(Text(label, style="dim"), *cells)

    row("position rad", arm.position)
    row("velocity rad/s", arm.velocity)
    return _panel("arm", joints, stale, PANEL_HEIGHT)


def conveyor_panel(state: cell_pb2.CellState, stale: bool = False) -> Panel:
    if not state.HasField("conveyor"):
        return _panel("conveyor", Text("no data", style="dim"), stale, PANEL_HEIGHT)
    conveyor = state.conveyor
    fault = (Text("0 (none)", style="green") if conveyor.fault_code == 0
             else Text(str(conveyor.fault_code), style="bold red"))
    rows = [
        ("speed", f"{conveyor.speed:.3f} m/s"),
        ("item present", "yes" if conveyor.item_present else "no"),
        ("fault code", fault),
        ("link up", _yes_no(conveyor.link_up)),
    ]
    return _panel("conveyor", _key_value_grid(rows), stale, PANEL_HEIGHT)


def frame_age_ms(state: cell_pb2.CellState, state_age_s: float) -> float | None:
    """Age of the latest sensor frame.

    Both stamps in the state come from the cell host's monotonic clock, so their
    difference is valid even when cellctl runs on another machine; the time
    since the state was received is then added from the local clock.
    """
    if not state.HasField("sensor") or state.sensor.h.send_mono_ns == 0:
        return None
    at_publish_ns = state.h.send_mono_ns - state.sensor.h.send_mono_ns
    return max(0.0, at_publish_ns / 1e6 + state_age_s * 1000.0)


def sensor_panel(state: cell_pb2.CellState, state_age_s: float = 0.0,
                 stale: bool = False) -> Panel:
    if not state.HasField("sensor"):
        return _panel("sensor", Text("no data", style="dim"), stale, PANEL_HEIGHT)
    sensor = state.sensor
    age = frame_age_ms(state, state_age_s)
    if age is None:
        age_text = Text(DASH, style="dim")
    elif age >= 1000.0:
        age_text = Text(f"{age / 1000.0:.1f} s", style="bold red")
    else:
        age_text = Text(f"{age:.0f} ms")
    rows = [
        ("resolution", f"{sensor.width} x {sensor.height}"),
        ("frame seq", fmt_count(sensor.h.seq)),
        ("frame size", fmt_bytes(sensor.bytes)),
        ("frame age", age_text),
    ]
    return _panel("sensor", _key_value_grid(rows), stale, PANEL_HEIGHT)


def device_row(state: cell_pb2.CellState, state_age_s: float = 0.0,
               stale: bool = False) -> Table:
    """Arm, conveyor and sensor panels side by side."""
    grid = Table.grid(expand=True)
    grid.add_column(ratio=5)
    grid.add_column(ratio=2)
    grid.add_column(ratio=2)
    grid.add_row(arm_panel(state, stale), conveyor_panel(state, stale),
                 sensor_panel(state, state_age_s, stale))
    return grid


def streams_table(stats: cell_pb2.CellStats, windows: int = 1, stale: bool = False) -> Table:
    """Per-stream latency/throughput table (also the output of ``cellctl stats``)."""
    span = f"{stats.window_ms} ms window"
    if windows > 1:
        span = f"{windows} windows, {stats.window_ms} ms"
    title = (f"streams ({span})  ring high-water "
             f"{fmt_count(stats.ring_high_water)} / {fmt_count(stats.ring_capacity)}")
    table = Table(title=title + (" (stale)" if stale else ""), title_justify="left",
                  box=box.ROUNDED, expand=True, header_style="bold",
                  style="dim" if stale else "", title_style="bold")
    table.add_column("stream", no_wrap=True)
    for name in ("rate Hz", "msgs", "p50 µs", "p99 µs", "p999 µs", "max µs", "gaps", "drops",
                 "total msgs", "total gaps", "total drops"):
        table.add_column(name, justify="right", no_wrap=True)
    for s in stats.streams:
        table.add_row(
            Text(s.stream, style="bold"),
            f"{s.rate_hz:,.1f}",
            fmt_count(s.count),
            fmt_us(s.p50_ns),
            fmt_us(s.p99_ns),
            fmt_us(s.p999_ns),
            fmt_us(s.max_ns),
            _problem_count(s.seq_gaps),
            _problem_count(s.drops),
            fmt_count(s.total_count),
            _problem_count(s.total_seq_gaps),
            _problem_count(s.total_drops),
        )
    if not stats.streams:
        table.add_row(Text("no streams reported", style="dim"), *[""] * 11)
    return table


# --------------------------------------------------------------- dashboard ---


def header_panel(snapshot: Snapshot, now: float, wall_time: str, sub_endpoint: str,
                 stale_after_s: float) -> Panel:
    state = snapshot.state
    age = snapshot.state_age_s(now)
    reachable = snapshot.broker_reachable(now, stale_after_s)
    if state is None or age is None:
        verdict = Text("WAITING", style="bold yellow")
        border = "yellow"
    elif age > stale_after_s:
        last = "HEALTHY" if state.healthy else "UNHEALTHY"
        verdict = Text(f"STALE  no cell.state for {age:.1f} s (was {last})", style="bold yellow")
        border = "yellow"
    elif state.healthy:
        verdict = Text("HEALTHY", style="bold green")
        border = "green"
    else:
        verdict = Text("UNHEALTHY", style="bold red")
        border = "red"

    broker = Text.assemble(("broker ", "dim"), sub_endpoint, "  ",
                           ("reachable", "green") if reachable else ("unreachable", "bold red"))
    grid = Table.grid(expand=True)
    grid.add_column(ratio=2)
    grid.add_column(ratio=3, justify="center")
    grid.add_column(ratio=1, justify="right")
    grid.add_row(Text.assemble(("cell ", "dim"), verdict), broker, Text(wall_time, style="dim"))
    return Panel(grid, box=box.ROUNDED, border_style=border, title="[bold]cellctl status[/bold]",
                 title_align="left", padding=(0, 1))


def dashboard(snapshot: Snapshot, now: float, wall_time: str, sub_endpoint: str,
              stale_after_s: float) -> RenderableType:
    """The whole ``cellctl status`` screen for one instant."""
    parts: list[RenderableType] = [
        header_panel(snapshot, now, wall_time, sub_endpoint, stale_after_s)]
    state = snapshot.state
    age = snapshot.state_age_s(now)
    if state is None or age is None:
        parts.append(Panel(
            Text(f"waiting for broker… no cell.state received from {sub_endpoint} yet",
                 style="yellow"),
            box=box.ROUNDED, padding=(1, 2)))
        return Group(*parts)

    stale = age > stale_after_s
    parts.append(processes_table(state, stale))
    parts.append(device_row(state, age, stale))
    if snapshot.stats is not None and snapshot.stats_rx is not None:
        # Stats arrive once per window; allow a couple of missed windows.
        window_s = max(snapshot.stats.window_ms / 1000.0, 1.0)
        stats_stale = stale or now - snapshot.stats_rx > 2.0 * window_s + stale_after_s
        parts.append(streams_table(snapshot.stats, stale=stats_stale))
    else:
        parts.append(Panel(Text("waiting for the first cell.stats window…", style="dim"),
                           title="[bold]streams[/bold]", title_align="left", box=box.ROUNDED))
    return Group(*parts)


# ------------------------------------------------------------------ health ---


def health_lines(report: dict[str, Any]) -> list[Text]:
    """Human output of ``cellctl health``: one line per process, then the verdict."""
    lines: list[Text] = []
    processes = report["processes"]
    width = max((len(p["name"]) for p in processes), default=0)
    for proc in processes:
        alive = bool(proc["alive"])
        good = alive and proc["status"] == "OK"
        line = Text.assemble(
            (f"{proc['name']:<{width}}  ", "bold"),
            (f"{proc['status']:<9}", "green" if good else ("yellow" if alive else "red")),
            ("alive" if alive else "DEAD ", "green" if alive else "bold red"),
            f"  up {fmt_duration_ms(proc['uptime_ms']):>8}",
            f"  last seen {proc['last_seen_age_ms']:>5} ms ago",
        )
        if proc["detail"]:
            line.append(f"  {proc['detail']}")
        lines.append(line)
    if report["healthy"]:
        lines.append(Text.assemble(("HEALTHY", "bold green"), f"  {report['reason']}"))
    else:
        lines.append(Text.assemble(("UNHEALTHY", "bold red"), f"  {report['reason']}"))
    return lines
