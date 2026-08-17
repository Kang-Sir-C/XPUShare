# xhook — GPU Interception and Time-Slice Arbitration Engine

## Overview

xhook is the execution-side enforcement engine of XPUShare. It provides fine-grained GPU compute time-slicing and memory quota enforcement for containerized workloads via user-space API interception (`LD_PRELOAD`).

## Architecture

xhook consists of three components:

- **xhook-schd** (per-GPU scheduler): Manages time-slice tokens for all clients sharing a physical GPU. Uses EMA-based burst prediction, overuse-aware quota adjustment, and deficit-ratio priority scheduling to achieve proportional fairness.
- **xhook-pmgr** (per-Pod manager): Proxy between hook libraries and the scheduler. Aggregates burst/overuse statistics from multiple threads within a Pod and forwards quota requests.
- **libxhook.so.1** (hook library): Intercepts CUDA-like API calls via `LD_PRELOAD`. Requests time-slice tokens before kernel launches and enforces memory quotas on allocation calls.

Communication between components uses TCP sockets.

## Scheduling Algorithm

The time-slice scheduler implements a self-adaptive quota mechanism:

1. **Burst Prediction**: Each client's kernel burst duration is predicted using an Exponential Moving Average (EMA, α=0.3) combined with a decaying maximum envelope (half-life 500ms). This prevents outlier bursts from inflating predictions for extended periods.

2. **Overuse-Aware Quota**: The granted quota is adjusted by a feedback factor based on the client's historical overuse ratio. Clients that consistently exceed their allocation receive smaller quotas; under-utilizing clients receive slightly larger ones. The correction factor is clamped to [0.5, 1.3].

3. **Deficit-Ratio Priority**: When multiple clients compete for the GPU, priority is determined by `deficit_ratio = (require - usage) / require`, where `require` is the client's configured share × window size. This normalizes urgency by each client's own request, ensuring proportional convergence regardless of absolute weight magnitudes.

4. **Non-Blocking Dispatch**: After granting a token, the scheduler immediately returns to evaluate the next candidate rather than sleeping for the quota duration. This eliminates the serialization bottleneck that previously allowed low-weight clients to monopolize scheduling cycles.

## Multi-Backend Support

xhook supports multiple GPU backends via a pluggable architecture:

| Backend | Directory | GPU Vendors | Status |
|---------|-----------|-------------|--------|
| CUDA | `backends/cuda/` | Iluvatar CoreX, BiRen SUPA, NVIDIA | Production |
| MUSA | `backends/musa/` | Moore Threads | Skeleton |
| HIP | `backends/hip/` | Hygon DCU | Skeleton |
| Ascend | `backends/ascend/` | Huawei Ascend NPU | Skeleton (memory only) |

## Build

```bash
cd src

# CUDA backend (default)
make BACKEND=cuda CUDA_PATH=/usr/local/corex-4.3.6

# Other backends
make BACKEND=musa MUSA_PATH=/usr/local/musa
make BACKEND=hip HIP_PATH=/opt/dtk
make BACKEND=ascend ASCEND_PATH=/usr/local/Ascend/ascend-toolkit/latest

# Debug build (verbose scheduling logs)
make BACKEND=cuda CUDA_PATH=/usr/local/corex-4.3.6 DEBUG=1
```

Outputs:
- `lib/libxhook.so.1` — LD_PRELOAD interception library
- `bin/xhook-schd` — per-GPU time-slice scheduler
- `bin/xhook-pmgr` — per-Pod manager

## Resource Configuration

The scheduler reads a resource configuration file with the following format:

```
N
[ID] [REQUEST] [LIMIT] [GPU_MEM]
...
```

- `N`: number of clients
- `ID`: unique client name (ASCII, <63 chars)
- `REQUEST`: minimum guaranteed GPU time ratio (0~1.0)
- `LIMIT`: maximum allowed GPU time ratio (0~1.0)
- `GPU_MEM`: maximum GPU memory in bytes

Changes are hot-reloaded by `xhook-schd` (memory limits require client restart).

## Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `XPUSHARE_ENFORCEMENT_LAYER` | Enforcement mode: `runtime`, `driver`, `profile`, `none` | `driver` |
| `XPUSHARE_PROFILING` | Enable coverage profiling (`1` to enable) | disabled |
| `XPUSHARE_DRIVER_COVERAGE` | Count driver-boundary calls (`1` to enable) | disabled |
| `XPUSHARE_SHIM_COVERAGE` | Count runtime-boundary calls (`1` to enable) | disabled |
| `XPUSHARE_FORCE_REQ_QUOTA` | Debug: force quota request on every kernel | disabled |
| `XPUSHARE_EXTRA_DEBUG` | Extra debug logging | disabled |
| `POD_MANAGER_IP` | Pod manager IP address | `127.0.0.1` |
| `POD_MANAGER_PORT` | Pod manager port | `50052` |
| `SCHEDULER_IP` | Scheduler IP (for xhook-pmgr) | `127.0.0.1` |
| `SCHEDULER_PORT` | Scheduler port (for xhook-pmgr) | `50051` |

## Directory Structure

```
xhook/
├── src/
│   ├── core/                    # Backend-agnostic core logic
│   │   ├── scheduler.cpp/h      # Time-slice scheduler (xhook-schd)
│   │   ├── schd-priority.cpp    # Deficit-ratio priority comparator
│   │   ├── pod-manager.cpp      # Per-Pod manager (xhook-pmgr)
│   │   ├── predictor.cpp/h      # EMA burst predictor
│   │   ├── comm.cpp/h           # IPC protocol
│   │   ├── debug.cpp/h          # Logging
│   │   └── util.h               # Utilities
│   ├── backends/
│   │   ├── cuda/                # CUDA hook + shim
│   │   ├── musa/                # MUSA hook (skeleton)
│   │   ├── hip/                 # HIP hook (skeleton)
│   │   └── ascend/              # AscendCL shim (skeleton)
│   └── Makefile                 # Multi-target: make BACKEND=cuda|musa|hip|ascend
├── bin/                         # xhook-schd, xhook-pmgr
├── lib/                         # libxhook.so.1
└── tools/                       # Launcher scripts
```
