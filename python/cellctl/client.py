"""ZeroMQ client for the broker's consumer-facing sockets.

Telemetry: a SUB socket on the broker's XPUB; every message is two frames,
``[topic][protobuf]``. Commands: a REQ socket on the broker's ROUTER; one
``Command`` frame out, one ``CommandReply`` frame back.

Nothing here blocks forever: every wait takes a timeout, and sockets are
created with ``LINGER=0`` so closing never waits for an absent broker.
"""
from __future__ import annotations

import time
from collections.abc import Callable
from dataclasses import dataclass
from types import TracebackType

import zmq
from google.protobuf.message import DecodeError

from cell_proto import cell_pb2

from .errors import CellctlError

TOPIC_STATE = b"cell.state"
TOPIC_STATS = b"cell.stats"

COMMAND_TARGETS = ("arm", "sensor", "plc")


@dataclass
class Snapshot:
    """The most recent telemetry, with local receive times (``time.monotonic()``)."""

    state: cell_pb2.CellState | None = None
    state_rx: float | None = None
    state_count: int = 0
    stats: cell_pb2.CellStats | None = None
    stats_rx: float | None = None
    stats_count: int = 0
    last_rx: float | None = None  # any well-formed message from the broker
    malformed: int = 0

    def state_age_s(self, now: float) -> float | None:
        return None if self.state_rx is None else max(0.0, now - self.state_rx)

    def broker_reachable(self, now: float, stale_after_s: float) -> bool:
        return self.last_rx is not None and now - self.last_rx <= stale_after_s


@dataclass(frozen=True)
class CommandResult:
    ok: bool
    error: str
    latency_ms: float | None  # None when no reply arrived
    replied: bool


