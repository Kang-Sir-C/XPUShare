#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import time


def fmt_bytes(n: int) -> str:
    for unit in ["B", "KiB", "MiB", "GiB", "TiB"]:
        if n < 1024 or unit == "TiB":
            return f"{n:.0f}{unit}" if unit == "B" else f"{n/1024:.2f}{unit}"
        n /= 1024
    return str(n)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--step-mib", type=int, default=256, help="allocation step (MiB)")
    ap.add_argument("--max-steps", type=int, default=128, help="max allocation steps")
    ap.add_argument("--sleep", type=float, default=0.5, help="sleep seconds between steps")
    args = ap.parse_args()

    try:
        import torch  # type: ignore
    except Exception as e:
        print(f"[E3][ERROR] torch import failed: {e}")
        return 2

    if not torch.cuda.is_available():
        print("[E3][ERROR] torch.cuda.is_available() == False")
        return 3

    dev = torch.device("cuda:0")
    torch.cuda.set_device(dev)

    props = torch.cuda.get_device_properties(0)
    print(f"[E3] device_name={props.name}")
    print(f"[E3] device_total_memory={props.total_memory}")

    def mem_info():
        free_b, total_b = torch.cuda.mem_get_info()
        return int(free_b), int(total_b)

    free0, total0 = mem_info()
    print(f"[E3] mem_get_info_begin free={free0} total={total0}")

    allocations = []
    step_bytes = int(args.step_mib) * 1024 * 1024

    for i in range(args.max_steps):
        want = (i + 1) * step_bytes
        try:
            t0 = time.time()
            x = torch.empty((want // 4,), dtype=torch.float32, device=dev)
            allocations.append(x)
            torch.cuda.synchronize()
            dt = time.time() - t0
            free_b, total_b = mem_info()
            print(
                f"[E3] step={i+1} alloc={want} ({fmt_bytes(want)}) ok=1 "
                f"free={free_b} total={total_b} alloc_time_sec={dt:.4f}"
            )
        except Exception as e:
            free_b, total_b = mem_info()
            print(
                f"[E3] step={i+1} alloc={want} ({fmt_bytes(want)}) ok=0 "
                f"free={free_b} total={total_b} err={type(e).__name__}:{e}"
            )
            break
        time.sleep(args.sleep)

    print("[E3] done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

