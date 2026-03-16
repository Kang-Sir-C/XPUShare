#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Parse E5 microbench logs (from exp/e5_microbench_torch.py) and compute Jain's fairness index.

Input: one or more --log files (each file is one tenant/pod).
       Optionally, --quota values (one per --log) for proportional-fairness normalisation.
Output: CSV with per-log throughput and overall Jain.

When --quota is supplied, Jain is computed on *normalised* throughput
(iter_per_sec / quota) so that a perfectly proportional allocation yields J ≈ 1.0.
Without --quota, Jain is computed on raw throughput (backward-compatible).
"""

from __future__ import annotations

import argparse
import csv
import re
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional


RESULT_RE = re.compile(r"\[E5\]\s+result\s+.*iter_per_sec=(\d+(?:\.\d+)?)")


@dataclass
class Item:
    path: Path
    quota: Optional[float] = None
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
    ap.add_argument("--quota", action="append", type=float, default=None,
                    help="gpu_request quota for each --log (repeatable, same order)")
    ap.add_argument("--out", required=True, help="output CSV")
    args = ap.parse_args()

    quotas: Optional[List[float]] = args.quota
    if quotas is not None and len(quotas) != len(args.log):
        print(f"[ERROR] --quota count ({len(quotas)}) != --log count ({len(args.log)})")
        return 1

    items = [parse_one(Path(p)) for p in args.log]
    if quotas is not None:
        for it, q in zip(items, quotas):
            it.quota = q

    # Normalised throughput for Jain (if quotas provided)
    norm_vals: List[float] = []
    raw_vals: List[float] = []
    for it in items:
        if it.iter_per_sec is not None:
            raw_vals.append(it.iter_per_sec)
            if it.quota is not None and it.quota > 0:
                norm_vals.append(it.iter_per_sec / it.quota)

    use_norm = len(norm_vals) == len(raw_vals) and len(norm_vals) > 0
    jain_val = jain(norm_vals) if use_norm else jain(raw_vals)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        header = ["log", "iter_per_sec"]
        if use_norm:
            header.append("normalised_ips")
        w.writerow(header)
        for it in items:
            row = [str(it.path),
                   "" if it.iter_per_sec is None else f"{it.iter_per_sec:.6f}"]
            if use_norm and it.iter_per_sec is not None and it.quota:
                row.append(f"{it.iter_per_sec / it.quota:.6f}")
            elif use_norm:
                row.append("")
            w.writerow(row)
        w.writerow([])
        label = "jain_fairness (normalised)" if use_norm else "jain_fairness (raw)"
        w.writerow([label, f"{jain_val:.6f}"])

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

