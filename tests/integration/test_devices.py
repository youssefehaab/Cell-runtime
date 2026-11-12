"""Integration tests for the device processes: arm_sim, sensor_sim, plc_bridge, plc_sim.

Each test launches the real binaries (and ``python -m plc_sim``) with endpoints
and ports unique to that test, talks to them over ZeroMQ exactly as the broker
will, and checks rates, sequence continuity, command handling, fault recovery
and clean SIGTERM shutdown.

Environment:
  CELL_BIN_DIR      directory with the C++ binaries (default: <repo>/build/bin)
  CELL_TEST_SLOW=1  relax timing tolerances (sanitizer builds, loaded machines).
                    Functional assertions - sequence continuity, command
                    results, exit codes - are never relaxed.

Run:  PYTHONPATH=python python -m pytest tests/integration/test_devices.py -q
"""
from __future__ import annotations

import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from types import SimpleNamespace

import pytest
import zmq

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "python"))

from cell_proto import cell_pb2  # noqa: E402  (needs the path set up above)

BIN_DIR = Path(os.environ.get("CELL_BIN_DIR", REPO / "build" / "bin"))
if not BIN_DIR.is_absolute():
    BIN_DIR = (Path.cwd() / BIN_DIR).resolve()
CONFIG_DIR = REPO / "config"

SLOW = os.environ.get("CELL_TEST_SLOW", "") not in ("", "0")

# Timing tolerances: (default, CELL_TEST_SLOW).
RATE_TOL_ARM = 0.40 if SLOW else 0.05  # 1 kHz
RATE_TOL_PLC = 0.40 if SLOW else 0.10  # 100 Hz, two Modbus round trips per poll
RATE_TOL_SENSOR = 0.40 if SLOW else 0.05  # 15 fps
SHUTDOWN_S = 5.0 if SLOW else 1.0  # SIGTERM -> exit
STARTUP_S = 30.0 if SLOW else 10.0  # launch -> first message
EVENT_S = 30.0 if SLOW else 10.0  # generic "this should happen soon" bound

TOPIC_TYPES = {
    b"arm.joint": cell_pb2.JointState,
    b"sensor.frame": cell_pb2.PointCloudFrame,
    b"plc.conveyor": cell_pb2.ConveyorState,
    b"hb": cell_pb2.Heartbeat,
}

SANITIZER_MARKERS = ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:")


# ------------------------------------------------------------------ helpers ---


def free_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Proc:
    """A launched process; stderr goes to a per-process log file."""

    def __init__(self, name: str, argv: list[str], log_path: Path, env: dict[str, str]):
        self.name = name
        self.log_path = log_path
        self._log = open(log_path, "ab")
        self.popen = subprocess.Popen(argv, stdout=self._log, stderr=self._log, env=env,
                                      cwd=str(REPO))

    def log_text(self) -> str:
        self._log.flush()
        return self.log_path.read_text(errors="replace")

    def assert_running(self) -> None:
        assert self.popen.poll() is None, (
            f"{self.name} exited early with {self.popen.returncode}:\n{self.log_text()}")

    def terminate(self, sig: int = signal.SIGTERM) -> tuple[int, float]:
        """Send `sig`, wait for exit. Returns (exit code, seconds it took)."""
        self.assert_running()
        t0 = time.monotonic()
        self.popen.send_signal(sig)
        try:
            code = self.popen.wait(timeout=SHUTDOWN_S + 10)
        except subprocess.TimeoutExpired:
            self.popen.kill()
            self.popen.wait()
            pytest.fail(f"{self.name} did not exit after signal {sig}:\n{self.log_text()}")
        return code, time.monotonic() - t0

    def stop_cleanly(self, sig: int = signal.SIGTERM) -> None:
        """Signal the process and assert a prompt, clean, sanitizer-clean exit."""
        code, took = self.terminate(sig)
        text = self.log_text()
        assert code == 0, f"{self.name} exit code {code}:\n{text}"
        assert took <= SHUTDOWN_S, f"{self.name} took {took:.2f}s to exit:\n{text}"
        for marker in SANITIZER_MARKERS:
            assert marker not in text, f"{self.name} sanitizer report:\n{text}"

    def kill(self) -> None:
        if self.popen.poll() is None:
            self.popen.kill()
            self.popen.wait()
        self._log.close()


