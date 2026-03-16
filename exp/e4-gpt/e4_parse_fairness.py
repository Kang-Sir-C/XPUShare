#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Parse E4 microbench logs (from exp/e4_microbench_torch.py) and compute Jain's fairness index.

Input: one or more --log files (each file is one tenant/pod).
Output: CSV with per-log throughput and overall Jain.
"""

import argparse
import csv
import re
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional


# Support both legacy and updated microbench output:
# - "[E4] result ... iter_per_sec=..."
# - "[E4] done ... iters_per_sec=..."
RESULT_RE = re.compile(r"\[E4\]\s+(?:result|done)\s+.*(?:iter_per_sec|iters_per_sec)=(\d+(?:\.\d+)?)")


@dataclass
class Item:
    path: Path
    iter_per_sec: Optional[float] = None


def jain(values: List[float]) -> float:
    if not values:
        return 0.0
    s = sum(values)
    ss = sum(v * v for v in values)
    if ss == 0:
        return 0.0
    n = float(len(values))
    return (s * s) / (n * ss)


def parse_one(path: Path) -> Item:
    item = Item(path=path)
    text = path.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        m = RESULT_RE.search(line)
        if m:
            item.iter_per_sec = float(m.group(1))
    return item


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", action="append", required=True, help="log file path (repeatable)")
    ap.add_argument("--out", required=True, help="output CSV")
    args = ap.parse_args()

    items = [parse_one(Path(p)) for p in args.log]
    vals = [i.iter_per_sec for i in items if i.iter_per_sec is not None]

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["log", "iter_per_sec"])
        for it in items:
            w.writerow([it.path.name, "" if it.iter_per_sec is None else f"{it.iter_per_sec:.6f}"])
        w.writerow([])
        w.writerow(["jain_fairness", f"{jain(vals):.6f}"])

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
