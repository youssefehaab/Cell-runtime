"""Process-level operations: find and kill a cell process, show its logs.

Three supervisors are supported:

* ``local``   - processes started by ``cellctl up`` (pidfiles and logs in the run dir)
* ``systemd`` - units ``cell-<process>.service`` (underscores become dashes)
* ``docker``  - compose services ``<process>`` (underscores become dashes)
"""
from __future__ import annotations

import os
import shutil
import signal
import subprocess
import sys
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path

from .errors import CellctlError

MODES = ("auto", "local", "systemd", "docker")

Echo = Callable[[str], None]


def unit_name(process: str) -> str:
    return f"cell-{process.replace('_', '-')}.service"


def service_name(process: str) -> str:
    return process.replace("_", "-")


def pidfile_path(run_dir: Path, process: str) -> Path:
    return run_dir / f"{process}.pid"


def log_path(run_dir: Path, process: str) -> Path:
    return run_dir / "logs" / f"{process}.log"


# ---------------------------------------------------------------- /proc scan ---


def _read_comm(pid: int) -> str | None:
    try:
        return Path(f"/proc/{pid}/comm").read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return None


def _read_cmdline(pid: int) -> list[str]:
    try:
        raw = Path(f"/proc/{pid}/cmdline").read_bytes()
    except OSError:
        return []
    return [part.decode("utf-8", errors="replace") for part in raw.split(b"\0") if part]


def pid_matches(pid: int, process: str) -> bool:
    """True if ``pid`` is running ``process``.

    Either the kernel's command name is exactly the process name (a C++
    binary; ``comm`` holds at most 15 characters), or it is a Python
    interpreter running the module or script of that name
    (``python -m plc_sim``, ``python .../plc_sim.py``, the ``plc-sim`` script).
    Substring matches are deliberately not accepted: ``cellctl fault kill
    plc_sim`` itself has "plc_sim" on its command line.
    """
    comm = _read_comm(pid)
    if comm is None:
        return False
    dashed = process.replace("_", "-")
    if comm in (process[:15], dashed[:15]):
        return True
    if not comm.startswith("python"):
        return False
    argv = _read_cmdline(pid)
    for i, arg in enumerate(argv[1:], start=1):
        if arg == "-m":
            return i + 1 < len(argv) and argv[i + 1] == process
        if arg == "-c":
            return False
        if arg.startswith("-"):
            continue
        return os.path.basename(arg) in (process, f"{process}.py", dashed)
    return False


def scan_pids(process: str) -> list[int]:
    """PIDs of every running instance of ``process`` (never this process)."""
    own = os.getpid()
    found: list[int] = []
    try:
        entries = os.listdir("/proc")
    except OSError:
        return found
    for entry in entries:
        if not entry.isdigit():
            continue
        pid = int(entry)
        if pid != own and pid_matches(pid, process):
            found.append(pid)
    return sorted(found)


def read_pidfile(path: Path) -> int | None:
    try:
        pid = int(path.read_text(encoding="ascii").strip())
    except (OSError, ValueError):
        return None
    return pid if pid > 0 else None


def pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


# ---------------------------------------------------------------------- kill ---


@dataclass(frozen=True)
class KillResult:
    mode: str
    description: str  # exactly what was done, for the operator


def _run(argv: Sequence[str], echo: Echo) -> subprocess.CompletedProcess[str]:
    echo("+ " + " ".join(argv))
    try:
        return subprocess.run(list(argv), capture_output=True, text=True, check=False)
    except FileNotFoundError as exc:
        raise CellctlError(f"'{argv[0]}' not found on PATH") from exc