class Launcher:
    """Starts processes for one test and guarantees they are gone afterwards.

    Only PIDs started here are ever signalled.
    """

    def __init__(self, tmp_path: Path):
        self.tmp_path = tmp_path
        # ipc:// endpoints are unix socket paths, limited to ~107 bytes. pytest's
        # tmp_path is used when it is short enough, else a short private dir.
        self.ipc_dir = tmp_path
        self._own_ipc_dir = len(str(tmp_path)) > 70
        if self._own_ipc_dir:
            self.ipc_dir = Path(tempfile.mkdtemp(prefix="cell-it-"))
        self.procs: list[Proc] = []
        self._count = 0

    def ipc(self, name: str) -> str:
        return f"ipc://{self.ipc_dir}/{name}"

    def _start(self, name: str, argv: list[str], overrides: dict[str, object]) -> Proc:
        for key, value in overrides.items():
            argv += ["--set", f"{key}={value}"]
        env = dict(os.environ)
        env["PYTHONPATH"] = f"{REPO / 'python'}{os.pathsep}{env.get('PYTHONPATH', '')}"
        self._count += 1
        proc = Proc(name, argv, self.tmp_path / f"{name}.{self._count}.log", env)
        self.procs.append(proc)
        return proc

    def cpp(self, name: str, **overrides: object) -> Proc:
        binary = BIN_DIR / name
        assert binary.exists(), f"{binary} not found - build first or set CELL_BIN_DIR"
        return self._start(name, [str(binary), "--config", str(CONFIG_DIR / f"{name}.json")],
                           overrides)

    def plc_sim(self, **overrides: object) -> Proc:
        argv = [sys.executable, "-m", "plc_sim", "--config", str(CONFIG_DIR / "plc_sim.json")]
        return self._start("plc_sim", argv, overrides)

    def close(self) -> None:
        for proc in self.procs:
            proc.kill()
        if self._own_ipc_dir:
            shutil.rmtree(self.ipc_dir, ignore_errors=True)


@pytest.fixture
def launcher(tmp_path: Path):
    lch = Launcher(tmp_path)
    yield lch
    lch.close()


@pytest.fixture
def zctx():
    ctx = zmq.Context()
    yield ctx
    ctx.destroy(linger=0)


class Stream:
    """SUB socket that accumulates decoded messages per topic.

    Point-cloud payloads are reduced to their size so a few seconds of frames
    do not hold hundreds of megabytes.
    """

    def __init__(self, ctx: zmq.Context, *endpoints: str):
        self.sub = ctx.socket(zmq.SUB)
        self.sub.setsockopt(zmq.LINGER, 0)
        self.sub.setsockopt(zmq.RCVHWM, 100_000)
        self.sub.setsockopt(zmq.SUBSCRIBE, b"")
        for endpoint in endpoints:
            self.sub.connect(endpoint)
        self.msgs: dict[bytes, list] = {topic: [] for topic in TOPIC_TYPES}

    def _receive_one(self) -> None:
        frames = self.sub.recv_multipart()
        assert len(frames) == 2, f"expected [topic][payload], got {len(frames)} frames"
        topic, payload = frames
        assert topic in TOPIC_TYPES, f"unexpected topic {topic!r}"
        msg = TOPIC_TYPES[topic]()
        msg.ParseFromString(payload)
        if topic == b"sensor.frame":
            msg = SimpleNamespace(h=msg.h, width=msg.width, height=msg.height,
                                  nbytes=len(msg.points), head=msg.points[:12 * 64])
        self.msgs[topic].append(msg)

    def pump(self, seconds: float = 0.0, until=None, timeout: float = EVENT_S) -> bool:
        """Receive for `seconds`, or until `until()` is true (False on timeout)."""
        end = time.monotonic() + (seconds if until is None else timeout)
        while True:
            if until is not None and until():
                return True
            remaining = end - time.monotonic()
            if remaining <= 0:
                return until is None
            if self.sub.poll(min(remaining, 0.05) * 1000):
                self._receive_one()

    def mark(self) -> dict[bytes, int]:
        """Current message counts, for looking only at what arrives afterwards."""
        return {topic: len(msgs) for topic, msgs in self.msgs.items()}

    def heartbeats(self, source: str, since: dict[bytes, int] | None = None) -> list:
        start = since[b"hb"] if since else 0
        return [m for m in self.msgs[b"hb"][start:] if m.h.source == source]


def assert_contiguous(msgs: list, what: str) -> None:
    assert msgs, f"no {what} messages"
    seqs = [m.h.seq for m in msgs]
    expected = list(range(seqs[0], seqs[0] + len(seqs)))
    if seqs != expected:
        bad = next(i for i, (a, b) in enumerate(zip(seqs, expected)) if a != b)
        pytest.fail(f"{what}: sequence gap at index {bad}: {seqs[max(0, bad - 2):bad + 3]}")


def rate_hz(msgs: list) -> float:
    """Message rate from the senders' own timestamps (immune to receive jitter)."""
    assert len(msgs) >= 2
    span_ns = msgs[-1].h.send_mono_ns - msgs[0].h.send_mono_ns
    return (len(msgs) - 1) / (span_ns / 1e9)


