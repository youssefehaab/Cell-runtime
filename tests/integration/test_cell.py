"""End-to-end tests of the whole cell, driven through the real `cellctl` CLI.

One module-scoped cell (plc_sim, arm_sim, sensor_sim, plc_bridge, broker) is
started with `cellctl up` on private ipc paths and free TCP ports, so the suite
can run next to a live cell or other test runs.

Environment:
  CELL_BIN_DIR      directory with the C++ binaries (default: <repo>/build/bin)
  CELL_TEST_SLOW=1  relax timing tolerances (sanitizer builds, loaded machines)
  CELL_TEST_LOG_DIR copy the per-process logs here on teardown (CI artifacts)
"""

from __future__ import annotations

import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BIN_DIR = Path(os.environ.get("CELL_BIN_DIR", REPO / "build" / "bin"))
SLOW = os.environ.get("CELL_TEST_SLOW", "") not in ("", "0")
SCALE = 4.0 if SLOW else 1.0
PROCESSES = ["arm_sim", "sensor_sim", "plc_bridge", "plc_sim", "broker"]


SANITIZER_MARKERS = ("WARNING: ThreadSanitizer", "ERROR: AddressSanitizer",
                     "ERROR: LeakSanitizer", "runtime error:")


def sanitizer_reports(log_dir: Path) -> list[str]:
    """Sanitizer findings printed by any process of the cell (instrumented builds only)."""
    found = []
    for log_file in sorted(log_dir.glob("*.log")):
        for line in log_file.read_text(errors="replace").splitlines():
            if any(marker in line for marker in SANITIZER_MARKERS):
                found.append(f"{log_file.name}: {line.strip()}")
    return found


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@dataclass
class Cell:
    run_dir: Path
    sub: str
    cmd: str
    supervisor: subprocess.Popen

    def ctl(self, *args: str, timeout: float = 30.0) -> subprocess.CompletedProcess:
        """Run `cellctl <args>` against this cell."""
        env = dict(os.environ, PYTHONPATH=str(REPO / "python"), COLUMNS="200")
        return subprocess.run(
            [sys.executable, "-m", "cellctl", "--broker-sub", self.sub, "--broker-cmd", self.cmd,
             "--set", f"run_dir={self.run_dir}", *args],
            cwd=REPO, env=env, capture_output=True, text=True, timeout=timeout * SCALE)

    def health(self) -> dict:
        out = self.ctl("health", "--json")
        return json.loads(out.stdout)

    def process(self, name: str) -> dict:
        return next(p for p in self.health()["processes"] if p["name"] == name)

    def stats(self, windows: int = 1) -> dict:
        out = self.ctl("stats", "--json", "--count", str(windows), timeout=windows + 10)
        assert out.returncode == 0, out.stderr
        return json.loads(out.stdout)

    def wait_healthy(self, timeout_s: float = 10.0) -> None:
        out = self.ctl("health", "--wait-healthy-ms", str(int(timeout_s * SCALE * 1000)))
        assert out.returncode == 0, f"cell did not become healthy:\n{out.stdout}\n{out.stderr}"

    def wait_for(self, predicate, what: str, timeout_s: float = 10.0):
        deadline = time.monotonic() + timeout_s * SCALE
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.1)
        pytest.fail(f"timed out waiting for {what}")

    def pid(self, process: str) -> int | None:
        """PID from the supervisor's pidfile; None while the process is being restarted."""
        try:
            return int((self.run_dir / f"{process}.pid").read_text().strip())
        except (FileNotFoundError, ValueError):
            return None


