"""`cellctl up`: a small local supervisor for the cell's processes.

For development machines, tests and hosts without systemd. It starts each
process as a child, writes ``<run_dir>/<process>.pid`` and appends the child's
output to ``<run_dir>/logs/<process>.log``, restarts a child that exits
unexpectedly (exponential backoff, reset once it has stayed up), and on
SIGINT/SIGTERM stops the children (SIGTERM, then SIGKILL after a grace period).

Single-threaded by design: one loop polls the children, so there is no shared
state to lock and signal handlers only set a flag.
"""
from __future__ import annotations

import ctypes
import os
import shutil
import signal
import subprocess
import sys
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from types import FrameType
from typing import IO

from .errors import CellctlError
from .ops import log_path, pid_alive, pidfile_path, read_pidfile

PYTHON_MODULES = frozenset({"plc_sim"})  # run as `python -m <name>`; the rest are binaries
SUPERVISOR_PIDFILE = "cellctl-up.pid"
POLL_INTERVAL_S = 0.05
_PR_SET_PDEATHSIG = 1


@dataclass(frozen=True)
class SupervisorOptions:
    processes: tuple[str, ...]  # in start order
    bin_dir: Path
    config_dir: Path
    run_dir: Path
    overrides: dict[str, tuple[str, ...]] = field(default_factory=dict)  # process -> --set values
    backoff_initial_s: float = 0.2
    backoff_max_s: float = 5.0
    backoff_reset_s: float = 10.0
    stop_grace_s: float = 5.0


def parse_overrides(specs: Sequence[str], known: Sequence[str]) -> dict[str, tuple[str, ...]]:
    """``['broker:pub_endpoint=tcp://*:6001', ...]`` -> ``{process: (key=value, ...)}``."""
    out: dict[str, list[str]] = {}
    for spec in specs:
        process, sep, setting = spec.partition(":")
        if not sep or "=" not in setting or setting.startswith("="):
            raise CellctlError(f"malformed --override '{spec}': expected PROCESS:dotted.key=value")
        if process not in known:
            raise CellctlError(f"--override '{spec}': unknown process '{process}' "
                               f"(expected one of: {', '.join(known)})")
        out.setdefault(process, []).append(setting)
    return {process: tuple(settings) for process, settings in out.items()}


def resolve_binary(name: str, bin_dir: Path) -> Path | None:
    candidate = bin_dir / name
    if candidate.is_file() and os.access(candidate, os.X_OK):
        return candidate.resolve()
    found = shutil.which(name)
    return Path(found) if found else None


def build_argv(name: str, options: SupervisorOptions) -> list[str]:
    """Command line for one process, same conventions as starting it by hand."""
    config = options.config_dir / f"{name}.json"
    if name in PYTHON_MODULES:
        argv = [sys.executable, "-m", name]
    else:
        binary = resolve_binary(name, options.bin_dir)
        if binary is None:
            raise CellctlError(
                f"binary '{name}' not found in {options.bin_dir} or on PATH "
                "(build first, pass --bin-dir, or leave it out with --only)")
        argv = [str(binary)]
    argv += ["--config", str(config)]
    for setting in options.overrides.get(name, ()):
        argv += ["--set", setting]
    return argv


def describe_exit(returncode: int) -> str:
    if returncode < 0:
        try:
            return f"killed by {signal.Signals(-returncode).name}"
        except ValueError:
            return f"killed by signal {-returncode}"
    return f"exited with code {returncode}"


def _timestamp() -> str:
    return time.strftime("%Y-%m-%d %H:%M:%S")


class _Child:
    """One supervised process and its restart bookkeeping."""

    def __init__(self, name: str, initial_backoff_s: float) -> None:
        self.name = name
        self.popen: subprocess.Popen[bytes] | None = None
        self.started_at = 0.0
        self.backoff_s = initial_backoff_s
        self.restart_at: float | None = None  # set while waiting to restart
        self.restarts = 0


