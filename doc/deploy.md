# XPUShare 部署文档

## 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│  K8s Master                                                     │
│  ┌──────────────────┐  ┌──────────────────┐                     │
│  │ xpushare-scheduler│  │xpushare-aggregator│                    │
│  └────────┬─────────┘  └────────┬─────────┘                     │
│           │ (调度决策)           │ (聚合 GPU 指标)                │
├───────────┼─────────────────────┼───────────────────────────────┤
│  GPU Node │                     │                               │
│  ┌────────┴─────────┐  ┌───────┴──────────┐                    │
│  │ xpushare-config   │  │ xpushare-collector│ (GPU 发现+指标)   │
│  └────────┬─────────┘  └──────────────────┘                     │
│           │                                                     │
│  ┌────────┴─────────┐                                           │
│  │ xhook-scheduler   │  (per-GPU 时间片调度器: xhook-schd)      │
│  │ xhook-pmgr        │  (per-Pod 管理器)                        │
│  └──────────────────┘                                           │
│           │                                                     │
│  ┌────────┴─────────┐                                           │
│  │ 用户 Pod          │  (LD_PRELOAD libxhook.so.1)              │
│  └──────────────────┘                                           │
└─────────────────────────────────────────────────────────────────┘
```

## 组件说明

| 组件 | 类型 | 说明 |
|------|------|------|
| `xpushare-scheduler` | Deployment | K8s 调度框架插件，选择节点 + GPU |
| `xpushare-collector` | DaemonSet | GPU 发现，暴露 Prometheus 指标 |
| `xpushare-aggregator` | Deployment | 聚合各节点 GPU 指标 |
| `xpushare-config` | DaemonSet (sidecar) | 为 xhook 更新配置文件 |
| `xpushare-query-ip` | init container | 为 xhook 注入节点 IP |
| `xhook-schd` | per-GPU daemon | 节点级 GPU 时间片调度 |
| `xhook-pmgr` | per-Pod daemon | Pod 级 GPU 管理 |
| `libxhook.so.1` | LD_PRELOAD 库 | 拦截 GPU Runtime/Driver API |

## 前置条件

- Kubernetes 1.18+
- GPU 节点已安装对应厂商的 GPU 驱动和 Runtime
- Prometheus（用于 GPU 指标采集）

## 支持的 GPU 后端

| 厂商 | Provider 名称 | 可见设备环境变量 | 拦截层 |
|------|--------------|----------------|--------|
| 天数智芯 (Iluvatar) | `iluvatar` | `IX_VISIBLE_DEVICES` | runtime |
| NVIDIA | `nvidia-smi` | `NVIDIA_VISIBLE_DEVICES` | driver |
| 摩尔线程 (Moore Threads) | `mthreads-gmi` | `MUSA_VISIBLE_DEVICES` | runtime |
| 沐曦 (MetaX) | `metax-mxsmi` | `METAX_VISIBLE_DEVICES` | driver |
| 壁仞 (BiRen) | `biren-brsmi` | `CUDA_VISIBLE_DEVICES` | driver |
| 海光 DCU (Hygon) | `hygon-rocmsmi` | `HIP_VISIBLE_DEVICES` | runtime |
| 华为昇腾 (Ascend) | `ascend-npusmi` | `ASCEND_RT_VISIBLE_DEVICES` | runtime |
| 通用 | `generic-cmd` | 自定义 | 自定义 |

## 配置

### 1. 标记 GPU 节点

```bash
kubectl label node <node-name> SharedGPU=true
```

### 2. GPU 拓扑配置

编辑 `deploy/config/xpushare-config.yaml`，描述集群 GPU 拓扑：

```yaml
cellTypes:
  # 叶子节点的 childCellType 必须是 GPU 型号名（空格替换为 -）
  BI-V150-NODE:
    childCellType: "Iluvatar-BI-V150"
    childCellNumber: 16
    childCellPriority: 1
    isNodeLevel: true

cells:
- cellType: BI-V150-NODE
  cellId: gpu-node-01    # 对应 K8s node name
```

GPU 型号名可通过以下工具查看：
- 天数智芯: `ixsmi -L`
- NVIDIA: `nvidia-smi -L`
- 摩尔线程: `mthreads-gmi`
- 海光 DCU: `rocm-smi --showproductname` 或 `hy-smi`
- 华为昇腾: `npu-smi info`

创建 ConfigMap：
```bash
kubectl apply -f deploy/xpushare-topology-configmap.yaml
```

### 3. 配置 GPU Provider

在 `deploy/collector.yaml` 和 `deploy/scheduler.yaml` 中设置环境变量：

```yaml
env:
- name: XPU_PROVIDER
  value: "iluvatar"              # 选择对应厂商
- name: XPU_VISIBLE_DEVICES_ENV  # 可选，覆盖默认值
  value: "IX_VISIBLE_DEVICES"
- name: XPU_VISIBLE_DEVICES_VALUE  # 可选: uuid 或 index
  value: "index"
```

## 编译

### Go 组件

```bash
make all
```

产物在 `bin/` 目录：`xpushare-scheduler`, `xpushare-collector`, `xpushare-aggregator`, `xpushare-config`, `xpushare-query-ip`

### xhook 拦截引擎

```bash
cd xhook/src

# CUDA 后端（天数智芯 / 壁仞 / NVIDIA）
make BACKEND=cuda CUDA_PATH=/usr/local/corex-4.3.6

# MUSA 后端（摩尔线程）
make BACKEND=musa MUSA_PATH=/usr/local/musa

# HIP 后端（海光 DCU）
make BACKEND=hip HIP_PATH=/opt/dtk