class CellClient:
    """Subscribes to broker telemetry and sends commands.

    Use as a context manager; sockets are created on first use, so a command
    never opens the telemetry socket and vice versa.
    """

    def __init__(self, sub_endpoint: str, cmd_endpoint: str, command_timeout_ms: int = 2000,
                 source: str = "cellctl") -> None:
        self.sub_endpoint = sub_endpoint
        self.cmd_endpoint = cmd_endpoint
        self.command_timeout_ms = command_timeout_ms
        self.source = source
        self.snapshot = Snapshot()
        self._ctx: zmq.Context | None = zmq.Context()
        self._sub: zmq.Socket | None = None
        self._seq = 0

    # ------------------------------------------------------------ lifetime ---

    def __enter__(self) -> CellClient:
        return self

    def __exit__(self, exc_type: type[BaseException] | None, exc: BaseException | None,
                 tb: TracebackType | None) -> None:
        self.close()

    def close(self) -> None:
        if self._sub is not None:
            self._sub.close(linger=0)
            self._sub = None
        if self._ctx is not None:
            self._ctx.destroy(linger=0)
            self._ctx = None

    def _context(self) -> zmq.Context:
        if self._ctx is None:
            raise RuntimeError("CellClient is closed")
        return self._ctx

    # ----------------------------------------------------------- telemetry ---

    def _subscriber(self) -> zmq.Socket:
        if self._sub is None:
            sub = self._context().socket(zmq.SUB)
            sub.setsockopt(zmq.LINGER, 0)
            # Only the latest sample matters; never let a backlog build up.
            sub.setsockopt(zmq.RCVHWM, 200)
            sub.setsockopt(zmq.SUBSCRIBE, TOPIC_STATE)
            sub.setsockopt(zmq.SUBSCRIBE, TOPIC_STATS)
            try:
                sub.connect(self.sub_endpoint)
            except zmq.ZMQError as exc:
                sub.close(linger=0)
                raise CellctlError(f"invalid broker_sub endpoint '{self.sub_endpoint}': "
                                 f"{exc.strerror}") from exc
            self._sub = sub
        return self._sub

    def _ingest(self, frames: list[bytes]) -> None:
        snap = self.snapshot
        if len(frames) != 2:
            snap.malformed += 1
            return
        topic, payload = frames
        now = time.monotonic()
        try:
            if topic == TOPIC_STATE:
                state = cell_pb2.CellState()
                state.ParseFromString(payload)
                snap.state, snap.state_rx = state, now
                snap.state_count += 1
            elif topic == TOPIC_STATS:
                stats = cell_pb2.CellStats()
                stats.ParseFromString(payload)
                snap.stats, snap.stats_rx = stats, now
                snap.stats_count += 1
            else:
                return  # prefix match on a topic we do not decode
        except DecodeError:
            snap.malformed += 1
            return
        snap.last_rx = now

    def pump(self, timeout_s: float, until: Callable[[], bool] | None = None) -> bool:
        """Receive telemetry into ``snapshot``.

        Without ``until``: receive for ``timeout_s`` and return True. With it:
        return True as soon as ``until()`` holds, False if ``timeout_s`` passes first.
        """
        sub = self._subscriber()
        deadline = time.monotonic() + max(0.0, timeout_s)
        while True:
            # Drain whatever is queued before looking at the clock or `until`.
            while True:
                try:
                    frames = sub.recv_multipart(zmq.NOBLOCK)
                except zmq.Again:
                    break
                self._ingest(frames)
            if until is not None and until():
                return True
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return until is None
            sub.poll(min(remaining, 0.05) * 1000)

    def wait_state(self, timeout_s: float) -> cell_pb2.CellState | None:
        """The next ``cell.state`` received after this call, or None on timeout."""
        seen = self.snapshot.state_count
        if self.pump(timeout_s, until=lambda: self.snapshot.state_count > seen):
            return self.snapshot.state
        return None

    def next_stats(self, timeout_s: float) -> cell_pb2.CellStats | None:
        """The next ``cell.stats`` window received after this call, or None on timeout."""
        seen = self.snapshot.stats_count
        if self.pump(timeout_s, until=lambda: self.snapshot.stats_count > seen):
            return self.snapshot.stats
        return None

    # ------------------------------------------------------------ commands ---

    def send_command(self, command: cell_pb2.Command) -> CommandResult:
        """Send one command and wait up to ``command_timeout_ms`` for the reply.

        The header is filled in here. A missing or unreadable reply is reported
        as ``ok=False`` with ``replied=False``; this never raises for a broker
        that is down and never waits longer than the timeout.
        """
        command.h.seq = self._seq
        command.h.source = self.source
        self._seq += 1
        # A fresh REQ socket per command: after a timeout a REQ socket is stuck
        # in "awaiting reply" and cannot be reused.
        req = self._context().socket(zmq.REQ)
        try:
            req.setsockopt(zmq.LINGER, 0)
            try:
                req.connect(self.cmd_endpoint)
            except zmq.ZMQError as exc:
                raise CellctlError(f"invalid broker_cmd endpoint '{self.cmd_endpoint}': "
                                 f"{exc.strerror}") from exc
            command.h.send_mono_ns = time.monotonic_ns()
            started = time.monotonic()
            req.send(command.SerializeToString())
            if not req.poll(self.command_timeout_ms):
                return CommandResult(
                    ok=False, replied=False, latency_ms=None,
                    error=(f"no reply from broker at {self.cmd_endpoint} "
                           f"within {self.command_timeout_ms} ms"))
            frames = req.recv_multipart()
            latency_ms = (time.monotonic() - started) * 1000.0
        finally:
            req.close(linger=0)
        reply = cell_pb2.CommandReply()
        try:
            if len(frames) != 1:
                raise DecodeError(f"expected 1 frame, got {len(frames)}")
            reply.ParseFromString(frames[0])
        except DecodeError as exc:
            return CommandResult(ok=False, replied=True, latency_ms=latency_ms,
                                 error=f"malformed reply from broker: {exc}")
        error = reply.error
        if not reply.ok and not error:
            error = "command rejected (no reason given)"
        return CommandResult(ok=reply.ok, error=error, latency_ms=latency_ms, replied=True)