def assert_rate(msgs: list, nominal: float, tolerance: float, what: str) -> None:
    rate = rate_hz(msgs)
    assert abs(rate - nominal) <= nominal * tolerance, (
        f"{what}: {rate:.2f} Hz, expected {nominal} Hz +/- {tolerance:.0%}")


class Commander:
    """REQ client for a device command socket."""

    def __init__(self, ctx: zmq.Context, endpoint: str):
        self.req = ctx.socket(zmq.REQ)
        self.req.setsockopt(zmq.LINGER, 0)
        self.req.setsockopt(zmq.REQ_RELAXED, 1)
        self.req.setsockopt(zmq.REQ_CORRELATE, 1)
        self.req.connect(endpoint)
        self.seq = 0

    def send_raw(self, payload: bytes, timeout: float = EVENT_S) -> cell_pb2.CommandReply:
        self.req.send(payload)
        assert self.req.poll(timeout * 1000), "no reply to command (REP socket stuck?)"
        frames = self.req.recv_multipart()
        assert len(frames) == 1
        reply = cell_pb2.CommandReply()
        reply.ParseFromString(frames[0])
        return reply

    def send(self, target: str, **action) -> cell_pb2.CommandReply:
        cmd = cell_pb2.Command(target=target, **action)
        cmd.h.seq = self.seq
        cmd.h.source = "test"
        cmd.h.send_mono_ns = time.monotonic_ns()
        self.seq += 1
        return self.send_raw(cmd.SerializeToString())


def wait_first(stream: Stream, topic: bytes, proc: Proc) -> None:
    ok = stream.pump(until=lambda: len(stream.msgs[topic]) > 0, timeout=STARTUP_S)
    proc.assert_running()
    assert ok, f"no {topic!r} message from {proc.name}:\n{proc.log_text()}"


def check_heartbeats(beats: list, source: str, interval_ms: int = 200) -> None:
    assert len(beats) >= 2, f"too few heartbeats from {source}"
    assert_contiguous(beats, f"{source} heartbeat")
    assert all(b.h.source == source for b in beats)
    uptimes = [b.uptime_ms for b in beats]
    assert uptimes == sorted(uptimes)
    # Mean interval near the configured one (transition beats may add extras).
    span_ms = (beats[-1].h.send_mono_ns - beats[0].h.send_mono_ns) / 1e6
    mean = span_ms / (len(beats) - 1)
    assert mean <= interval_ms * (2.0 if SLOW else 1.3), f"{source} heartbeat every {mean:.0f} ms"


# ------------------------------------------------------------------ arm_sim ---


@pytest.fixture
def arm(launcher: Launcher, zctx: zmq.Context):
    def start(**overrides):
        telemetry, cmd = launcher.ipc("arm.telemetry"), launcher.ipc("arm.cmd")
        proc = launcher.cpp("arm_sim", telemetry_endpoint=telemetry, cmd_endpoint=cmd,
                            **overrides)
        stream = Stream(zctx, telemetry)
        wait_first(stream, b"arm.joint", proc)
        return SimpleNamespace(proc=proc, stream=stream, cmd=Commander(zctx, cmd))

    return start


