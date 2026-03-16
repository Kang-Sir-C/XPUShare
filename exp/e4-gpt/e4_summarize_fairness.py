#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Summarize E4 fairness runs from one or more e4-fairness.csv files.

Each CSV is expected to contain:
  log,iter_per_sec
  e4-a.log,<num>
  e4-b.log,<num>
  ...
  jain_fairness,<num>    (optional)

We compute:
  - throughput ratio x_B/x_A
  - raw Jain index over [x_A, x_B]
  - weighted Jain index over normalized throughput y_i=x_i/w_i

Usage:
  python exp/e4_summarize_fairness.py --csv exp/e4-1/e4-fairness.csv exp/e4-2/e4-fairness.csv --wa 0.2 --wb 0.4
"""

from __future__ import annotations

import argparse
import csv
import statistics
import sys
from pathlib import Path
from typing import Dict, List, Tuple


def jain(xs: List[float]) -> float:
    n = len(xs)
    if n == 0:
        return 0.0
    s = sum(xs)
    ss = sum(x * x for x in xs)
    if ss <= 0:
        return 0.0
    return (s * s) / (n * ss)


def _looks_like_a(name: str) -> bool:
    n = (name or "").strip().lower()
    if n in ("a", "pod_a", "pod-a", "e4-a.log", "xpushare-e4-a"):
        return True
    return n.endswith(("-a.log", "_a.log"))


def _looks_like_b(name: str) -> bool:
    n = (name or "").strip().lower()
    if n in ("b", "pod_b", "pod-b", "e4-b.log", "xpushare-e4-b"):
        return True
    return n.endswith(("-b.log", "_b.log"))


def load_one(path: Path) -> Tuple[float, float]:
    xa = xb = None
    numeric_rows: List[Tuple[str, float]] = []
    with path.open(newline="", encoding="utf-8") as f:
        r = csv.reader(f)
        for row in r:
            if len(row) != 2:
                continue
            k, v = row[0].strip(), row[1].strip()
            if not v:
                continue
            try:
                fv = float(v)
            except ValueError:
                continue
            name = Path(k).name if k else k

            # Only treat per-pod throughput rows as candidates.
            # The CSV may also contain a summary row like "jain_fairness,<num>".
            if not name.lower().endswith(".log"):
                continue

            numeric_rows.append((name, fv))

            if xa is None and _looks_like_a(name):
                xa = fv
            elif xb is None and _looks_like_b(name):
                xb = fv

    # Fallback: if the CSV doesn't label A/B, but has exactly two numeric rows,
    # treat them as (A,B) in file order.
    if (xa is None or xb is None) and len(numeric_rows) >= 2:
        if xa is None:
            xa = numeric_rows[0][1]
        if xb is None:
            # pick the first row that is not the chosen xa row (handles duplicates)
            xb = numeric_rows[1][1]

    if xa is None or xb is None:
        raise ValueError(f"missing two throughput rows in {path} (got {len(numeric_rows)})")
    return float(xa), float(xb)


def median(xs: List[float]) -> float:
    return float(statistics.median(xs)) if xs else 0.0


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", nargs="+", required=True, help="Paths to e4-fairness.csv files")
    ap.add_argument("--wa", type=float, default=0.2, help="Weight for Pod A")
    ap.add_argument("--wb", type=float, default=0.4, help="Weight for Pod B")
    ap.add_argument(
        "--skip-invalid",
        action="store_true",
        help="Skip CSVs missing two throughput rows instead of failing.",
    )
    args = ap.parse_args(argv)

    rows: List[Dict[str, str]] = []
    ratios: List[float] = []
    jw: List[float] = []
    inversions = 0

    for p in [Path(x) for x in args.csv]:
        try:
            xa, xb = load_one(p)
        except Exception as e:
            if args.skip_invalid:
                print(f"[E4-SUM][SKIP] {p}: {type(e).__name__}: {e}", file=sys.stderr)
                continue
            raise
        ratio = xb / xa if xa > 0 else 0.0
        if ratio < 1.0:
            inversions += 1
        y = [xa / args.wa, xb / args.wb]
        jr = jain([xa, xb])
        jweighted = jain(y)
        ratios.append(ratio)
        jw.append(jweighted)
        rows.append(
            {
                "run": p.as_posix(),
                "xA": f"{xa:.6f}",
                "xB": f"{xb:.6f}",
                "ratio": f"{ratio:.3f}",
                "jain_raw": f"{jr:.3f}",
                "jain_weighted": f"{jweighted:.3f}",
            }
        )

    print("run,xA,xB,ratio,jain_raw,jain_weighted")
    for r in rows:
        print(",".join([r["run"], r["xA"], r["xB"], r["ratio"], r["jain_raw"], r["jain_weighted"]]))

    if ratios:
        ratios_sorted = sorted(ratios)
        jw_sorted = sorted(jw)
        print(f"[E4-SUM] n={len(ratios)} inversions={inversions}")
        print(f"[E4-SUM] ratio min/med/max = {ratios_sorted[0]:.3f}/{median(ratios_sorted):.3f}/{ratios_sorted[-1]:.3f}")
        print(f"[E4-SUM] weighted_jain min/med/max = {jw_sorted[0]:.3f}/{median(jw_sorted):.3f}/{jw_sorted[-1]:.3f}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
