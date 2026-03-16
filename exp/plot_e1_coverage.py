#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Plot E1 coverage comparison (driver vs runtime) from parse_coverage.py CSV output.

Input CSV format (long):
  run_id,layer,pid,category,symbol,count

This script aggregates by category (memory_alloc, kernel_launch, memory_view, ...)
for the latest run_id (or a chosen run_id) and plots a grouped bar chart.
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import defaultdict
from pathlib import Path
from typing import DefaultDict, Dict, Tuple


def read_csv(path: Path) -> Tuple[int, Dict[str, Dict[str, int]]]:
    per_run: DefaultDict[int, DefaultDict[str, DefaultDict[str, int]]] = defaultdict(
        lambda: defaultdict(lambda: defaultdict(int))
    )
    max_run = 0
    with path.open("r", encoding="utf-8") as f:
        r = csv.DictReader(f)
        required = {"run_id", "layer", "category", "count"}
        if not required.issubset(set(r.fieldnames or [])):
            raise ValueError(f"unexpected csv headers: {r.fieldnames}")
        for row in r:
            run_id = int(row["run_id"])
            layer = (row["layer"] or "").strip().lower()
            category = (row["category"] or "").strip().lower() or "other"
            count = int(row["count"])
            per_run[run_id][layer][category] += count
            max_run = max(max_run, run_id)

    if max_run == 0:
        raise ValueError("no rows found")

    latest = per_run[max_run]
    out: Dict[str, Dict[str, int]] = {"driver": dict(latest.get("driver", {})), "runtime": dict(latest.get("runtime", {}))}
    return max_run, out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", required=True, help="CSV produced by exp/parse_coverage.py --out")
    ap.add_argument("--out", required=True, help="Output PNG path")
    ap.add_argument("--title", default="E1 Coverage: Driver vs Runtime", help="Plot title")
    args = ap.parse_args(argv)

    csv_path = Path(args.csv)
    if not csv_path.exists():
        print(f"error: csv not found: {csv_path}", file=sys.stderr)
        return 2

    try:
        import matplotlib.pyplot as plt  # type: ignore
    except Exception as e:
        print("error: matplotlib is required to plot; install it or plot from CSV in Excel.", file=sys.stderr)
        print(f"detail: {e}", file=sys.stderr)
        return 3

    run_id, data = read_csv(csv_path)
    cats = sorted(set(data.get("driver", {}).keys()) | set(data.get("runtime", {}).keys()))
    if not cats:
        print("error: no categories found in csv", file=sys.stderr)
        return 4

    driver_vals = [data.get("driver", {}).get(c, 0) for c in cats]
    runtime_vals = [data.get("runtime", {}).get(c, 0) for c in cats]

    x = list(range(len(cats)))
    width = 0.38

    fig, ax = plt.subplots(figsize=(9, 4.8), dpi=200)
    ax.bar([i - width / 2 for i in x], driver_vals, width, label="Driver")
    ax.bar([i + width / 2 for i in x], runtime_vals, width, label="Runtime")

    ax.set_title(f"{args.title} (run {run_id})")
    ax.set_xticks(x)
    ax.set_xticklabels(cats, rotation=0)
    ax.set_ylabel("Call Count")
    ax.grid(axis="y", linestyle="--", linewidth=0.6, alpha=0.5)
    ax.legend()

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(out_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