def systemd_unit_active(process: str) -> bool:
    if shutil.which("systemctl") is None:
        return False
    try:
        result = subprocess.run(["systemctl", "is-active", "--quiet", unit_name(process)],
                                capture_output=True, check=False, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return result.returncode == 0


def resolve_mode(mode: str, process: str) -> str:
    if mode not in MODES:
        raise CellctlError(f"unknown mode '{mode}' (expected one of: {', '.join(MODES)})")
    if mode != "auto":
        return mode
    return "systemd" if systemd_unit_active(process) else "local"


def kill_local(process: str, run_dir: Path, kill_all: bool = False) -> KillResult:
    """SIGKILL the locally running ``process``: pidfile first, then a /proc scan."""
    pidfile = pidfile_path(run_dir, process)
    pid = read_pidfile(pidfile)
    targets: list[int] = []
    how = ""
    if pid is not None and pid_alive(pid) and pid_matches(pid, process):
        targets, how = [pid], f"pidfile {pidfile}"
    else:
        stale = ""
        if pid is not None:
            stale = f" (pidfile {pidfile} names pid {pid}, which is not a running {process})"
        targets = scan_pids(process)
        how = "/proc scan"
        if not targets:
            raise CellctlError(f"no running '{process}' process found: no usable pidfile in "
                               f"{run_dir} and no match in /proc{stale}")
        if len(targets) > 1 and not kill_all:
            raise CellctlError(
                f"{len(targets)} '{process}' processes found by /proc scan "
                f"(pids {', '.join(map(str, targets))}) and no pidfile to choose one; "
                "nothing killed. Pass --all to kill every match")
    killed: list[int] = []
    for target in targets:
        try:
            os.kill(target, signal.SIGKILL)
            killed.append(target)
        except ProcessLookupError:
            continue
        except PermissionError as exc:
            raise CellctlError(f"not permitted to kill {process} (pid {target})") from exc
    if not killed:
        raise CellctlError(f"'{process}' exited before it could be killed; nothing killed")
    pids = ", ".join(map(str, killed))
    return KillResult("local", f"sent SIGKILL to {process} (pid {pids}, found via {how})")


def kill_systemd(process: str, echo: Echo) -> KillResult:
    unit = unit_name(process)
    result = _run(["systemctl", "kill", "--signal=SIGKILL", unit], echo)
    if result.returncode != 0:
        raise CellctlError(f"systemctl kill {unit} failed (exit {result.returncode}): "
                           f"{result.stderr.strip() or 'no output'}")
    return KillResult("systemd", f"sent SIGKILL to systemd unit {unit}")


def kill_docker(process: str, compose_file: Path, echo: Echo) -> KillResult:
    """SIGKILL the container's main process from the host.

    `docker compose kill` is deliberately not used: Docker treats it as an
    operator stop and does not apply the restart policy, so it would not
    exercise recovery. Killing the host PID looks like a real crash.
    """
    service = service_name(process)
    compose = ["docker", "compose", "-f", str(compose_file)]
    ps = _run([*compose, "ps", "-q", service], echo)
    container = ps.stdout.strip().splitlines()[0] if ps.stdout.strip() else ""
    if ps.returncode != 0 or not container:
        raise CellctlError(f"compose service {service} is not running ({compose_file}): "
                           f"{ps.stderr.strip() or 'no container'}")
    inspect = _run(["docker", "inspect", "-f", "{{.State.Pid}}", container], echo)
    pid_text = inspect.stdout.strip()
    if inspect.returncode != 0 or not pid_text.isdigit() or int(pid_text) <= 0:
        raise CellctlError(f"cannot find the host PID of {service}: "
                           f"{inspect.stderr.strip() or pid_text or 'no output'}")
    pid = int(pid_text)
    try:
        os.kill(pid, signal.SIGKILL)
    except PermissionError as exc:
        raise CellctlError(f"not permitted to kill pid {pid} of {service}; run as root") from exc
    except ProcessLookupError as exc:
        raise CellctlError(f"{service} (pid {pid}) exited before it could be killed") from exc
    return KillResult("docker", f"sent SIGKILL to compose service {service} "
                                f"(host pid {pid}, container {container[:12]})")


# ---------------------------------------------------------------------- logs ---


def tail_lines(path: Path, count: int) -> list[str]:
    """The last ``count`` lines of a text file (reads at most the final 1 MiB)."""
    if count <= 0:
        return []
    with open(path, "rb") as handle:
        handle.seek(0, os.SEEK_END)
        size = handle.tell()
        handle.seek(max(0, size - (1 << 20)))
        data = handle.read()
    return data.decode("utf-8", errors="replace").splitlines()[-count:]


def show_local_logs(processes: Sequence[str], run_dir: Path, lines: int, follow: bool,
                    out: Callable[[str], None] = lambda s: print(s, flush=True)) -> None:
    """Print the tail of each process's log; with ``follow`` keep printing new lines."""
    paths = {p: log_path(run_dir, p) for p in processes}
    present = {p: path for p, path in paths.items() if path.is_file()}
    if not present:
        looked = ", ".join(str(path) for path in paths.values())
        raise CellctlError(
            f"no local logs found ({looked}). Local logs are written by `cellctl up`; "
            "for a systemd or docker deployment use --mode systemd|docker")
    prefix = len(present) > 1
    offsets: dict[str, int] = {}
    for process, path in present.items():
        offsets[process] = path.stat().st_size
        if prefix and not follow:
            out(f"==> {path} <==")
        for line in tail_lines(path, lines):
            out(f"{process} | {line}" if prefix and follow else line)
    if not follow:
        return
    pending: dict[str, bytes] = {p: b"" for p in present}
    while True:
        idle = True
        for process, path in present.items():
            try:
                size = path.stat().st_size
            except OSError:
                continue
            if size < offsets[process]:  # truncated or replaced
                offsets[process] = 0
            if size == offsets[process]:
                continue
            with open(path, "rb") as handle:
                handle.seek(offsets[process])
                chunk = handle.read()
            offsets[process] += len(chunk)
            *complete, pending[process] = (pending[process] + chunk).split(b"\n")
            for raw in complete:
                line = raw.decode("utf-8", errors="replace")
                out(f"{process} | {line}" if prefix else line)
            idle = False
        if idle:
            time.sleep(0.2)


def exec_log_command(argv: Sequence[str], echo: Echo) -> int:
    """Run journalctl / docker compose logs attached to this terminal."""
    echo("+ " + " ".join(argv))
    try:
        return subprocess.run(list(argv), check=False, stdout=sys.stdout,
                              stderr=sys.stderr).returncode
    except FileNotFoundError as exc:
        raise CellctlError(f"'{argv[0]}' not found on PATH") from exc
    except KeyboardInterrupt:
        return 0


def journalctl_argv(processes: Sequence[str], lines: int, follow: bool) -> list[str]:
    argv = ["journalctl", "--no-pager", "-n", str(lines)]
    for process in processes:
        argv += ["-u", unit_name(process)]
    if follow:
        argv.append("-f")
    return argv


def compose_logs_argv(processes: Sequence[str], compose_file: Path, lines: int,
                      follow: bool) -> list[str]:
    argv = ["docker", "compose", "-f", str(compose_file), "logs", "--tail", str(lines)]
    if follow:
        argv.append("-f")
    return argv + [service_name(p) for p in processes]
