"""Release management: install, upgrade, automatic rollback of a broken release.

Runs the real deploy/scripts against a private prefix with the local supervisor
(`cellctl up`), so it needs neither root nor systemd. Each release is staged
from the build tree by make-release.sh and gets private endpoints.

Environment: CELL_BIN_DIR, CELL_TEST_SLOW (see test_cell.py).
"""

from __future__ import annotations

import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BIN_DIR = Path(os.environ.get("CELL_BIN_DIR", REPO / "build" / "bin"))
BUILD_DIR = BIN_DIR.parent
SCRIPTS = REPO / "deploy" / "scripts"
SLOW = os.environ.get("CELL_TEST_SLOW", "") not in ("", "0")
HEALTH_WAIT_MS = 30000 if SLOW else 10000


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def load_config(path: Path) -> dict:
    text = re.sub(r"^\s*//.*$", "", path.read_text(), flags=re.MULTILINE)
    return json.loads(text)


class Site:
    """A private install prefix plus the endpoints every release of it uses."""

    def __init__(self, root: Path):
        self.root = root
        self.prefix = root / "opt"
        self.modbus, self.pub, self.cmd = free_port(), free_port(), free_port()
        self.env = dict(
            os.environ,
            CELL_PREFIX=str(self.prefix),
            CELL_SUPERVISOR="local",
            CELL_PYTHON=sys.executable,
            CELL_HEALTH_WAIT_MS=str(HEALTH_WAIT_MS),
        )
        self.env.pop("CELL_CONFIG_DIR", None)

    def run(self, script: str, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([str(SCRIPTS / script), *args], env=self.env, capture_output=True,
                              text=True, timeout=180)

    def stage(self, version: str, *, broken: bool = False) -> Path:
        """Stage a release with this site's endpoints; `broken` makes the broker crash."""
        out = self.root / f"stage-{version}"
        subprocess.run([str(SCRIPTS / "make-release.sh"), str(BUILD_DIR), str(out), version],
                       check=True, capture_output=True, timeout=120)
        etc = out / "etc" / "cell-runtime"
        ipc = lambda name: f"ipc://{self.root}/ipc/{name}"  # noqa: E731
        edits = {
            "arm_sim": {"telemetry_endpoint": ipc("arm.t"), "cmd_endpoint": ipc("arm.c")},
            "sensor_sim": {"frames_endpoint": ipc("sen.t"), "cmd_endpoint": ipc("sen.c")},
            "plc_sim": {"port": self.modbus, "heartbeat_endpoint": ipc("sim.t")},
            "plc_bridge": {"telemetry_endpoint": ipc("plc.t"), "cmd_endpoint": ipc("plc.c")},
            "broker": {
                "pub_endpoint": f"tcp://127.0.0.1:{self.pub}",
                "cmd_endpoint": f"tcp://127.0.0.1:{self.cmd}",
                "streams": [
                    {"topic": "arm.joint", "endpoint": ipc("arm.t"), "process": "arm_sim"},
                    {"topic": "sensor.frame", "endpoint": ipc("sen.t"), "process": "sensor_sim"},
                    {"topic": "plc.conveyor", "endpoint": ipc("plc.t"), "process": "plc_bridge"},
                    {"endpoint": ipc("sim.t"), "process": "plc_sim"},
                ],
                "routes": {"arm": ipc("arm.c"), "sensor": ipc("sen.c"), "plc": ipc("plc.c")},
            },
            "cellctl": {
                "broker_sub": f"tcp://127.0.0.1:{self.pub}",
                "broker_cmd": f"tcp://127.0.0.1:{self.cmd}",
                "run_dir": str(self.prefix / "run"),
            },
        }
        for name, changes in edits.items():
            path = etc / f"{name}.json"
            config = load_config(path)
            config.update(changes)
            if name == "plc_bridge":
                config["modbus"]["port"] = self.modbus
            path.write_text(json.dumps(config, indent=2))
        if broken:
            broker = out / "bin" / "broker"
            broker.write_text("#!/bin/sh\necho 'broker: simulated bad build' >&2\nexit 1\n")
            broker.chmod(0o755)
        return out

    def current(self) -> str:
        return os.path.basename(os.readlink(self.prefix / "current"))

    def previous(self) -> str:
        return os.path.basename(os.readlink(self.prefix / "previous"))

    def healthy(self) -> bool:
        return self.run("healthcheck.sh").returncode == 0


@pytest.fixture()
def site():
    for name in ("arm_sim", "sensor_sim", "plc_bridge", "broker"):
        assert (BIN_DIR / name).exists(), f"{BIN_DIR / name} not found - build first"
    root = Path(tempfile.mkdtemp(prefix="cellrel-", dir="/tmp"))
    s = Site(root)
    try:
        yield s
    finally:
        # Stop whatever release is running (lib.sh cell_stop via a tiny inline script).
        subprocess.run(["bash", "-c", f"source {SCRIPTS}/lib.sh; cell_stop"], env=s.env,
                       capture_output=True, timeout=60)
        shutil.rmtree(root, ignore_errors=True)


def test_upgrade_then_broken_release_rolls_back_automatically(site: Site):
    # First install: becomes current; start it.
    first = site.run("install.sh", str(site.stage("1.0.0")))
    assert first.returncode == 0, first.stderr
    assert site.current() == "1.0.0"
    subprocess.run(["bash", "-c", f"source {SCRIPTS}/lib.sh; cell_start"], env=site.env,
                   check=True, timeout=60)
    assert site.healthy()

    # Releases are immutable: installing the same version again is refused.
    assert site.run("install.sh", str(site.root / "stage-1.0.0")).returncode != 0

    # A good upgrade switches over and records the previous release.
    good = site.run("upgrade.sh", str(site.stage("1.1.0")))
    assert good.returncode == 0, good.stderr
    assert site.current() == "1.1.0" and site.previous() == "1.0.0"
    assert site.healthy()

    # A release whose broker crashes on start fails the health gate; upgrade.sh
    # restores 1.1.0 by itself and reports exit code 3.
    bad = site.run("upgrade.sh", str(site.stage("2.0.0-bad", broken=True)))
    assert bad.returncode == 3, f"expected automatic rollback (3), got {bad.returncode}\n{bad.stderr}"
    assert "rolling back" in bad.stderr
    assert site.current() == "1.1.0"
    assert site.healthy()
    # The failed release stays installed for inspection but is not running.
    assert (site.prefix / "releases" / "2.0.0-bad").is_dir()

    # Manual rollback to a named release also works.
    back = site.run("rollback.sh", "1.0.0")
    assert back.returncode == 0, back.stderr
    assert site.current() == "1.0.0"
    assert site.healthy()
