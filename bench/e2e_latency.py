#!/usr/bin/env python3
"""End-to-end latency benchmark: device publish -> broker receive, per stream.

Starts the whole cell with `cellctl up` on private endpoints at 1x, 2x and 5x
the default rates, collects the broker's own CellStats windows (latency =
broker receive CLOCK_MONOTONIC - Header.send_mono_ns) and prints a Markdown
report with machine details.

    PYTHONPATH=python python bench/e2e_latency.py [--seconds 30] [--loads 1,2,5]

Per-window percentiles cannot be merged exactly, so the report gives the median
window and the worst window for each percentile rather than pretending to a
single pooled number.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
NOMINAL = {"arm.joint": 1000, "sensor.frame": 15, "plc.conveyor": 100}


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run_load(load: int, seconds: int, bin_dir: Path) -> dict:
    root = Path(tempfile.mkdtemp(prefix="cellbench-", dir="/tmp"))
    ipc = lambda name: f"ipc://{root}/{name}"  # noqa: E731
    modbus, pub, cmd = free_port(), free_port(), free_port()
    streams = [
        {"topic": "arm.joint", "endpoint": ipc("arm.t"), "process": "arm_sim"},
        {"topic": "sensor.frame", "endpoint": ipc("sen.t"), "process": "sensor_sim"},
        {"topic": "plc.conveyor", "endpoint": ipc("plc.t"), "process": "plc_bridge"},
        {"endpoint": ipc("sim.t"), "process": "plc_sim"},
    ]
    routes = {"arm": ipc("arm.c"), "sensor": ipc("sen.c"), "plc": ipc("plc.c")}
    overrides = [
        f"arm_sim:telemetry_endpoint={ipc('arm.t')}", f"arm_sim:cmd_endpoint={ipc('arm.c')}",
        f"arm_sim:rate_hz={NOMINAL['arm.joint'] * load}",
        f"sensor_sim:frames_endpoint={ipc('sen.t')}", f"sensor_sim:cmd_endpoint={ipc('sen.c')}",
        f"sensor_sim:fps={NOMINAL['sensor.frame'] * load}",
        f"plc_bridge:telemetry_endpoint={ipc('plc.t')}", f"plc_bridge:cmd_endpoint={ipc('plc.c')}",
        f"plc_bridge:modbus.port={modbus}", f"plc_bridge:poll_hz={NOMINAL['plc.conveyor'] * load}",
        f"plc_sim:port={modbus}", f"plc_sim:heartbeat_endpoint={ipc('sim.t')}",
        f"broker:pub_endpoint=tcp://127.0.0.1:{pub}", f"broker:cmd_endpoint=tcp://127.0.0.1:{cmd}",
        f"broker:streams={json.dumps(streams)}", f"broker:routes={json.dumps(routes)}",
    ]
    env = dict(os.environ, PYTHONPATH=str(REPO / "python"))
    ctl = [sys.executable, "-m", "cellctl", "--broker-sub", f"tcp://127.0.0.1:{pub}",
           "--broker-cmd", f"tcp://127.0.0.1:{cmd}"]
    up = [sys.executable, "-m", "cellctl", "up", "--bin-dir", str(bin_dir),
          "--config-dir", str(REPO / "config"), "--run-dir", str(root / "run")]
    for o in overrides:
        up += ["--override", o]
    supervisor = subprocess.Popen(up, cwd=REPO, env=env, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
    try:
        subprocess.run([*ctl, "health", "--wait-healthy-ms", "20000"], cwd=REPO, env=env,
                       check=True, capture_output=True)
        subprocess.run([*ctl, "stats", "--json", "--count", "2"], cwd=REPO, env=env,
                       check=True, capture_output=True)  # discard warm-up windows
        watch = subprocess.run([*ctl, "stats", "--json", "--watch", "--count", str(seconds)],
                               cwd=REPO, env=env, check=True, capture_output=True, text=True)
        windows = [json.loads(line) for line in watch.stdout.splitlines()
                   if line.startswith("{")]
    finally:
        supervisor.send_signal(signal.SIGTERM)
        supervisor.wait(timeout=30)
        shutil.rmtree(root, ignore_errors=True)

    result = {}
    for name in NOMINAL:
        rows = [next(s for s in w["streams"] if s["stream"] == name) for w in windows]
        first, last = rows[0], rows[-1]
        result[name] = {
            "rate_hz": statistics.mean(r["rate_hz"] for r in rows),
            "messages": sum(r["count"] for r in rows),
            "gaps": last["total_seq_gaps"] - first["total_seq_gaps"] + first["seq_gaps"],
            "drops": last["total_drops"] - first["total_drops"] + first["drops"],
            **{f"{p}_med": statistics.median(r[f"{p}_us"] for r in rows)
               for p in ("p50", "p99", "p999")},
            **{f"{p}_worst": max(r[f"{p}_us"] for r in rows) for p in ("p50", "p99", "p999")},
            "max": max(r["max_us"] for r in rows),
        }
    result["ring_high_water"] = max(w["ring_high_water"] for w in windows)
    result["ring_capacity"] = windows[-1]["ring_capacity"]
    result["windows"] = len(windows)
    return result


def machine() -> list[str]:
    cpu = "unknown"
    for line in Path("/proc/cpuinfo").read_text().splitlines():
        if line.startswith("model name"):
            cpu = line.split(":", 1)[1].strip()
            break
    mem_kb = int(Path("/proc/meminfo").read_text().split()[1])
    return [
        f"- CPU: {cpu}, {os.cpu_count()} logical cores",
        f"- Memory: {mem_kb / 1048576:.1f} GiB",
        f"- Kernel: {platform.release()} ({platform.machine()})",
        f"- Load average at start: {os.getloadavg()[0]:.2f}",
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--seconds", type=int, default=30, help="1 s stats windows per load level")
    parser.add_argument("--loads", default="1,2,5", help="comma-separated rate multipliers")
    parser.add_argument("--bin-dir", type=Path,
                        default=Path(os.environ.get("CELL_BIN_DIR", REPO / "build" / "bin")))
    args = parser.parse_args()

    print("### Machine\n")
    print("\n".join(machine()))
    print(f"\n### Results ({args.seconds} one-second windows per load level; latencies in µs)\n")
    print("| Load | Stream | Rate (Hz) | Messages | Gaps | Drops | p50 | p99 | p999 "
          "| worst p99 | worst p999 | max |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    notes = []
    for load in (int(x) for x in args.loads.split(",")):
        r = run_load(load, args.seconds, args.bin_dir)
        for name in NOMINAL:
            s = r[name]
            print(f"| {load}x | `{name}` | {s['rate_hz']:.1f} | {s['messages']:,} | {s['gaps']} "
                  f"| {s['drops']} | {s['p50_med']:.0f} | {s['p99_med']:.0f} | {s['p999_med']:.0f} "
                  f"| {s['p99_worst']:.0f} | {s['p999_worst']:.0f} | {s['max']:.0f} |", flush=True)
        notes.append(f"- {load}x: ring high-water {r['ring_high_water']:,} of "
                     f"{r['ring_capacity']:,} slots")
    print("\np50/p99/p999 are the median across windows; \"worst\" is the worst single window.\n")
    print("\n".join(notes))


if __name__ == "__main__":
    main()
