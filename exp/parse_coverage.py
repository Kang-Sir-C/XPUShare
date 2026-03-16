#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Parse XPUShare/xhook coverage logs from /xpushare/log/hook.log.

The hook library emits lines like:
  [COVERAGE][driver] begin pid=1234
  [COVERAGE][driver] cuLaunchKernel=42
  [COVERAGE][driver] end pid=1234

and:
  [COVERAGE][runtime] begin pid=1234
  [COVERAGE][runtime] cudaMalloc=10
  [COVERAGE][runtime] end pid=1234

This script extracts blocks and exports a CSV suitable for Figure E1.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple


BEGIN_RE = re.compile(r"\[COVERAGE\]\[(driver|runtime)\]\s+begin(?:\s+pid=(\d+))?", re.I)
END_RE = re.compile(r"\[COVERAGE\]\[(driver|runtime)\]\s+end(?:\s+pid=(\d+))?", re.I)
KV_RE = re.compile(r"\[COVERAGE\]\[(driver|runtime)\]\s+([A-Za-z0-9_]+)=(\d+)", re.I)


@dataclass
class CoverageBlock:
    layer: str
    pid: str = ""
    counts: Dict[str, int] = field(default_factory=dict)


def parse_blocks(text: str) -> List[CoverageBlock]:
    blocks: List[CoverageBlock] = []
    current: Optional[CoverageBlock] = None

    for line in text.splitlines():
        m = BEGIN_RE.search(line)
        if m:
            current = CoverageBlock(layer=m.group(1).lower(), pid=m.group(2) or "")
            continue

        m = END_RE.search(line)
        if m:
            if current and current.layer == m.group(1).lower():
                # prefer pid from end marker if present
                if not current.pid:
                    current.pid = m.group(2) or ""
                blocks.append(current)
            current = None
            continue

        m = KV_RE.search(line)
        if m and current and current.layer == m.group(1).lower():
            sym = m.group(2)
            val = int(m.group(3))
            current.counts[sym] = val

    return blocks


def classify_symbol(symbol: str) -> str:
    s = symbol.lower()
    # Runtime symbols
    if s in ("cudamalloc", "cudafree"):
        return "memory_alloc"
    if s in ("cudamemgetinfo", "cudagetdeviceproperties"):
        return "memory_view"
    if s in ("cudalaunchkernel", "cudalaunch"):
        return "kernel_launch"
    # Driver symbols
    if s in ("cumemalloc", "cumemalloc_v2", "cumemallocmanaged", "cumemallocmanaged_v2", "cumemallocpitch"):
        return "memory_alloc"
    if s in ("cumemfree",):
        return "memory_alloc"
    if s in ("cumemgetinfo", "cudevicetotalmem"):
        return "memory_view"
    if s in ("culaunchkernel", "culaunchkernel_v2", "culaunchcooperativekernel"):
        return "kernel_launch"
    if s.startswith("culaunchkernel") or s.startswith("culaunch"):
        return "kernel_launch"
    if s.startswith("cumem"):
        return "memory_alloc"
    if s.startswith("cuctx"):
        return "context"
    if s.startswith("cugetprocaddress"):
        return "init"
    return "other"


def aggregate_by_category(block: CoverageBlock) -> Dict[str, int]:
    out: Dict[str, int] = {}
    for sym, val in block.counts.items():
        cat = classify_symbol(sym)
        out[cat] = out.get(cat, 0) + int(val)
    return out


def latest_per_layer(blocks: List[CoverageBlock]) -> Dict[str, CoverageBlock]:
    out: Dict[str, CoverageBlock] = {}
    for b in blocks:
        out[b.layer] = b
    return out


def write_csv_long(blocks: List[CoverageBlock], out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["run_id", "layer", "pid", "category", "symbol", "count"])
        for run_id, b in enumerate(blocks, start=1):
            for sym, val in sorted(b.counts.items()):
                w.writerow([run_id, b.layer, b.pid, classify_symbol(sym), sym, val])


def print_summary(latest: Dict[str, CoverageBlock]) -> None:
    layers = ["driver", "runtime"]
    symbols = set()
    for layer in layers:
        b = latest.get(layer)
        if b:
            symbols.update(b.counts.keys())

    symbols_sorted = sorted(symbols)
    print("symbol,driver,runtime")
    for s in symbols_sorted:
        d = latest.get("driver").counts.get(s, 0) if latest.get("driver") else 0
        r = latest.get("runtime").counts.get(s, 0) if latest.get("runtime") else 0
        print(f"{s},{d},{r}")


def print_category_summary(latest: Dict[str, CoverageBlock]) -> None:
    layers = ["driver", "runtime"]
    cats = set()
    per_layer: Dict[str, Dict[str, int]] = {}
    for layer in layers:
        b = latest.get(layer)
        if not b:
            continue
        per_layer[layer] = aggregate_by_category(b)
        cats.update(per_layer[layer].keys())

    cats_sorted = sorted(cats)
    print("category,driver,runtime")
    for c in cats_sorted:
        d = per_layer.get("driver", {}).get(c, 0)
        r = per_layer.get("runtime", {}).get(c, 0)
        print(f"{c},{d},{r}")


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True, help="Path to hook.log")
    ap.add_argument("--out", default="", help="Optional output CSV path (long format)")
    ap.add_argument(
        "--mode",
        choices=["all", "latest", "category"],
        default="latest",
        help="all=export all blocks; latest=latest symbol counts; category=latest aggregated by category",
    )
    args = ap.parse_args(argv)

    p = Path(args.log)
    if not p.exists():
        print(f"error: log not found: {p}", file=sys.stderr)
        return 2

    text = p.read_text(encoding="utf-8", errors="replace")
    blocks = parse_blocks(text)
    if not blocks:
        print("error: no coverage blocks found", file=sys.stderr)
        return 3

    if args.out:
        write_csv_long(blocks, Path(args.out))

    latest = latest_per_layer(blocks)
    if args.mode == "category":
        print_category_summary(latest)
    else:
        # For "all", still print a latest summary to stdout, and rely on CSV for details.
        print_summary(latest)

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
