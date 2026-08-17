# XPUShare

A containerized cluster GPU sharing framework for closed-source CUDA-like heterogeneous platforms.

XPUShare decomposes GPU sharing into three loosely coupled layers:
- **Unified Device View** — cross-platform GPU discovery via pluggable Providers
- **Verifiable Binding Contract** — explicit, in-container verifiable device binding
- **Pluggable Execution Enforcement** — user-space interception with self-adaptive time-slice arbitration

## Features

- Fractional GPU allocation (≤1.0) and integer GPU allocation (>1)
- GPU heterogeneity and topology awareness
- Coscheduling (PodGroup)
- Multi-vendor support: Iluvatar CoreX, NVIDIA, Moore Threads (MUSA), MetaX (MACA), BiRen (SUPA), Hygon DCU (HIP), Huawei Ascend (AscendCL)
- Self-adaptive time-slice scheduling with EMA burst prediction, overuse-aware quota feedback, and deficit-ratio priority
- Configurable interception boundary (runtime / driver / profile / none) with mutual-exclusion enforcement

## Prerequisites

- Kubernetes 1.18+ cluster with DNS enabled
- GPU nodes with vendor-specific driver and runtime installed
- Prometheus (for GPU metrics collection)
- Go 1.16+
- Tested with Kubernetes v1.22.9 (Iluvatar BI-V150), v1.18.10 (NVIDIA)

## Quick Start

See [Deployment Guide](doc/deploy.md) for full instructions.

```bash
# 1. Label GPU nodes
kubectl label node <node-name> SharedGPU=true

# 2. Deploy components
kubectl apply -f crd/v1.yaml
kubectl apply -f deploy/xpushare-topology-configmap.yaml
kubectl apply -f deploy/collector.yaml
kubectl apply -f deploy/aggregator.yaml
kubectl apply -f deploy/node-daemon.yaml
kubectl apply -f deploy/scheduler.yaml
```

## Workloads

Pods declare GPU requirements via labels:

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: my-gpu-pod
  labels:
    sharedgpu/gpu_request: "0.3"    # guaranteed GPU compute share
    sharedgpu/gpu_limit: "0.5"      # maximum GPU compute share
    sharedgpu/gpu_mem: "4294967296" # memory limit in bytes
spec:
  schedulerName: xpushare-scheduler
  containers:
  - name: app
    image: my-gpu-app:latest
```

### Label Reference

| Label | Required | Description |
|-------|----------|-------------|
| `sharedgpu/gpu_request` | Yes | Guaranteed GPU compute ratio (0~1.0) |
| `sharedgpu/gpu_limit` | Yes | Maximum GPU compute ratio (request ≤ limit ≤ 1.0) |
| `sharedgpu/gpu_mem` | Yes | GPU memory limit (bytes) |
| `sharedgpu/gpu_model` | No | Target GPU model name |
| `sharedgpu/priority` | No | Priority 0~100 (0 = Opportunistic) |
| `sharedgpu/group_name` | No | Coscheduling group name |
| `sharedgpu/group_headcount` | No | Total Pods in group |
| `sharedgpu/group_threshold` | No | Minimum co-schedule ratio |

## Build

### Go Components

```bash
make all
```

Outputs in `bin/`: `xpushare-scheduler`, `xpushare-collector`, `xpushare-aggregator`, `xpushare-config`, `xpushare-query-ip`

### xhook Interception Engine

```bash
cd xhook/src

# CUDA backend (Iluvatar / BiRen / NVIDIA)
make BACKEND=cuda CUDA_PATH=/usr/local/corex-4.3.6

# MUSA backend (Moore Threads)
make BACKEND=musa MUSA_PATH=/usr/local/musa

# HIP backend (Hygon DCU)
make BACKEND=hip HIP_PATH=/opt/dtk

# Ascend backend (Huawei Ascend, memory isolation only)
make BACKEND=ascend ASCEND_PATH=/usr/local/Ascend/ascend-toolkit/latest
```

Outputs: `xhook/lib/libxhook.so.1`, `xhook/bin/xhook-schd`, `xhook/bin/xhook-pmgr`

### Docker Images

```bash
make build-image CONTAINER_NAME=xpushare-scheduler
make build-image CONTAINER_NAME=xhook-scheduler
make build-image CONTAINER_NAME=xhook-init
# ... etc
```

## GPU Isolation Engine

See [xhook/README.md](xhook/README.md) for the interception engine internals.

## Project Structure

```
XPUShare/
├── cmd/                          # Go entry points
│   ├── xpushare-scheduler/
│   ├── xpushare-collector/
│   ├── xpushare-aggregator/
│   ├── xpushare-config/
│   └── xpushare-query-ip/
├── pkg/                          # Go core libraries
│   ├── scheduler/                # Scheduling algorithms
│   ├── gpu/                      # GPU Provider abstraction
│   ├── aggregator/
│   ├── collector/
│   ├── config/
│   └── logger/
├── xhook/                        # GPU interception engine (C/C++)
│   └── src/
│       ├── core/                 # Backend-agnostic core
│       │   ├── scheduler.cpp     # xhook-schd (time-slice scheduler)
│       │   ├── pod-manager.cpp   # xhook-pmgr (per-Pod manager)
│       │   ├── predictor.cpp     # EMA burst predictor
│       │   ├── schd-priority.cpp # Deficit-ratio priority comparator
│       │   └── comm.cpp          # IPC protocol
│       └── backends/
│           ├── cuda/             # CUDA (Iluvatar/BiRen/NVIDIA)
│           ├── musa/             # MUSA (Moore Threads)
│           ├── hip/              # HIP (Hygon DCU)
│           └── ascend/           # AscendCL (Huawei Ascend)
├── deploy/                       # K8s manifests
├── docker/                       # Dockerfiles
├── crd/                          # CRD definitions
├── exp/                          # Experiment templates and scripts
└── doc/                          # Documentation
```