def test_arm_telemetry_rate_seq_heartbeat_and_shutdown(arm):
    a = arm()
    start = a.stream.mark()
    a.stream.pump(2.0)
    joints = a.stream.msgs[b"arm.joint"][start[b"arm.joint"]:]

    assert_rate(joints, 1000.0, RATE_TOL_ARM, "arm.joint")
    assert_contiguous(a.stream.msgs[b"arm.joint"], "arm.joint")
    for m in joints[:: max(1, len(joints) // 50)]:
        assert m.h.source == "arm_sim"
        assert len(m.position) == 6 and len(m.velocity) == 6
        assert m.mode == cell_pb2.ARM_IDLE
        assert list(m.position) == [0.0] * 6 and list(m.velocity) == [0.0] * 6
    # send_mono_ns is CLOCK_MONOTONIC on this host: recent and never decreasing.
    stamps = [m.h.send_mono_ns for m in joints]
    assert stamps == sorted(stamps)
    assert 0 <= time.monotonic_ns() - stamps[-1] < 5e9

    beats = a.stream.heartbeats("arm_sim")
    check_heartbeats(beats, "arm_sim")
    assert all(b.status == cell_pb2.PROCESS_OK for b in beats)

    a.proc.stop_cleanly()
    a.stream.pump(0.2)
    assert_contiguous(a.stream.msgs[b"arm.joint"], "arm.joint (through shutdown)")
    assert "stopped cleanly" in a.proc.log_text()


def test_arm_exits_cleanly_on_sigint(arm):
    a = arm()
    a.proc.stop_cleanly(signal.SIGINT)


def test_arm_move_to_reaches_target_and_returns_to_idle(arm):
    a = arm()
    target = [0.4, -0.3, 0.2, 0.1, -0.2, 0.05]
    mark = a.stream.mark()
    reply = a.cmd.send("arm", move_to=cell_pb2.MoveTo(joints=target))
    assert reply.ok, reply.error
    assert reply.h.source == "arm_sim"

    joints = a.stream.msgs[b"arm.joint"]
    moving = lambda: any(m.mode == cell_pb2.ARM_MOVING for m in joints[mark[b"arm.joint"]:])
    assert a.stream.pump(until=moving), "arm never reported MOVING"
    arrived = lambda: joints[-1].mode == cell_pb2.ARM_IDLE
    assert a.stream.pump(until=arrived), "arm never returned to IDLE"

    final = joints[-1]
    assert list(final.position) == target  # exact landing, not approximate
    assert list(final.velocity) == [0.0] * 6

    # Limits from config/arm_sim.json held on every published sample.
    move = joints[mark[b"arm.joint"]:]
    assert max(abs(v) for m in move for v in m.velocity) <= 1.5 + 1e-9
    for prev, cur in zip(move, move[1:]):
        for v0, v1 in zip(prev.velocity, cur.velocity):
            assert abs(v1 - v0) <= 4.0 * 0.001 + 1e-9
    # Joint 0 never overshoots its target.
    assert max(m.position[0] for m in move) <= target[0]
    assert_contiguous(joints, "arm.joint")
    a.proc.stop_cleanly()


def test_arm_command_validation_estop_latch_and_reset(arm):
    # A slow e-stop deceleration keeps the arm decelerating for ~1.5 s, so the
    # "reset while still moving" refusal is deterministic.
    a = arm(estop_deceleration=1.0)
    joints = a.stream.msgs[b"arm.joint"]

    reply = a.cmd.send("arm", move_to=cell_pb2.MoveTo(joints=[0.1, 0.2]))
    assert not reply.ok and "joints" in reply.error
    reply = a.cmd.send("arm", move_to=cell_pb2.MoveTo(joints=[0, 0, 0, 0, 0, 9.0]))
    assert not reply.ok and "limit" in reply.error
    reply = a.cmd.send("arm", set_conveyor_speed=cell_pb2.SetConveyorSpeed(speed=0.5))
    assert not reply.ok and "unsupported" in reply.error
    reply = a.cmd.send("arm", fault_inject=cell_pb2.FaultInject(kind=1))
    assert not reply.ok and "unsupported" in reply.error
    reply = a.cmd.send("arm")  # no action set
    assert not reply.ok and reply.error
    reply = a.cmd.send_raw(b"\xff\xff\xff not a protobuf")
    assert not reply.ok and "malformed" in reply.error
    a.stream.pump(0.1)
    assert joints[-1].mode == cell_pb2.ARM_IDLE  # none of that moved the arm

    assert a.cmd.send("arm", move_to=cell_pb2.MoveTo(joints=[2.0, 0, 0, 0, 0, 0])).ok
    at_speed = lambda: joints[-1].velocity[0] >= 1.5
    assert a.stream.pump(until=at_speed)

    assert a.cmd.send("arm", estop=cell_pb2.EStop()).ok
    reply = a.cmd.send("arm", reset=cell_pb2.Reset())
    assert not reply.ok and "decelerating" in reply.error
    reply = a.cmd.send("arm", move_to=cell_pb2.MoveTo(joints=[0, 0, 0, 0, 0, 0]))
    assert not reply.ok and "ESTOP" in reply.error

    at_rest = lambda: joints[-1].mode == cell_pb2.ARM_ESTOP and joints[-1].velocity[0] == 0.0
    assert a.stream.pump(until=at_rest)
    rest_position = joints[-1].position[0]
    assert rest_position < 2.0
    a.stream.pump(0.3)
    assert joints[-1].mode == cell_pb2.ARM_ESTOP  # latched while at rest
    assert joints[-1].position[0] == rest_position

    assert a.cmd.send("arm", reset=cell_pb2.Reset()).ok
    idle = lambda: joints[-1].mode == cell_pb2.ARM_IDLE
    assert a.stream.pump(until=idle)
    a.stream.pump(0.2)
    assert joints[-1].position[0] == rest_position  # the old target is gone

    # Stop: decelerate to rest, back to IDLE, short of the target.
    assert a.cmd.send("arm", move_to=cell_pb2.MoveTo(joints=[-2.0, 0, 0, 0, 0, 0])).ok
    assert a.stream.pump(until=lambda: joints[-1].velocity[0] <= -1.5)
    mark = a.stream.mark()
    assert a.cmd.send("arm", stop=cell_pb2.Stop()).ok
    assert a.stream.pump(until=lambda: joints[-1].mode == cell_pb2.ARM_STOPPING)
    assert a.stream.pump(until=idle)
    assert joints[-1].velocity[0] == 0.0 and joints[-1].position[0] > -2.0
    modes = {m.mode for m in joints[mark[b"arm.joint"]:]}
    assert cell_pb2.ARM_STOPPING in modes

    assert_contiguous(joints, "arm.joint")
    assert all(b.status == cell_pb2.PROCESS_OK for b in a.stream.heartbeats("arm_sim"))
    a.proc.stop_cleanly()


# --------------------------------------------------------------- sensor_sim ---


@pytest.fixture
def sensor(launcher: Launcher, zctx: zmq.Context):
    def start(**overrides):
        frames, cmd = launcher.ipc("sensor.frames"), launcher.ipc("sensor.cmd")
        proc = launcher.cpp("sensor_sim", frames_endpoint=frames, cmd_endpoint=cmd, **overrides)
        stream = Stream(zctx, frames)
        wait_first(stream, b"sensor.frame", proc)
        return SimpleNamespace(proc=proc, stream=stream, cmd=Commander(zctx, cmd))

    return start


def test_sensor_frames_rate_size_seq_and_shutdown(sensor):
    s = sensor()
    start = s.stream.mark()
    s.stream.pump(2.5)
    frames = s.stream.msgs[b"sensor.frame"][start[b"sensor.frame"]:]

    assert_rate(frames, 15.0, RATE_TOL_SENSOR, "sensor.frame")
    assert_contiguous(s.stream.msgs[b"sensor.frame"], "sensor.frame")
    for f in frames:
        assert f.h.source == "sensor_sim"
        assert (f.width, f.height) == (640, 480)
        assert f.nbytes == 640 * 480 * 12 == 3_686_400
    # The scene changes from frame to frame.
    assert frames[0].head != frames[1].head

    beats = s.stream.heartbeats("sensor_sim")
    check_heartbeats(beats, "sensor_sim")
    assert all(b.status == cell_pb2.PROCESS_OK for b in beats)

    s.proc.stop_cleanly()
    assert "stopped cleanly" in s.proc.log_text()


def test_sensor_disconnect_and_reconnect(sensor):
    # Small frames: this test is about the fault state machine, not throughput.
    s = sensor(width=64, height=48, fps=30)
    frames = s.stream.msgs[b"sensor.frame"]
    s.stream.pump(0.3)

    # --- timed disconnect: silent for duration_ms, then resumes on its own ---
    duration_ms = 700
    mark = s.stream.mark()
    reply = s.cmd.send("sensor", fault_inject=cell_pb2.FaultInject(kind=1, duration_ms=duration_ms))
    assert reply.ok, reply.error
    assert reply.h.source == "sensor_sim"

    degraded = lambda: any(b.status == cell_pb2.PROCESS_DEGRADED
                           for b in s.stream.heartbeats("sensor_sim", mark))
    assert s.stream.pump(until=degraded), "no DEGRADED heartbeat after disconnect"
    beat = next(b for b in s.stream.heartbeats("sensor_sim", mark)
                if b.status == cell_pb2.PROCESS_DEGRADED)
    assert "disconnect" in beat.detail

    count_at_fault = len(frames)
    resumed = lambda: len(frames) > count_at_fault + 3
    assert s.stream.pump(until=resumed), "frames did not resume after timed disconnect"
    # The gap between the frames either side of the fault is about duration_ms.
    # (Start one frame before the command: the fault can land before any more arrive.)
    stamps = [f.h.send_mono_ns for f in frames[mark[b"sensor.frame"] - 1:]]
    longest_gap_ms = max(b - a for a, b in zip(stamps, stamps[1:])) / 1e6
    assert duration_ms - 50 <= longest_gap_ms <= duration_ms + (1000 if SLOW else 150), (
        f"frame gap {longest_gap_ms:.0f} ms for a {duration_ms} ms disconnect")
    ok_again = lambda: s.stream.heartbeats("sensor_sim")[-1].status == cell_pb2.PROCESS_OK
    assert s.stream.pump(until=ok_again)
    assert s.stream.heartbeats("sensor_sim")[-1].detail == ""

    # --- indefinite disconnect (duration 0) until RECONNECT ---
    assert s.cmd.send("sensor", fault_inject=cell_pb2.FaultInject(kind=1, duration_ms=0)).ok
    s.stream.pump(0.2)  # let frames already in flight arrive
    count = len(frames)
    s.stream.pump(1.0)
    assert len(frames) == count, "frames published while disconnected"
    recent = s.stream.heartbeats("sensor_sim")[-3:]
    assert all(b.status == cell_pb2.PROCESS_DEGRADED for b in recent)  # heartbeats continue

    assert s.cmd.send("sensor", fault_inject=cell_pb2.FaultInject(kind=2)).ok
    assert s.stream.pump(until=lambda: len(frames) > count + 3), "no frames after RECONNECT"
    assert s.stream.pump(until=ok_again)

    # Sequence numbers carry on across both outages without a gap.
    assert_contiguous(frames, "sensor.frame")
    check_heartbeats(s.stream.heartbeats("sensor_sim"), "sensor_sim")

    # --- everything else is refused, but always answered ---
    reply = s.cmd.send("sensor", fault_inject=cell_pb2.FaultInject(kind=0))
    assert not reply.ok and "DISCONNECT" in reply.error
    reply = s.cmd.send("sensor", stop=cell_pb2.Stop())
    assert not reply.ok and "unsupported" in reply.error
    reply = s.cmd.send("sensor", move_to=cell_pb2.MoveTo(joints=[0.0]))
    assert not reply.ok
    reply = s.cmd.send_raw(b"\xff\xff garbage")
    assert not reply.ok and "malformed" in reply.error

    s.proc.stop_cleanly()


def test_sensor_shuts_down_promptly_while_disconnected(sensor):
    s = sensor(width=64, height=48)
    assert s.cmd.send("sensor", fault_inject=cell_pb2.FaultInject(kind=1, duration_ms=0)).ok
    s.stream.pump(0.3)
    s.proc.stop_cleanly()


# ------------------------------------------------------ plc_sim + plc_bridge ---


class PlcRig:
    """plc_sim + plc_bridge wired together on a private port and ipc paths."""

    def __init__(self, launcher: Launcher, zctx: zmq.Context, sim_overrides=None,
                 bridge_overrides=None):
        self.launcher = launcher
        self.port = free_tcp_port()
        self.telemetry = launcher.ipc("plc.telemetry")
        self.sim_hb = launcher.ipc("plcsim.telemetry")
        self.sim_overrides = {"host": "127.0.0.1", "port": self.port,
                              "heartbeat_endpoint": self.sim_hb, **(sim_overrides or {})}
        self.bridge_overrides = {
            "telemetry_endpoint": self.telemetry, "cmd_endpoint": launcher.ipc("plc.cmd"),
            "modbus.port": self.port,
            # Keep reconnects quick so outage tests stay short.
            "backoff.initial_ms": 50, "backoff.max_ms": 400,
            **(bridge_overrides or {})}
        self.stream = Stream(zctx, self.telemetry, self.sim_hb)
        self.cmd = Commander(zctx, self.bridge_overrides["cmd_endpoint"])
        self.conveyor = self.stream.msgs[b"plc.conveyor"]
        self.sim: Proc | None = None
        self.bridge: Proc | None = None

    def start_sim(self) -> Proc:
        self.sim = self.launcher.plc_sim(**self.sim_overrides)
        return self.sim

    def start_bridge(self) -> Proc:
        self.bridge = self.launcher.cpp("plc_bridge", **self.bridge_overrides)
        wait_first(self.stream, b"plc.conveyor", self.bridge)
        return self.bridge

    def wait_link(self, up: bool) -> None:
        ok = self.stream.pump(until=lambda: bool(self.conveyor) and self.conveyor[-1].link_up == up)
        if self.sim is not None and up:
            self.sim.assert_running()
        assert ok, (f"link_up never became {up}\n--- bridge ---\n{self.bridge.log_text()}"
                    f"\n--- plc_sim ---\n{self.sim.log_text() if self.sim else ''}")

    def wait_bridge_status(self, status: int) -> None:
        beats = lambda: self.stream.heartbeats("plc_bridge")
        assert self.stream.pump(until=lambda: bool(beats()) and beats()[-1].status == status), (
            f"plc_bridge heartbeat never reached status {status}")


@pytest.fixture
def plc(launcher: Launcher, zctx: zmq.Context):
    def make(**kwargs) -> PlcRig:
        return PlcRig(launcher, zctx, **kwargs)

    return make


def test_plc_telemetry_commands_and_shutdown(plc):
    rig = plc(sim_overrides={"ramp_per_s": 4.0, "item_period_s": 0.4})
    rig.start_sim()
    rig.start_bridge()
    rig.wait_link(True)
    rig.wait_bridge_status(cell_pb2.PROCESS_OK)

    mark = rig.stream.mark()
    rig.stream.pump(2.0)
    window = rig.conveyor[mark[b"plc.conveyor"]:]
    assert_rate(window, 100.0, RATE_TOL_PLC, "plc.conveyor")
    assert all(m.link_up and m.h.source == "plc_bridge" for m in window)
    assert all(m.speed == pytest.approx(0.5, abs=1e-3) for m in window)
    assert all(m.fault_code == 0 for m in window)
    # The photo-eye toggles as items pass (0.4 s period here).
    assert {m.item_present for m in window} == {True, False}

    # Speed command: accepted, then visible in telemetry as the belt ramps.
    reply = rig.cmd.send("plc", set_conveyor_speed=cell_pb2.SetConveyorSpeed(speed=1.25))
    assert reply.ok, reply.error
    assert reply.h.source == "plc_bridge"
    at_speed = lambda: rig.conveyor[-1].speed == pytest.approx(1.25, abs=1e-3)
    assert rig.stream.pump(until=at_speed), f"speed stuck at {rig.conveyor[-1].speed}"
    ramp = [m.speed for m in rig.conveyor[mark[b"plc.conveyor"]:]]
    assert any(0.55 < v < 1.2 for v in ramp), "speed jumped instead of ramping"

    assert rig.cmd.send("plc", set_conveyor_speed=cell_pb2.SetConveyorSpeed(speed=0.0)).ok
    assert rig.stream.pump(until=lambda: rig.conveyor[-1].speed == 0.0)
    # Belt stopped: no new items, the photo-eye holds its state.
    mark = rig.stream.mark()
    rig.stream.pump(0.8)
    assert len({m.item_present for m in rig.conveyor[mark[b"plc.conveyor"]:]}) == 1

    # Validation and unsupported actions: refused, but always answered.
    reply = rig.cmd.send("plc", set_conveyor_speed=cell_pb2.SetConveyorSpeed(speed=2.5))
    assert not reply.ok and "outside" in reply.error
    reply = rig.cmd.send("plc", set_conveyor_speed=cell_pb2.SetConveyorSpeed(speed=-0.1))
    assert not reply.ok and "outside" in reply.error
    reply = rig.cmd.send("plc", set_conveyor_speed=cell_pb2.SetConveyorSpeed(speed=float("nan")))
    assert not reply.ok
    reply = rig.cmd.send("plc", estop=cell_pb2.EStop())
    assert not reply.ok and "unsupported" in reply.error
    reply = rig.cmd.send("plc", fault_inject=cell_pb2.FaultInject(kind=2))
    assert not reply.ok
    reply = rig.cmd.send_raw(b"\xff\xff garbage")
    assert not reply.ok and "malformed" in reply.error
    rig.stream.pump(0.2)
    assert rig.conveyor[-1].link_up and rig.conveyor[-1].speed == 0.0

    assert_contiguous(rig.conveyor, "plc.conveyor")
    check_heartbeats(rig.stream.heartbeats("plc_bridge"), "plc_bridge")
    sim_beats = rig.stream.heartbeats("plc_sim")
    check_heartbeats(sim_beats, "plc_sim")
    assert all(b.status == cell_pb2.PROCESS_OK for b in sim_beats)
    assert 0 <= time.monotonic_ns() - sim_beats[-1].h.send_mono_ns < 5e9  # same clock as C++

    rig.bridge.stop_cleanly()
    assert "stopped cleanly" in rig.bridge.log_text()
    rig.sim.stop_cleanly()
    assert "exiting with code 0" in rig.sim.log_text()


def test_plc_sim_exits_cleanly_on_sigint(plc):
    rig = plc()
    sim = rig.start_sim()
    beats = lambda: rig.stream.heartbeats("plc_sim")
    assert rig.stream.pump(until=lambda: len(beats()) >= 2, timeout=STARTUP_S), sim.log_text()
    assert beats()[0].h.seq == 0 or beats()[0].h.seq < 5  # starts from 0 (we may join late)
    sim.stop_cleanly(signal.SIGINT)


def test_plc_bridge_survives_plc_sim_restart(plc):
    rig = plc()

    # Bridge first, PLC absent: stream is up, link is down, status DEGRADED.
    rig.start_bridge()
    rig.stream.pump(0.5)
    assert not rig.conveyor[-1].link_up
    rig.wait_bridge_status(cell_pb2.PROCESS_DEGRADED)
    assert "modbus link down" in rig.stream.heartbeats("plc_bridge")[-1].detail
    reply = rig.cmd.send("plc", set_conveyor_speed=cell_pb2.SetConveyorSpeed(speed=1.0))
    assert not reply.ok and "link is down" in reply.error

    # PLC appears: the bridge finds it by itself.
    first_sim = rig.start_sim()
    rig.wait_link(True)
    rig.wait_bridge_status(cell_pb2.PROCESS_OK)
    assert rig.stream.heartbeats("plc_bridge")[-1].detail == ""
    assert rig.cmd.send("plc", set_conveyor_speed=cell_pb2.SetConveyorSpeed(speed=1.0)).ok
    assert rig.stream.pump(until=lambda: rig.conveyor[-1].speed == pytest.approx(1.0, abs=1e-3))

    # Kill the PLC: link down + DEGRADED, last known values held, still publishing.
    first_sim.stop_cleanly()
    rig.wait_link(False)
    rig.wait_bridge_status(cell_pb2.PROCESS_DEGRADED)
    mark = rig.stream.mark()
    rig.stream.pump(1.0)
    down = rig.conveyor[mark[b"plc.conveyor"]:]
    assert len(down) >= (20 if SLOW else 60), "stream stalled while the link was down"
    assert all(not m.link_up for m in down)
    assert all(m.speed == pytest.approx(1.0, abs=1e-3) for m in down)  # last known value
    reply = rig.cmd.send("plc", fault_inject=cell_pb2.FaultInject(kind=1, duration_ms=100))
    assert not reply.ok and "link is down" in reply.error
    rig.bridge.assert_running()

    # Restart it: link comes back, status OK, fresh values (new process -> 0.5).
    rig.start_sim()
    rig.wait_link(True)
    rig.wait_bridge_status(cell_pb2.PROCESS_OK)
    assert rig.stream.pump(until=lambda: rig.conveyor[-1].speed == pytest.approx(0.5, abs=1e-3))

    # No sequence gap anywhere across down -> up -> down -> up.
    assert_contiguous(rig.conveyor, "plc.conveyor")
    assert_contiguous(rig.stream.heartbeats("plc_bridge"), "plc_bridge heartbeat")
    log_text = rig.bridge.log_text()
    assert log_text.count("modbus link up") == 2
    assert log_text.count("modbus link down") == 2  # once per outage, not per retry

    rig.bridge.stop_cleanly()
    rig.sim.stop_cleanly()


def test_plc_fault_inject_outage_and_recovery(plc):
    rig = plc()
    rig.start_sim()
    rig.start_bridge()
    rig.wait_link(True)
    rig.wait_bridge_status(cell_pb2.PROCESS_OK)

    outage_ms = 1000
    mark = rig.stream.mark()
    reply = rig.cmd.send("plc", fault_inject=cell_pb2.FaultInject(kind=1, duration_ms=outage_ms))
    assert reply.ok, reply.error

    # The bridge was connected; it only sees link-down if plc_sim really
    # dropped the established connection, not just its listening socket.
    rig.wait_link(False)
    t_down = rig.conveyor[-1].h.send_mono_ns
    rig.wait_bridge_status(cell_pb2.PROCESS_DEGRADED)
    sim_degraded = lambda: any(b.status == cell_pb2.PROCESS_DEGRADED
                               for b in rig.stream.heartbeats("plc_sim", mark))
    assert rig.stream.pump(until=sim_degraded), "plc_sim heartbeat not DEGRADED during outage"
    rig.sim.assert_running()  # off the network, but the process is alive

    rig.wait_link(True)
    down_ms = (rig.conveyor[-1].h.send_mono_ns - t_down) / 1e6
    assert outage_ms - 150 <= down_ms <= outage_ms + (5000 if SLOW else 1200), (
        f"link was down for {down_ms:.0f} ms after a {outage_ms} ms outage request")
    rig.wait_bridge_status(cell_pb2.PROCESS_OK)
    sim_ok = lambda: rig.stream.heartbeats("plc_sim")[-1].status == cell_pb2.PROCESS_OK
    assert rig.stream.pump(until=sim_ok)

    # Fully functional again, and the outage register cleared itself (a stuck
    # register would immediately trigger another outage).
    assert rig.cmd.send("plc", set_conveyor_speed=cell_pb2.SetConveyorSpeed(speed=0.8)).ok
    assert rig.stream.pump(until=lambda: rig.conveyor[-1].speed == pytest.approx(0.8, abs=1e-3))
    mark = rig.stream.mark()
    rig.stream.pump(0.5)
    assert all(m.link_up for m in rig.conveyor[mark[b"plc.conveyor"]:])

    assert_contiguous(rig.conveyor, "plc.conveyor")
    assert_contiguous(rig.stream.heartbeats("plc_sim"), "plc_sim heartbeat")
    assert_contiguous(rig.stream.heartbeats("plc_bridge"), "plc_bridge heartbeat")

    rig.bridge.stop_cleanly()
    rig.sim.stop_cleanly()


def test_plc_bridge_shuts_down_promptly_while_reconnecting(plc):
    # Nothing listens on the port: the bridge sits in its backoff state, with a
    # long delay, and must still answer commands and exit immediately.
    rig = plc(bridge_overrides={"backoff.initial_ms": 60_000, "backoff.max_ms": 60_000})
    rig.start_bridge()
    rig.stream.pump(0.5)
    mark = rig.stream.mark()
    rig.stream.pump(1.0)
    waiting = rig.conveyor[mark[b"plc.conveyor"]:]
    assert_rate(waiting, 100.0, RATE_TOL_PLC, "plc.conveyor during backoff")
    assert all(not m.link_up for m in waiting)
    reply = rig.cmd.send("plc", set_conveyor_speed=cell_pb2.SetConveyorSpeed(speed=1.0))
    assert not reply.ok
    rig.bridge.stop_cleanly()