@pytest.fixture(scope="module")
def cell():
    for name in ("arm_sim", "sensor_sim", "plc_bridge", "broker"):
        assert (BIN_DIR / name).exists(), f"{BIN_DIR / name} not found - build first"

    # Unix socket paths are limited to ~107 bytes: keep the directory short.
    root = Path(tempfile.mkdtemp(prefix="cell-", dir="/tmp"))
    ipc = lambda name: f"ipc://{root}/{name}"  # noqa: E731
    modbus, pub, cmd = free_port(), free_port(), free_port()
    streams = [
        {"topic": "arm.joint", "endpoint": ipc("arm.telemetry"), "process": "arm_sim"},
        {"topic": "sensor.frame", "endpoint": ipc("sensor.frames"), "process": "sensor_sim"},
        {"topic": "plc.conveyor", "endpoint": ipc("plc.telemetry"), "process": "plc_bridge"},
        {"endpoint": ipc("plcsim.telemetry"), "process": "plc_sim"},
    ]
    routes = {"arm": ipc("arm.cmd"), "sensor": ipc("sensor.cmd"), "plc": ipc("plc.cmd")}
    overrides = [
        f"arm_sim:telemetry_endpoint={ipc('arm.telemetry')}",
        f"arm_sim:cmd_endpoint={ipc('arm.cmd')}",
        f"sensor_sim:frames_endpoint={ipc('sensor.frames')}",
        f"sensor_sim:cmd_endpoint={ipc('sensor.cmd')}",
        f"plc_bridge:telemetry_endpoint={ipc('plc.telemetry')}",
        f"plc_bridge:cmd_endpoint={ipc('plc.cmd')}",
        f"plc_bridge:modbus.port={modbus}",
        f"plc_sim:port={modbus}",
        f"plc_sim:heartbeat_endpoint={ipc('plcsim.telemetry')}",
        f"broker:pub_endpoint=tcp://127.0.0.1:{pub}",
        f"broker:cmd_endpoint=tcp://127.0.0.1:{cmd}",
        f"broker:streams={json.dumps(streams)}",
        f"broker:routes={json.dumps(routes)}",
    ]
    run_dir = root / "run"
    args = [sys.executable, "-m", "cellctl", "up", "--bin-dir", str(BIN_DIR),
            "--config-dir", str(REPO / "config"), "--run-dir", str(run_dir)]
    for o in overrides:
        args += ["--override", o]
    env = dict(os.environ, PYTHONPATH=str(REPO / "python"))
    log = open(root / "supervisor.log", "w")
    supervisor = subprocess.Popen(args, cwd=REPO, env=env, stdout=log, stderr=subprocess.STDOUT)

    c = Cell(run_dir, f"tcp://127.0.0.1:{pub}", f"tcp://127.0.0.1:{cmd}", supervisor)
    try:
        c.wait_healthy(20.0)
        yield c
    finally:
        supervisor.send_signal(signal.SIGTERM)
        try:
            rc = supervisor.wait(timeout=15 * SCALE)
        except subprocess.TimeoutExpired:
            supervisor.kill()
            rc = None
        log.close()
        reports = sanitizer_reports(run_dir / "logs")
        log_dir = os.environ.get("CELL_TEST_LOG_DIR")
        if log_dir:
            dest = Path(log_dir) / root.name
            shutil.copytree(run_dir / "logs", dest, dirs_exist_ok=True)
            shutil.copy(root / "supervisor.log", dest)
        shutil.rmtree(root, ignore_errors=True)
        assert rc == 0, f"cellctl up exited with {rc} instead of a clean 0"
        assert not reports, "sanitizer reports in process logs:\n" + "\n".join(reports)


# ------------------------------------------------------------------ health ---

def test_health_reports_all_processes(cell: Cell):
    out = cell.ctl("health", "--json")
    assert out.returncode == 0
    doc = json.loads(out.stdout)  # stdout is JSON and nothing else
    assert doc["healthy"] is True and doc["broker_reachable"] is True
    assert sorted(p["name"] for p in doc["processes"]) == sorted(PROCESSES)
    assert all(p["alive"] and p["status"] == "OK" for p in doc["processes"])


def test_health_fails_when_broker_unreachable(cell: Cell):
    env = dict(os.environ, PYTHONPATH=str(REPO / "python"))
    out = subprocess.run(
        [sys.executable, "-m", "cellctl", "--broker-sub", f"tcp://127.0.0.1:{free_port()}",
         "health", "--json", "--timeout-ms", "500"],
        cwd=REPO, env=env, capture_output=True, text=True, timeout=20 * SCALE)
    assert out.returncode == 1
    doc = json.loads(out.stdout)
    assert doc["healthy"] is False and doc["broker_reachable"] is False


def test_status_dashboard_renders(cell: Cell):
    out = cell.ctl("status", "--once")
    assert out.returncode == 0, out.stderr
    for text in ("HEALTHY", "arm.joint", "sensor.frame", "plc.conveyor", *PROCESSES):
        assert text in out.stdout


# ------------------------------------------------------------------- stats ---