# Ascend 后端（华为昇腾，仅显存隔离）
make BACKEND=ascend ASCEND_PATH=/usr/local/Ascend/ascend-toolkit/latest
```

产物：
- `xhook/lib/libxhook.so.1` — LD_PRELOAD 拦截库
- `xhook/bin/xhook-schd` — per-GPU 调度器
- `xhook/bin/xhook-pmgr` — per-Pod 管理器

### 构建容器镜像（containerd + nerdctl）

集群使用 containerd 作为容器运行时，使用 `nerdctl` 直接在节点上构建镜像到 `k8s.io` namespace。

安装 nerdctl（如尚未安装）：
```bash
sudo tar -C /usr/local/bin -xzvf nerdctl-2.0.0-linux-arm64.tar.gz nerdctl
```

推荐设置别名简化操作：
```bash
alias nb='sudo nerdctl --namespace k8s.io build -t'
alias ni='sudo nerdctl -n k8s.io images'
alias nt='sudo nerdctl -n k8s.io tag'
alias np='sudo nerdctl -n k8s.io pull'
alias nd='sudo nerdctl -n k8s.io rmi -f'
```

构建所有组件镜像（`<tag>` 替换为版本标签，如 `0227-1`）：
```bash
# Go 组件镜像
nb xpushare-scheduler:<tag> -f docker/Dockerfile.scheduler .
nb xpushare-collector:<tag> -f docker/Dockerfile.collector .
nb xpushare-aggregator:<tag> -f docker/Dockerfile.aggregator .
nb xpushare-config:<tag> -f docker/Dockerfile.config .
nb xpushare-query-ip:<tag> -f docker/Dockerfile.query-ip .

# xhook 镜像
nb xhook-scheduler:<tag> -f docker/xhook-scheduler/Dockerfile .
nb xhook-init:<tag> -f docker/xhook-init/Dockerfile .
```

验证镜像：
```bash
ni | grep -E 'xpushare|xhook'
```

也可以通过 Makefile 构建（底层同样调用 nerdctl）：
```bash
make build-image CONTAINER_NAME=xpushare-scheduler CONTAINER_VERSION=<tag>
```

## 部署

按以下顺序部署：

```bash
# 1. CRD
kubectl apply -f crd/v1.yaml

# 2. 拓扑配置
kubectl apply -f deploy/xpushare-topology-configmap.yaml

# 3. Collector + Aggregator（GPU 发现和指标）
kubectl apply -f deploy/collector.yaml
kubectl apply -f deploy/aggregator.yaml

# 4. Node Daemon（xhook 调度器 + config + query-ip）
# 先确认 prometheusURL 指向集群已有的 Prometheus（如 prometheus.icps-monitor.svc:9090）
kubectl apply -f deploy/node-daemon.yaml

# 5. Scheduler
# 同样确认 prometheusURL 参数
kubectl apply -f deploy/scheduler.yaml
```

## 验证

```bash
# 检查所有组件是否运行
kubectl get pod -A | grep xpushare

# 检查 Prometheus 指标
curl http://<prometheus>:9090/api/v1/query?query=gpu_capacity
```

## 提交共享 GPU Pod

Pod 通过 labels 声明 GPU 需求：

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: my-gpu-pod
  labels:
    sharedgpu/gpu_request: "0.3"    # GPU 算力保证 30%
    sharedgpu/gpu_limit: "0.5"      # GPU 算力上限 50%
    sharedgpu/gpu_mem: "4294967296" # 显存上限 4GB
spec:
  schedulerName: xpushare-scheduler
  containers:
  - name: app
    image: my-gpu-app:latest
    resources:
      limits:
        cpu: "1"
        memory: "4Gi"
```

### Label 说明

| Label | 必填 | 说明 |
|-------|------|------|
| `sharedgpu/gpu_request` | 是 | GPU 算力保证比例 (0~1.0) |
| `sharedgpu/gpu_limit` | 是 | GPU 算力上限 (request ≤ limit ≤ 1.0) |
| `sharedgpu/gpu_mem` | 是 | 显存上限 (bytes) |
| `sharedgpu/gpu_model` | 否 | 指定 GPU 型号 |
| `sharedgpu/priority` | 否 | 优先级 0~100 (0=Opportunistic) |
| `sharedgpu/group_name` | 否 | 协同调度组名 |
| `sharedgpu/group_headcount` | 否 | 组内 Pod 总数 |
| `sharedgpu/group_threshold` | 否 | 最小同时调度比例 |

## 目录结构

```
XPUShare/
├── cmd/                          # Go 入口点
│   ├── xpushare-scheduler/
│   ├── xpushare-collector/
│   ├── xpushare-aggregator/
│   ├── xpushare-config/
│   └── xpushare-query-ip/
├── pkg/                          # Go 核心库
│   ├── scheduler/                # 调度算法
│   ├── gpu/                      # GPU Provider 抽象层
│   ├── aggregator/
│   ├── collector/
│   ├── config/
│   └── logger/
├── xhook/                        # GPU 拦截引擎 (C/C++)
│   └── src/
│       ├── core/                 # 后端无关核心逻辑
│       │   ├── scheduler.cpp     # xhook-schd
│       │   ├── pod-manager.cpp   # xhook-pmgr
│       │   ├── predictor.cpp     # burst predictor
│       │   └── comm.cpp          # 通信协议
│       └── backends/
│           ├── cuda/             # CUDA (天数智芯/壁仞/NVIDIA)
│           ├── musa/             # MUSA (摩尔线程)
│           ├── hip/              # HIP (海光 DCU)
│           └── ascend/           # AscendCL (华为昇腾)
├── deploy/                       # K8s 部署清单
├── docker/                       # Dockerfile
├── crd/                          # CRD 定义
└── test/                         # 测试 YAML
```
