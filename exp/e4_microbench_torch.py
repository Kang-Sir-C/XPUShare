#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=60, help="benchmark duration")
    ap.add_argument("--size", type=int, default=2048, help="matmul size (NxN)")
    ap.add_argument("--dtype", type=str, default="fp16", choices=["fp16", "fp32"])
    ap.add_argument("--warmup", type=int, default=3, help="warmup seconds")
    args = ap.parse_args()

    try:
        import torch  # type: ignore
    except Exception as e:
        print(f"[E4][ERROR] torch import failed: {e}")
        return 2

    if not torch.cuda.is_available():
        print("[E4][ERROR] torch.cuda.is_available() == False")
        return 3

    dev = torch.device("cuda:0")
    torch.cuda.set_device(dev)

    dtype = torch.float16 if args.dtype == "fp16" else torch.float32
    n = int(args.size)
    a = torch.randn((n, n), device=dev, dtype=dtype)
    b = torch.randn((n, n), device=dev, dtype=dtype)

    # Warmup
    t0 = time.time()
    warm_end = t0 + float(args.warmup)
    warm_iters = 0
    while time.time() < warm_end:
        _ = a @ b
        warm_iters += 1
    torch.cuda.synchronize()
    print(f"[E4] warmup_done iters={warm_iters} seconds={time.time()-t0:.3f}")

    # Run
    start = time.time()
    end = start + float(args.seconds)
    iters = 0
    last_report = start
    while time.time() < end:
        _ = a @ b
        iters += 1
        now = time.time()
        if now - last_report >= 1.0:
            torch.cuda.synchronize()
            elapsed = now - start
            ips = iters / elapsed if elapsed > 0 else 0.0
            print(f"[E4] progress elapsed_sec={elapsed:.1f} iters={iters} iter_per_sec={ips:.3f}")
            last_report = now

    torch.cuda.synchronize()
    duration = time.time() - start
    ips = iters / duration if duration > 0 else 0.0
    print(f"[E4] result seconds={duration:.3f} iters={iters} iter_per_sec={ips:.6f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

