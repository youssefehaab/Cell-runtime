"""The cell_algo Python module (pybind11 bindings of src/algo) and its demo."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BIN_DIR = Path(os.environ.get("CELL_BIN_DIR", REPO / "build" / "bin"))
MODULE_DIR = BIN_DIR.parent / "python"

sys.path.insert(0, str(MODULE_DIR))
cell_algo = pytest.importorskip("cell_algo", reason="cell_algo bindings were not built")


def test_aabb_collision():
    a = cell_algo.Aabb(0, 0, 0, 1, 1, 1)
    assert cell_algo.intersects(a, cell_algo.Aabb(0.5, 0.5, 0.5, 2, 2, 2))
    assert not cell_algo.intersects(a, cell_algo.Aabb(1, 0, 0, 2, 1, 1))  # touching only
    assert cell_algo.contains(cell_algo.Aabb(0, 0, 0, 2, 2, 2), a)
    assert a.volume() == 1.0


def test_pack_respects_stacking_and_reports_fill():
    pallet = cell_algo.Pallet(length=1.2, width=0.8, max_height=0.8, max_weight=1000.0)
    boxes = [cell_algo.Box(id=i, length=0.4, width=0.4, height=0.4, weight=10, max_load=500)
             for i in range(12)]
    result = cell_algo.pack(pallet, boxes)
    assert cell_algo.validate(pallet, boxes, result) == ""
    assert len(result.placed) == 12 and result.unplaced == []
    assert result.fill_ratio == pytest.approx(1.0)
    assert result.total_weight == pytest.approx(120.0)

    fragile = [cell_algo.Box(id=1, length=0.4, width=0.4, height=0.4, weight=2, max_load=1),
               cell_algo.Box(id=2, length=0.4, width=0.4, height=0.4, weight=30, max_load=100)]
    column = cell_algo.Pallet(length=0.4, width=0.4, max_height=2.0, max_weight=1000.0)
    stacked = cell_algo.pack(column, fragile)
    assert [p.id for p in stacked.placed] == [2, 1]  # the heavy box is never on the fragile one
    assert stacked.placed[1].z == pytest.approx(0.4)


def test_demo_runs_and_validates():
    env = dict(os.environ, PYTHONPATH=str(MODULE_DIR), COLUMNS="120")
    out = subprocess.run([sys.executable, str(REPO / "python" / "demos" / "pallet_demo.py"),
                          "--boxes", "30"], env=env, capture_output=True, text=True, timeout=60)
    assert out.returncode == 0, out.stdout + out.stderr
    assert "all satisfied" in out.stdout and "volume fill" in out.stdout
