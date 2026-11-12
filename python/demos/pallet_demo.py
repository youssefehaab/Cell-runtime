#!/usr/bin/env python3
"""Pack a random mix of boxes onto a pallet and draw the result in the terminal.

Uses the `cell_algo` C++ module (pybind11). Build first, then:

    PYTHONPATH=build/python python python/demos/pallet_demo.py [--boxes 40] [--seed 7]

Each layer of the load is drawn top-down; every box gets its own colour and its
id. A summary table follows.
"""

from __future__ import annotations

import argparse
import random
import sys

from rich.console import Console
from rich.table import Table
from rich.text import Text

try:
    import cell_algo
except ImportError:
    sys.exit("cell_algo not found: build the project and run with PYTHONPATH=build/python")

CELL_M = 0.025  # one character cell = 2.5 cm along X
ROW_M = 0.05    # one text row = 5 cm along Y (characters are about twice as tall as wide)
COLOURS = ["red", "green", "yellow", "blue", "magenta", "cyan", "bright_red", "bright_green",
           "bright_yellow", "bright_blue", "bright_magenta", "bright_cyan", "orange3", "purple"]


def random_boxes(count: int, seed: int) -> list:
    rng = random.Random(seed)
    sizes = [(0.4, 0.3, 0.25), (0.3, 0.2, 0.2), (0.6, 0.4, 0.3), (0.2, 0.2, 0.15), (0.4, 0.4, 0.2)]
    boxes = []
    for i in range(count):
        length, width, height = rng.choice(sizes)
        weight = round(length * width * height * rng.uniform(150, 400), 1)
        boxes.append(cell_algo.Box(id=i, length=length, width=width, height=height, weight=weight,
                                   max_load=round(weight * rng.uniform(2, 8), 1)))
    return boxes


def draw_layer(console: Console, pallet, placements: list, z: float) -> None:
    cols, rows = round(pallet.length / CELL_M), round(pallet.width / ROW_M)
    grid: list[list[tuple[str, str]]] = [[("·", "grey37")] * cols for _ in range(rows)]
    for p in placements:
        colour = COLOURS[p.id % len(COLOURS)]
        c0, c1 = round(p.x / CELL_M), round((p.x + p.length) / CELL_M)
        r0, r1 = round(p.y / ROW_M), round((p.y + p.width) / ROW_M)
        label = str(p.id)
        for r in range(r0, min(r1, rows)):
            for c in range(c0, min(c1, cols)):
                grid[r][c] = ("█", colour)
        mid, start = (r0 + r1 - 1) // 2, c0 + max(0, (c1 - c0 - len(label)) // 2)
        for k, ch in enumerate(label[: c1 - c0]):
            if mid < rows and start + k < cols:
                grid[mid][start + k] = (ch, f"bold black on {colour}")
    console.print(f"[bold]z = {z:.2f} m[/bold]  ({len(placements)} boxes)")
    for row in reversed(grid):  # Y grows upwards on screen
        console.print(Text.assemble(*row))
    console.print()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--boxes", type=int, default=40)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--max-height", type=float, default=1.2)
    args = parser.parse_args()

    pallet = cell_algo.Pallet(length=1.2, width=0.8, max_height=args.max_height, max_weight=800.0)
    boxes = random_boxes(args.boxes, args.seed)
    result = cell_algo.pack(pallet, boxes)
    problem = cell_algo.validate(pallet, boxes, result)

    console = Console()
    for z in sorted({round(p.z, 6) for p in result.placed}):
        draw_layer(console, pallet, [p for p in result.placed if round(p.z, 6) == z], z)

    table = Table(title="Pallet 1.2 x 0.8 m", show_header=False)
    table.add_row("boxes placed", f"{len(result.placed)} of {len(boxes)}")
    table.add_row("volume fill", f"{result.fill_ratio * 100:.1f} %")
    table.add_row("load height", f"{result.load_height:.2f} m of {pallet.max_height:.2f} m")
    table.add_row("total weight", f"{result.total_weight:.1f} kg of {pallet.max_weight:.0f} kg")
    table.add_row("constraints", "[green]all satisfied[/green]" if not problem
                  else f"[red]{problem}[/red]")
    console.print(table)
    sys.exit(1 if problem else 0)


if __name__ == "__main__":
    main()