def test_stats_rates_latency_and_zero_gaps(cell: Cell):
    """Nominal load: default rates, no lost messages anywhere in the pipeline."""
    before = {s["stream"]: s for s in cell.stats()["streams"]}
    windows = 5
    t0 = time.monotonic()
    doc = cell.stats(windows)
    elapsed = time.monotonic() - t0
    assert windows - 1.5 <= elapsed <= (windows + 2.5) * SCALE  # one window per second

    tolerance = 0.25 if SLOW else 0.08
    nominal = {"arm.joint": 1000.0, "sensor.frame": 15.0, "plc.conveyor": 100.0}
    after = {s["stream"]: s for s in doc["streams"]}
    assert set(after) == set(nominal)
    for name, rate in nominal.items():
        s = after[name]
        assert s["rate_hz"] == pytest.approx(rate, rel=tolerance), name
        # Totals over the whole observation: nothing missing, nothing dropped.
        assert s["total_seq_gaps"] == before[name]["total_seq_gaps"] == 0, name
        assert s["total_drops"] == 0, name
        assert s["total_count"] > before[name]["total_count"], name
        assert 0 < s["p50_us"] <= s["p99_us"] <= s["p999_us"], name
        assert s["p50_us"] < (100_000 if SLOW else 20_000), name  # sane: well under 20 ms
    assert doc["ring_high_water"] < doc["ring_capacity"]


# ---------------------------------------------------------------- commands ---

def test_arm_move_estop_reset(cell: Cell):
    out = cell.ctl("cmd", "--json", "arm", "move-to", "0.3", "-0.3", "0.1", "0", "0", "0")
    assert out.returncode == 0, out.stdout + out.stderr
    assert json.loads(out.stdout)["ok"] is True

    assert cell.ctl("cmd", "arm", "estop").returncode == 0
    refused = cell.ctl("cmd", "--json", "arm", "move-to", "0", "0", "0", "0", "0", "0")
    assert refused.returncode == 1
    reply = json.loads(refused.stdout)
    assert reply["ok"] is False and "ESTOP" in reply["error"].upper()

    # Reset is only accepted once the arm is at rest.
    cell.wait_for(lambda: cell.ctl("cmd", "arm", "reset").returncode == 0, "arm reset")
    assert cell.ctl("cmd", "arm", "move-to", "0", "0", "0", "0", "0", "0").returncode == 0


def test_arm_rejects_invalid_target(cell: Cell):
    out = cell.ctl("cmd", "--json", "arm", "move-to", "0.1", "0.2")  # wrong joint count
    assert out.returncode == 1
    assert json.loads(out.stdout)["ok"] is False


def test_plc_speed_command(cell: Cell):
    assert cell.ctl("cmd", "plc", "speed", "0.8").returncode == 0
    too_fast = cell.ctl("cmd", "--json", "plc", "speed", "99")
    assert too_fast.returncode == 1 and json.loads(too_fast.stdout)["ok"] is False
    assert cell.ctl("cmd", "plc", "speed", "0.5").returncode == 0


# ------------------------------------------------------- faults + recovery ---

def test_sensor_disconnect_and_recover(cell: Cell):
    assert cell.ctl("fault", "disconnect-sensor", "--duration-ms", "1500").returncode == 0
    cell.wait_for(lambda: cell.process("sensor_sim")["status"] == "DEGRADED", "sensor DEGRADED")
    assert cell.ctl("health").returncode == 1  # scripts see the outage
    cell.wait_healthy()


def test_plc_outage_and_reconnect(cell: Cell):
    assert cell.ctl("fault", "plc-down", "--duration-ms", "1500").returncode == 0
    cell.wait_for(lambda: cell.process("plc_bridge")["status"] == "DEGRADED", "bridge DEGRADED")
    # While the link is down the bridge refuses commands instead of hanging.
    refused = cell.ctl("cmd", "--json", "plc", "speed", "0.4")
    assert refused.returncode == 1 and json.loads(refused.stdout)["ok"] is False
    cell.wait_healthy(15.0)
    assert cell.ctl("cmd", "plc", "speed", "0.5").returncode == 0


@pytest.mark.parametrize("victim", ["arm_sim", "plc_bridge", "broker"])
def test_killed_process_is_restarted(cell: Cell, victim: str):
    old_pid = cell.pid(victim)
    out = cell.ctl("fault", "kill", victim, "--mode", "local")
    assert out.returncode == 0, out.stdout + out.stderr
    cell.wait_for(lambda: cell.pid(victim) not in (None, old_pid), f"{victim} restart")
    cell.wait_healthy(15.0)
    assert cell.process(victim)["uptime_ms"] < 20_000 * SCALE
    # The cell works again end to end.
    assert cell.ctl("cmd", "arm", "stop").returncode == 0


def test_restart_is_not_counted_as_sequence_gap(cell: Cell):
    """Runs after the kill tests: publisher restarts must not show up as loss."""
    cell.wait_healthy()
    doc = cell.stats(2)
    for s in doc["streams"]:
        assert s["seq_gaps"] == 0, s
