"""Pure transforms from protobuf telemetry to plain data (JSON output, verdicts)."""
from __future__ import annotations

from collections.abc import Sequence
from typing import Any

from cell_proto import cell_pb2


def process_status_name(status: int) -> str:
    """Enum name without the ``PROCESS_`` prefix: OK, DEGRADED, FAULT, ..."""
    try:
        name = cell_pb2.ProcessStatus.Name(status)
    except ValueError:
        return f"UNKNOWN({status})"
    if name == "PROCESS_STATUS_UNSPECIFIED":
        return "UNSPECIFIED"
    return name.removeprefix("PROCESS_")


def arm_mode_name(mode: int) -> str:
    try:
        name = cell_pb2.ArmMode.Name(mode)
    except ValueError:
        return f"UNKNOWN({mode})"
    if name == "ARM_MODE_UNSPECIFIED":
        return "UNSPECIFIED"
    return name.removeprefix("ARM_")


# ------------------------------------------------------------------ health ---


def unhealthy_reason(state: cell_pb2.CellState) -> str:
    """Why the broker's verdict is 'unhealthy', from the per-process list."""
    problems: list[str] = []
    for proc in state.processes:
        if not proc.alive:
            problems.append(f"{proc.name}: not alive "
                            f"(last heartbeat {proc.last_seen_age_ms} ms ago)")
        elif proc.status != cell_pb2.PROCESS_OK:
            detail = f" ({proc.detail})" if proc.detail else ""
            problems.append(f"{proc.name}: {process_status_name(proc.status)}{detail}")
    if not problems:
        return "broker reports the cell unhealthy"
    return "; ".join(problems)


def health_report(state: cell_pb2.CellState | None, endpoint: str,
                  waited_ms: int) -> dict[str, Any]:
    """The ``cellctl health --json`` document. Keys are a stable interface.

    ``healthy`` is the broker's own verdict (``CellState.healthy``); it is false
    when no state was received at all.
    """
    if state is None:
        return {
            "healthy": False,
            "broker_reachable": False,
            "processes": [],
            "reason": f"no cell.state received from {endpoint} within {waited_ms} ms "
                      "(broker unreachable)",
        }
    processes = [
        {
            "name": proc.name,
            "status": process_status_name(proc.status),
            "alive": proc.alive,
            "uptime_ms": proc.uptime_ms,
            "last_seen_age_ms": proc.last_seen_age_ms,
            "detail": proc.detail,
        }
        for proc in state.processes
    ]
    reason = "all processes alive and OK" if state.healthy else unhealthy_reason(state)
    return {
        "healthy": bool(state.healthy),
        "broker_reachable": True,
        "processes": processes,
        "reason": reason,
    }


# ------------------------------------------------------------------- stats ---


def aggregate_stats(windows: Sequence[cell_pb2.CellStats]) -> cell_pb2.CellStats:
    """Fold consecutive stats windows into one.

    Counts, gaps, drops and bytes are summed and the rate recomputed over the
    combined window. Percentiles cannot be merged exactly without the
    histograms, so p99/p999/max are the worst window's (an upper bound) and p50
    is the count-weighted mean. Totals and the header come from the last window.
    """
    if not windows:
        raise ValueError("no stats windows to aggregate")
    out = cell_pb2.CellStats()
    out.h.CopyFrom(windows[-1].h)
    out.window_ms = sum(w.window_ms for w in windows)
    out.ring_capacity = windows[-1].ring_capacity
    out.ring_high_water = max(w.ring_high_water for w in windows)

    order: list[str] = []
    per_stream: dict[str, list[cell_pb2.StreamStats]] = {}
    for window in windows:
        for stream in window.streams:
            if stream.stream not in per_stream:
                order.append(stream.stream)
                per_stream[stream.stream] = []
            per_stream[stream.stream].append(stream)

    for name in order:
        parts = per_stream[name]
        agg = out.streams.add()
        agg.stream = name
        agg.count = sum(p.count for p in parts)
        agg.rate_hz = agg.count * 1000.0 / out.window_ms if out.window_ms else 0.0
        agg.seq_gaps = sum(p.seq_gaps for p in parts)
        agg.drops = sum(p.drops for p in parts)
        agg.bytes = sum(p.bytes for p in parts)
        if agg.count:
            agg.p50_ns = round(sum(p.p50_ns * p.count for p in parts) / agg.count)
        agg.p99_ns = max(p.p99_ns for p in parts)
        agg.p999_ns = max(p.p999_ns for p in parts)
        agg.max_ns = max(p.max_ns for p in parts)
        agg.total_count = parts[-1].total_count
        agg.total_seq_gaps = parts[-1].total_seq_gaps
        agg.total_drops = parts[-1].total_drops
    return out


def _us(ns: int) -> float:
    return round(ns / 1000.0, 3)


def stats_report(stats: cell_pb2.CellStats, windows: int = 1) -> dict[str, Any]:
    """The ``cellctl stats --json`` document. Latencies are in microseconds."""
    return {
        "window_ms": stats.window_ms,
        "windows": windows,
        "ring_capacity": stats.ring_capacity,
        "ring_high_water": stats.ring_high_water,
        "streams": [
            {
                "stream": s.stream,
                "count": s.count,
                "rate_hz": round(s.rate_hz, 3),
                "p50_us": _us(s.p50_ns),
                "p99_us": _us(s.p99_ns),
                "p999_us": _us(s.p999_ns),
                "max_us": _us(s.max_ns),
                "seq_gaps": s.seq_gaps,
                "drops": s.drops,
                "bytes": s.bytes,
                "total_count": s.total_count,
                "total_seq_gaps": s.total_seq_gaps,
                "total_drops": s.total_drops,
            }
            for s in stats.streams
        ],
    }