class Supervisor:
    def __init__(self, options: SupervisorOptions,
                 log: Callable[[str], None] | None = None) -> None:
        self.options = options
        self._children = [_Child(name, options.backoff_initial_s) for name in options.processes]
        self._stop_signals = 0
        self._log_sink = log
        self._own_log: IO[str] | None = None
        self._libc = self._load_libc()

    # ------------------------------------------------------------ plumbing ---

    @staticmethod
    def _load_libc() -> ctypes.CDLL | None:
        try:
            return ctypes.CDLL(None, use_errno=True)
        except OSError:
            return None

    def _log(self, message: str) -> None:
        line = f"[{_timestamp()}] [cellctl up] {message}"
        if self._log_sink is not None:
            self._log_sink(line)
        else:
            print(line, file=sys.stderr, flush=True)
        if self._own_log is not None:
            self._own_log.write(line + "\n")
            self._own_log.flush()

    def _child_setup(self) -> None:
        # Runs in the child between fork and exec. If the supervisor dies
        # without a chance to clean up (SIGKILL), the kernel terminates the
        # child instead of leaving an orphan.
        if self._libc is not None:
            self._libc.prctl(_PR_SET_PDEATHSIG, int(signal.SIGTERM), 0, 0, 0)

    def _child_env(self) -> dict[str, str]:
        env = dict(os.environ)
        # `python -m plc_sim` must find the package this cellctl was loaded from.
        package_root = str(Path(__file__).resolve().parents[1])
        existing = env.get("PYTHONPATH", "")
        env["PYTHONPATH"] = package_root + (os.pathsep + existing if existing else "")
        env["CELL_CONFIG_DIR"] = str(self.options.config_dir)
        return env

    def _on_signal(self, signum: int, frame: FrameType | None) -> None:
        del signum, frame
        self._stop_signals += 1

    # --------------------------------------------------------------- start ---

    def _preflight(self) -> None:
        """Fail before starting anything if something obvious is missing."""
        options = self.options
        if not options.processes:
            raise CellctlError("nothing to run")
        for name in options.processes:
            config = options.config_dir / f"{name}.json"
            if not config.is_file():
                raise CellctlError(f"config file {config} not found (pass --config-dir)")
            build_argv(name, options)  # raises if the binary is missing
        pidfile = options.run_dir / SUPERVISOR_PIDFILE
        other = read_pidfile(pidfile)
        if other is not None and other != os.getpid() and pid_alive(other):
            raise CellctlError(f"another `cellctl up` (pid {other}) already uses run dir "
                               f"{options.run_dir}; stop it or pass a different --run-dir")

    def _spawn(self, child: _Child) -> None:
        options = self.options
        path = log_path(options.run_dir, child.name)
        try:
            argv = build_argv(child.name, options)
            with open(path, "ab") as log_file:
                log_file.write(f"=== [{_timestamp()}] cellctl up: starting {child.name}: "
                               f"{' '.join(argv)}\n".encode())
                log_file.flush()
                # Own process group: a terminal Ctrl-C reaches only the
                # supervisor, which then stops the children in order.
                child.popen = subprocess.Popen(
                    argv, stdin=subprocess.DEVNULL, stdout=log_file, stderr=log_file,
                    env=self._child_env(), process_group=0, preexec_fn=self._child_setup)
        except (OSError, CellctlError) as exc:
            child.popen = None
            self._schedule_restart(child, f"could not start {child.name}: {exc}")
            return
        child.started_at = time.monotonic()
        child.restart_at = None
        pidfile_path(options.run_dir, child.name).write_text(f"{child.popen.pid}\n",
                                                             encoding="ascii")
        self._log(f"started {child.name} (pid {child.popen.pid}, log {path})")

    def _schedule_restart(self, child: _Child, why: str) -> None:
        delay = child.backoff_s
        child.backoff_s = min(child.backoff_s * 2.0, self.options.backoff_max_s)
        child.restart_at = time.monotonic() + delay
        child.restarts += 1
        self._log(f"{why}; restart #{child.restarts} in {delay * 1000:.0f} ms")

    # ---------------------------------------------------------------- loop ---

    def _check(self, child: _Child, now: float) -> None:
        if child.popen is not None:
            returncode = child.popen.poll()
            if returncode is None:
                if (child.backoff_s != self.options.backoff_initial_s
                        and now - child.started_at >= self.options.backoff_reset_s):
                    child.backoff_s = self.options.backoff_initial_s
                return
            pid = child.popen.pid
            uptime = now - child.started_at
            child.popen = None
            pidfile_path(self.options.run_dir, child.name).unlink(missing_ok=True)
            if uptime >= self.options.backoff_reset_s:
                child.backoff_s = self.options.backoff_initial_s
            self._schedule_restart(child, f"{child.name} (pid {pid}) {describe_exit(returncode)} "
                                          f"after {uptime:.1f} s")
        elif child.restart_at is not None and now >= child.restart_at:
            self._spawn(child)

    def _stop_children(self) -> bool:
        """SIGTERM in reverse start order, SIGKILL stragglers. True if all exited cleanly."""
        running = [c for c in reversed(self._children) if c.popen is not None]
        for child in running:
            assert child.popen is not None
            if child.popen.poll() is None:
                child.popen.send_signal(signal.SIGTERM)
        deadline = time.monotonic() + self.options.stop_grace_s
        signals_at_stop = self._stop_signals
        clean = True
        while any(c.popen is not None and c.popen.poll() is None for c in running):
            if time.monotonic() >= deadline or self._stop_signals > signals_at_stop:
                break
            time.sleep(POLL_INTERVAL_S)
        for child in running:
            assert child.popen is not None
            if child.popen.poll() is None:
                self._log(f"{child.name} (pid {child.popen.pid}) still running; sending SIGKILL")
                child.popen.kill()
                clean = False
            returncode = child.popen.wait()
            if returncode not in (0, -signal.SIGTERM, -signal.SIGKILL):
                clean = False
            self._log(f"{child.name} (pid {child.popen.pid}) {describe_exit(returncode)}")
            child.popen = None
            pidfile_path(self.options.run_dir, child.name).unlink(missing_ok=True)
        return clean

    def run(self) -> int:
        """Supervise until SIGINT/SIGTERM. Returns the process exit code."""
        options = self.options
        self._preflight()
        (options.run_dir / "logs").mkdir(parents=True, exist_ok=True)
        supervisor_pidfile = options.run_dir / SUPERVISOR_PIDFILE
        supervisor_pidfile.write_text(f"{os.getpid()}\n", encoding="ascii")

        previous = {signo: signal.signal(signo, self._on_signal)
                    for signo in (signal.SIGINT, signal.SIGTERM)}
        clean = False
        try:
            with open(options.run_dir / "logs" / "cellctl-up.log", "a",
                      encoding="utf-8") as own_log:
                self._own_log = own_log
                self._log(f"supervising {', '.join(options.processes)} "
                          f"(run dir {options.run_dir}, config dir {options.config_dir})")
                try:
                    for child in self._children:
                        if self._stop_signals:
                            break
                        self._spawn(child)
                    while not self._stop_signals:
                        now = time.monotonic()
                        for child in self._children:
                            self._check(child, now)
                        time.sleep(POLL_INTERVAL_S)
                    self._log("stop requested; stopping children")
                finally:
                    clean = self._stop_children()
                    self._log("stopped cleanly" if clean else "stopped (not all children "
                                                              "exited cleanly)")
                    self._own_log = None
        finally:
            for signo, handler in previous.items():
                signal.signal(signo, handler)
            supervisor_pidfile.unlink(missing_ok=True)
        return 0 if clean else 1
