# 国产 GPU 虚拟化方案调研与 XPUShare 扩展可行性评估

> 调研范围：壁仞 (BiRen)、摩尔线程 (Moore Threads)、海光 DCU (Hygon)、华为昇腾 (Ascend)
> 对比基线：天数智芯 (Iluvatar CoreX) — XPUShare 当前已适配平台

---

## 一、XPUShare 现有架构概述

XPUShare 基于 KubeShare 2.0 演化，在 Kubernetes 上将物理 GPU 以分数形式分配给多个 Pod。

### 1.1 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│                    Kubernetes Cluster                            │
│                                                                 │
│  ┌──────────────────┐    Prometheus    ┌──────────────────────┐ │
│  │  kubeshare-       │◄──── scrape ───►│  kubeshare-          │ │
│  │  scheduler        │                 │  aggregator          │ │
│  │  (调度框架插件)    │                 │  (Pod GPU需求聚合)    │ │
│  └────────┬─────────┘                  └──────────────────────┘ │
│           │ 调度决策: 选节点+GPU                                  │
│           ▼                                                     │
│  ┌──────────────────────── 每个 GPU 节点 (DaemonSet) ──────────┐ │
│  │  ┌─────────────┐  ┌──────────────┐  ┌───────────────────┐  │ │
│  │  │ collector   │  │ config       │  │ gemini-scheduler  │  │ │
│  │  │ (GPU发现→   │  │ (Pod→Gemini  │  │ (gem-schd +       │  │ │
│  │  │  Prometheus)│  │  配置文件)    │  │  gem-pmgr)        │  │ │
│  │  └─────────────┘  └──────────────┘  └────────┬──────────┘  │ │
│  │                                              │ TCP socket  │ │
│  │  ┌───────────────────────────────────────────▼──────────┐  │ │
│  │  │              用户 Pod 容器                             │  │ │
│  │  │  LD_PRELOAD=libgemhook.so.1                          │  │ │
│  │  │  ┌─────────────┐  ┌──────────┐                      │  │ │
│  │  │  │ hook.cpp     │  │ shim.c   │                      │  │ │
│  │  │  │ (Driver API  │  │ (Runtime │                      │  │ │
│  │  │  │  拦截)       │  │  API拦截) │                      │  │ │
│  │  │  └─────────────┘  └──────────┘                      │  │ │
│  │  └──────────────────────────────────────────────────────┘  │ │
│  └─────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### 1.2 扩展新厂商需要改动的三个层次

| 层次 | 内容 | 改动量 | 关键文件 |
|------|------|--------|----------|
| **L1: 设备发现 (Go)** | GPU Provider 接口实现 | 小 | `pkg/gpu/providers_*.go`, `config.go` |
| **L2: API 拦截 (C/C++)** | LD_PRELOAD hook 库 | **大** | `Gemini/src/hook.cpp`, `shim.c`, `hook.h`, `Makefile` |
| **L3: K8s 集成 (Go)** | 环境变量注入、容器镜像 | 小 | `pkg/scheduler/pod.go`, `deploy/` |

其中 L1 和 L3 已有良好的抽象（`Provider` 接口 + `Config` 环境变量驱动），**L2 是核心挑战**。

### 1.3 当前 API 拦截机制

XPUShare 通过 `LD_PRELOAD` 注入 `libgemhook.so.1`，提供两层拦截：

- **Driver 层** (`hook.cpp`): 拦截 CUDA Driver API (`cu*` 函数)，通过 `dlsym(RTLD_NEXT)` + `cuGetProcAddress` 替换
- **Runtime 层** (`shim.c`): 拦截 CUDA Runtime API (`cuda*` 函数)，专为天数智芯 CoreX 设计
- 通过 TLS 标志 `g_kubeshare_in_runtime_shim` 防止 runtime→driver 调用链的双重拦截
- 环境变量 `KUBESHARE_ENFORCEMENT_LAYER` 选择在哪一层执行配额控制

**拦截的关键函数类别**：

| 类别 | Driver API (hook.cpp) | Runtime API (shim.c) |
|------|----------------------|---------------------|
| 显存分配 | `cuMemAlloc`, `cuMemFree`, `cuMemAllocManaged`, `cuMemAllocPitch` | `cudaMalloc`, `cudaFree` |
| 显存查询 | `cuMemGetInfo`, `cuDeviceTotalMem` | `cudaMemGetInfo`, `cudaGetDeviceProperties` |
| Kernel 发射 | `cuLaunchKernel`, `cuLaunchCooperativeKernel` | `cudaLaunchKernel`, `cudaLaunch` |
| 同步 | `cuCtxSynchronize` | `cudaDeviceSynchronize`, `cudaStreamSynchronize` |
| 数据传输 | `cuMemcpyHtoD`, `cuMemcpyDtoH`, `cuMemcpyAtoH`, `cuMemcpyHtoA` | — |

---

## 二、各厂商 Runtime API 结构总览

| 维度 | 天数智芯 CoreX | 壁仞 SUPA | 摩尔线程 MUSA | 海光 DCU (HIP) | 昇腾 AscendCL |
|------|---------------|-----------|--------------|---------------|--------------|
| **Runtime 库** | `libcudart.so` (兼容) | `libcudart.so` (兼容，推测) | `libmusa_runtime.so` | `libamdhip64.so` | `libascendcl.so` |
| **Driver 库** | `libcuda.so` + `libixthunk.so` | `libcuda.so` (兼容，推测) | `libmu_driver.so` (推测) | `libhsa-runtime64.so` | 无独立 driver 层 |
| **Malloc** | `cudaMalloc` / `cuMemAlloc` | `cudaMalloc` / `cuMemAlloc` | `musaMalloc` / `muMemAlloc` | `hipMalloc` / `hipDrvMemAlloc` | `aclrtMalloc` |
| **Free** | `cudaFree` / `cuMemFree` | `cudaFree` / `cuMemFree` | `musaFree` / `muMemFree` | `hipFree` | `aclrtFree` |
| **Kernel Launch** | `cudaLaunchKernel` / `cuLaunchKernel` | `cudaLaunchKernel` / `cuLaunchKernel` | `musaLaunchKernel` | `hipLaunchKernel` / `hipModuleLaunchKernel` | `aclrtLaunch` (算子级) |
| **MemInfo** | `cudaMemGetInfo` / `cuMemGetInfo` | `cudaMemGetInfo` / `cuMemGetInfo` | `musaMemGetInfo` | `hipMemGetInfo` | `aclrtGetMemInfo` |
| **Synchronize** | `cudaDeviceSynchronize` / `cuCtxSynchronize` | `cudaDeviceSynchronize` | `musaDeviceSynchronize` | `hipDeviceSynchronize` | `aclrtSynchronizeStream` |
| **编译器** | `clang++` (CoreX) | 未公开 | `mcc` | `hipcc` | Ascend C 编译器 |
| **设备管理工具** | `ixsmi` | `brsmi` | `mthreads-gmi` | `rocm-smi` / `hy-smi` | `npu-smi` |
| **可见设备环境变量** | `IX_VISIBLE_DEVICES` | `CUDA_VISIBLE_DEVICES` (兼容) | `MUSA_VISIBLE_DEVICES` | `HIP_VISIBLE_DEVICES` / `ROCR_VISIBLE_DEVICES` | `ASCEND_RT_VISIBLE_DEVICES` |
| **K8s 资源名** | `iluvatar.ai/gpu` | `biren.com/gpu` | `mthreads.com/gpu` | `hygon.com/dcu` | `huawei.com/Ascend910` |
| **CUDA 兼容度** | ★★★★★ 二进制兼容 | ★★★★☆ 高度兼容 | ★★☆☆☆ API 同构但需重编译 | ★★☆☆☆ HIP 需移植 | ☆☆☆☆☆ 完全不同 |

---

## 三、壁仞 (BiRen) — SUPA 平台

### 3.1 平台概况

壁仞科技自研 SUPA (Software Unified Programming Architecture) 软件栈，硬件产品包括 BR100（双芯片设计，77B 晶体管，7nm，64GB HBM2E）、BR104（推理卡）、BR106 系列。

**核心策略**：高度 CUDA 兼容。壁仞声称 SUPA 平台可以运行 CUDA 代码，API 层面保持与 NVIDIA CUDA 的兼容性。

### 3.2 Runtime API 结构

- **兼容路线**：SUPA 提供 CUDA 兼容的 runtime 和 driver API
- 函数命名沿用 `cuda*` / `cu*` 前缀（与 NVIDIA 一致）
- 提供自己的 `libcuda.so` 和 `libcudart.so` 替代品
- 应用程序理论上无需修改源码即可运行

### 3.3 设备管理

- **SMI 工具**：`brsmi` — 输出 GPU 信息（UUID、温度、显存使用等）
- XPUShare 已有 `providers_biren_brsmi.go` 解析 `brsmi` 输出

### 3.4 Device Plugin

- **官方插件**：`BirenTechnology/k8s-device-plugin`（GitHub）
- 资源名：`biren.com/gpu`
- HAMi 项目目前**未列出**壁仞的支持

### 3.5 与天数智芯的异同

| 维度 | 天数智芯 | 壁仞 |
|------|---------|------|
| CUDA 兼容度 | 二进制级兼容 | 高度兼容（具体程度待验证） |
| Driver 层 | `libixthunk.so` | 未公开（推测有类似 thunk 层） |
| SDK 公开度 | 较好（go-ixml 开源） | **极低**，SDK 文档不公开 |
| LD_PRELOAD 可行性 | 已验证 | 理论可行，需实机验证 |

### 3.6 关键风险

- ⚠️ **SDK 不公开**：SUPA SDK 文档和头文件未对外发布，无法确认 API 细节
- ⚠️ **制裁影响**：壁仞受美国出口管制影响，硬件获取和生态发展受限
- ⚠️ `cuGetProcAddress` 兼容性未知：hook.cpp 中的 CUDA 11.3+ 动态符号查找替换是否在 SUPA 上工作需要验证

---

## 四、摩尔线程 (Moore Threads) — MUSA 平台

### 4.1 平台概况

摩尔线程自研 MUSA (Moore Threads Unified System Architecture) 软件栈，硬件产品包括 MTT S80（消费级）、MTT S4000（数据中心）等。MUSA 在 API 结构上模仿 CUDA，但使用独立的命名空间。

### 4.2 Runtime API 结构

MUSA 的 API 命名规则是将 CUDA 的 `cuda` 前缀替换为 `musa`：

```c
// CUDA Runtime API          →  MUSA Runtime API
cudaMalloc(&ptr, size)        →  musaMalloc(&ptr, size)
cudaFree(ptr)                 →  musaFree(ptr)
cudaMemcpy(dst, src, n, kind) →  musaMemcpy(dst, src, n, kind)
cudaLaunchKernel(...)         →  musaLaunchKernel(...)
cudaMemGetInfo(&free, &total) →  musaMemGetInfo(&free, &total)
cudaSetDevice(dev)            →  musaSetDevice(dev)
cudaGetDeviceCount(&count)    →  musaGetDeviceCount(&count)
cudaDeviceSynchronize()       →  musaDeviceSynchronize()
cudaStreamSynchronize(stream) →  musaStreamSynchronize(stream)
cudaGetDeviceProperties(...)  →  musaGetDeviceProperties(...)
```

**Driver API**（推测）：`mu*` 前缀，如 `muMemAlloc`, `muLaunchKernel` 等。

### 4.3 MUSIFY 移植工具

摩尔线程提供 `musify` 工具，自动将 CUDA 代码转换为 MUSA 代码：
- 头文件替换：`cuda_runtime.h` → `musa_runtime.h`
- API 名称替换：`cuda*` → `musa*`
- 类型替换：`cudaError_t` → `musaError_t`, `cudaStream_t` → `musaStream_t`

### 4.4 专用库生态

| CUDA 库 | MUSA 对应 |
|---------|----------|
| cuBLAS | muBLAS |
| cuFFT | muFFT |
| cuDNN | muDNN |
| Thrust | muThrust |
| NCCL | muCCL |

### 4.5 设备管理

- **SMI 工具**：`mthreads-gmi` — 输出格式类似 nvidia-smi
- XPUShare 已有 `providers_mthreads_gmi.go` 解析其输出

### 4.6 环境变量

- `MUSA_VISIBLE_DEVICES` — 用户态 runtime 层设备选择
- `MTHREADS_VISIBLE_DEVICES` — 容器运行时层设备选择（类似 `NVIDIA_VISIBLE_DEVICES`）
- XPUShare `config.go` 已配置默认使用 `MUSA_VISIBLE_DEVICES`

### 4.7 容器运行时与 Device Plugin

- **容器运行时**：`mthreads` container runtime（类似 nvidia-container-runtime）
- **K8s 设备插件**：`mt-gpu-device-plugin`
- **HAMi 支持**：已支持 MTT S4000，提供显存隔离 + 算力隔离

### 4.8 与天数智芯的异同

| 维度 | 天数智芯 | 摩尔线程 |
|------|---------|---------|
| CUDA 兼容策略 | 二进制兼容（同名 API） | API 同构但独立命名空间（`musa*`） |
| 源码移植 | 无需修改 | 需要 `musify` 工具转换 |
| hook.cpp 复用 | 直接使用 | **不可复用**，需要 `mu*` 版本 |
| shim.c 复用 | 直接使用 | **不可复用**，需要 `musa*` 版本 |
| 拦截难度 | 低 | 中等（API 结构一致，仅名称不同） |

### 4.9 XPUShare 适配方案

需要编写 `shim_musa.c` 和 `hook_musa.cpp`，拦截以下函数：

```c
// shim_musa.c 需要拦截的 Runtime API
musaMalloc, musaFree, musaLaunchKernel, musaLaunch,
musaMemGetInfo, musaGetDeviceProperties,
musaDeviceSynchronize, musaStreamSynchronize

// hook_musa.cpp 需要拦截的 Driver API
muMemAlloc, muMemFree, muLaunchKernel,
muMemGetInfo, muDeviceTotalMem, muCtxSynchronize,
muMemcpyHtoD, muMemcpyDtoH, muGetProcAddress
```

由于 API 结构与 CUDA 完全同构，可以通过**宏/模板**从现有代码生成，工作量可控。

---

## 五、海光 DCU (Hygon) — ROCm/HIP 路线

### 5.1 平台概况

海光 DCU (Deep Computing Unit) 基于 AMD 的 GCN/CDNA 架构授权，软件栈基于 ROCm 生态，使用 HIP (Heterogeneous-computing Interface for Portability) 作为编程接口。主要产品包括 Z100、Z100L 等。

### 5.2 Runtime API 结构

HIP Runtime API 与 CUDA Runtime API 高度对应，命名规则是 `cuda` → `hip`：

```c
// CUDA Runtime API          →  HIP Runtime API
cudaMalloc(&ptr, size)        →  hipMalloc(&ptr, size)
cudaFree(ptr)                 →  hipFree(ptr)
cudaMemcpy(dst, src, n, kind) →  hipMemcpy(dst, src, n, kind)
cudaLaunchKernel(...)         →  hipLaunchKernel(...)
cudaMemGetInfo(&free, &total) →  hipMemGetInfo(&free, &total)
cudaSetDevice(dev)            →  hipSetDevice(dev)
cudaGetDeviceCount(&count)    →  hipGetDeviceCount(&count)
cudaDeviceSynchronize()       →  hipDeviceSynchronize()
cudaStreamSynchronize(stream) →  hipStreamSynchronize(stream)
cudaGetDeviceProperties(...)  →  hipGetDeviceProperties(...)
```

### 5.3 Driver API 层

HIP/ROCm 的 driver 层与 NVIDIA 有显著差异：

- **无独立 `cu*` 风格 Driver API**：ROCm 底层是 HSA (Heterogeneous System Architecture) runtime
- Driver 层库：`libhsa-runtime64.so`
- HIP 同时包含了 runtime 和部分 driver 功能，不像 NVIDIA 那样有清晰的 `libcudart.so` / `libcuda.so` 分层
- Module/Context API：`hipModuleLoad`, `hipModuleLaunchKernel`, `hipCtxCreate` 等

### 5.4 设备管理

- **SMI 工具**：`rocm-smi`（标准 ROCm）或 `hy-smi`（海光定制版）
- XPUShare 目前**没有**海光 DCU 的 provider，需要新增

### 5.5 环境变量（多层级）

| 环境变量 | 层级 | 说明 |
|---------|------|------|
| `ROCR_VISIBLE_DEVICES` | ROCm 软件运行时层 | 支持设备索引和 UUID |
| `HIP_VISIBLE_DEVICES` | HIP 运行时层 | 仅设备索引 |
| `GPU_DEVICE_ORDINAL` | ROCclr 层 | 影响 OpenCL 和 HIP |

推荐使用 `HIP_VISIBLE_DEVICES`（与 XPUShare 的 index 模式匹配）。

### 5.6 容器运行时与 Device Plugin

- **官方设备插件**：`ROCm/k8s-device-plugin`（AMD 官方）
- **HAMi 支持**：已支持海光 Z100/Z100L，提供显存隔离 + 算力隔离 + DCU 共享
  - 资源名：`hygon.com/dcu`
  - 共享控制：`hygon.com/dcunum`, `hygon.com/dcucores`, `hygon.com/dcumem`
  - HAMi 为 DCU 提供 vDCU 功能，通过创建 vdev 文件实现虚拟化

### 5.7 与天数智芯的异同

| 维度 | 天数智芯 | 海光 DCU |
|------|---------|---------|
| 基础架构 | 自研（CUDA 兼容） | AMD GCN/CDNA 授权 |
| API 命名 | `cuda*` / `cu*` (兼容) | `hip*` (独立命名) |
| Runtime/Driver 分层 | 有（与 NVIDIA 一致） | **模糊**（HIP 混合了两层） |
| 开源程度 | 部分开源 | **高**（ROCm 大部分开源） |
| hook.cpp 复用 | 直接使用 | **不可复用**，需要 `hip*` 版本 |
| shim.c 复用 | 直接使用 | **不可复用**，需要 `hip*` 版本 |
| `cuGetProcAddress` 等价物 | 有 | **无直接等价物**（HIP 不使用此机制） |

### 5.8 XPUShare 适配方案

由于 HIP 没有 NVIDIA 那样的 `cuGetProcAddress` 动态符号查找机制，适配策略需要调整：

```c
// shim_hip.c 需要拦截的 Runtime API
hipMalloc, hipFree, hipLaunchKernel,
hipMemGetInfo, hipGetDeviceProperties,
hipDeviceSynchronize, hipStreamSynchronize

// hook_hip.cpp — 由于 HIP 没有独立 driver 层，
// 建议只做 runtime 层拦截（enforcement_layer=runtime）
// 不需要 cuGetProcAddress 等价物
```

**关键差异**：
- HIP 的 runtime/driver 不分层，建议 `KUBESHARE_ENFORCEMENT_LAYER=runtime`
- 拦截库链接 `-lamdhip64` 而非 `-lcuda -lcudart`
- `hipLaunchKernel` 的函数签名与 `cudaLaunchKernel` 略有不同（多一个 `sharedMemBytes` 参数位置差异）
- ROCm 开源特性使得理解内部机制更容易，调试友好

---

## 六、华为昇腾 (Ascend) — CANN/AscendCL

### 6.1 平台概况

华为昇腾是 NPU（Neural Processing Unit），采用达芬奇架构，与 GPU 有本质区别。软件栈为 CANN (Compute Architecture for Neural Networks)，编程接口为 AscendCL。主要产品包括 Ascend 910B（训练）、Ascend 310P（推理）等。

### 6.2 AscendCL Runtime API 结构

AscendCL 与 CUDA 的编程模型有**根本性差异**：

```c
// 初始化/销毁
aclInit(configPath)              // 全局初始化（CUDA 无对应）
aclFinalize()                    // 全局销毁

// 设备管理
aclrtSetDevice(deviceId)         // ≈ cudaSetDevice
aclrtGetDevice(&deviceId)        // ≈ cudaGetDevice
aclrtResetDevice(deviceId)       // 无 CUDA 对应

// 内存管理
aclrtMalloc(&ptr, size, policy)  // ≈ cudaMalloc（多一个 policy 参数）
aclrtFree(ptr)                   // ≈ cudaFree
aclrtMemcpy(dst, dstMax, src, count, kind)  // ≈ cudaMemcpy（多 dstMax 参数）
aclrtGetMemInfo(memType, &free, &total)     // ≈ cudaMemGetInfo（多 memType 参数）

// 流管理
aclrtCreateStream(&stream)       // ≈ cudaStreamCreate
aclrtDestroyStream(stream)       // ≈ cudaStreamDestroy
aclrtSynchronizeStream(stream)   // ≈ cudaStreamSynchronize

// 算子执行 — 与 CUDA kernel launch 完全不同
aclrtLaunch(...)                 // 不是简单的 kernel launch
// 实际执行路径：
// 1. 加载模型/算子：aclmdlLoadFromFile / aclopCompileAndExecute
// 2. 创建输入输出 dataset
// 3. 执行：aclmdlExecute / aclopExecuteV2
// 4. 获取结果
```

### 6.3 与 CUDA 的根本差异

| 维度 | CUDA | AscendCL |
|------|------|----------|
| 计算单元 | GPU (SIMT) | NPU (达芬奇 Cube/Vector) |
| 编程模型 | Kernel Launch (<<<>>>)  | 算子描述 + 执行引擎 |
| 自定义算子 | CUDA C/C++ kernel | Ascend C (TBE) |
| 执行粒度 | 单个 kernel 函数 | 算子/模型级别 |
| Runtime/Driver 分层 | 有 | **无**（AscendCL 是统一接口） |
| 内存模型 | 统一虚拟地址 | 分离的 Host/Device 内存 + policy |
| 上层框架 | PyTorch (原生) | PyTorch (torch_npu 适配层) |

### 6.4 设备管理

- **SMI 工具**：`npu-smi info` — 显示 NPU 信息
- XPUShare 目前**没有**昇腾的 provider，需要新增

### 6.5 环境变量

- `ASCEND_RT_VISIBLE_DEVICES` — 控制可见 NPU 设备

### 6.6 NPU 虚拟化 (vNPU)

华为已提供原生虚拟化方案：
- **静态虚拟化**：预先划分 NPU 资源模板（类似 NVIDIA MIG），如 1/2、1/4、1/8 切分
- **动态虚拟化**：运行时动态分配资源
- **MindX DL 套件**：包含设备插件、调度器、容器运行时
- **HAMi 支持**：已支持 Ascend 910A/910B/910B2/910B3/910B4/310P，提供显存隔离 + 算力核心隔离

### 6.7 容器运行时与 Device Plugin

- **官方设备插件**：`ascend-device-plugin`（Gitee: `Ascend/ascend-device-plugin`）
- **容器运行时**：`ascend-docker-runtime`
- **调度器**：Volcano 调度器支持 vNPU 特性

### 6.8 与天数智芯的异同

| 维度 | 天数智芯 | 昇腾 |
|------|---------|------|
| 硬件类型 | GPU | **NPU** |
| API 兼容度 | CUDA 二进制兼容 | **完全不兼容** |
| LD_PRELOAD 拦截 | 已验证可行 | **理论可行但意义有限** |
| 拦截点 | `cudaMalloc`/`cuLaunchKernel` | `aclrtMalloc`/`aclmdlExecute` |
| 算力调度语义 | kernel launch 粒度 | **算子/模型执行粒度** |
| 时间片调度适用性 | 适用 | **不确定**（算子执行时间差异大） |

### 6.9 XPUShare 适配评估

**LD_PRELOAD 拦截技术上可行，但语义适配困难**：

1. **可以拦截的函数**：`aclrtMalloc`, `aclrtFree`, `aclrtGetMemInfo` — 显存隔离可实现
2. **难以适配的部分**：
   - 算力调度：CUDA 的 kernel launch 是细粒度的（微秒级），昇腾的算子执行是粗粒度的（毫秒级），Gemini 的 burst predictor 模型不适用
   - 执行路径多样：`aclmdlExecute`（模型推理）、`aclopExecuteV2`（单算子）、`aclrtLaunch`（kernel 级）等多个入口
   - 同步语义不同：没有 `cudaEventSynchronize` 等价物

3. **推荐策略**：
   - 显存隔离：通过 LD_PRELOAD 拦截 `aclrtMalloc`/`aclrtFree`/`aclrtGetMemInfo` 实现
   - 算力隔离：**放弃 Gemini 时间片调度**，改用华为原生 vNPU 机制
   - 或者：仅做显存隔离 + 调度级别的算力分配（不做 API 级别的时间片控制）

---

## 七、与天数智芯的异同对比矩阵

| 维度 | 天数智芯 (基线) | 壁仞 | 摩尔线程 | 海光 DCU | 昇腾 |
|------|----------------|------|---------|---------|------|
| **API 命名** | `cuda*`/`cu*` (兼容) | `cuda*`/`cu*` (兼容) | `musa*`/`mu*` | `hip*` | `acl*` |
| **hook.cpp 复用** | ✅ 直接使用 | ✅ 可能直接使用 | ❌ 需重写 | ❌ 需重写 | ❌ 需重写 |
| **shim.c 复用** | ✅ 直接使用 | ✅ 可能直接使用 | ❌ 需重写 | ❌ 需重写 | ❌ 需重写 |
| **cuGetProcAddress** | ✅ 有 | ✅ 可能有 | ❓ 未知 | ❌ 无 | ❌ 无 |
| **Runtime/Driver 分层** | ✅ 有 | ✅ 有 | ✅ 有 | ⚠️ 模糊 | ❌ 无 |
| **LD_PRELOAD 可行性** | ✅ 已验证 | ✅ 理论可行 | ✅ 可行 | ✅ 可行 | ⚠️ 有限 |
| **Gemini 时间片适用** | ✅ 适用 | ✅ 适用 | ✅ 适用 | ✅ 适用 | ❌ 不适用 |
| **Provider 现状** | ✅ 已实现 | ✅ 已有骨架 | ✅ 已有骨架 | ❌ 需新增 | ❌ 需新增 |
| **config.go 支持** | ✅ 已配置 | ✅ 已配置 | ✅ 已配置 | ❌ 需新增 | ❌ 需新增 |
| **HAMi 参考** | ✅ 有 | ❌ 无 | ✅ 有 | ✅ 有 | ✅ 有 |
| **编译工具链** | CoreX clang++ | 未公开 | mcc | hipcc | Ascend C 编译器 |
| **Makefile 改动** | 无 | 小 | 中 | 中 | 大 |

---

## 八、XPUShare 扩展可行性评估

### 8.1 总体评估

| 厂商 | 可行性 | 适配难度 | 预估工作量 | 建议优先级 |
|------|--------|---------|-----------|-----------|
| 壁仞 | ✅ 高（如 CUDA 兼容属实） | 低 | 1-2 周（验证为主） | ★★★★☆ |
| 摩尔线程 | ✅ 高 | 中 | 3-4 周 | ★★★★★ |
| 海光 DCU | ✅ 高 | 中 | 3-4 周 | ★★★★★ |
| 昇腾 | ⚠️ 有限 | 高 | 6-8 周（仅显存隔离） | ★★☆☆☆ |

### 8.2 壁仞适配可行性

**结论：可行性高，但依赖 SDK 获取**

- 如果 SUPA 确实提供 CUDA 二进制兼容，现有的 `hook.cpp` + `shim.c` 可能**无需修改**即可工作
- Go 侧 Provider 已有骨架 (`providers_biren_brsmi.go`)
- `config.go` 已配置 `CUDA_VISIBLE_DEVICES` 和 index 模式
- **阻塞项**：需要获取 SUPA SDK 和实机环境进行验证

**需要验证的关键点**：
1. `dlsym(RTLD_NEXT, "cuMemAlloc")` 是否能在壁仞的 `libcuda.so` 中找到符号
2. `cuGetProcAddress` 是否存在且行为一致
3. `cudaEventSynchronize` 是否有天数智芯 CoreX 上的阻塞问题

### 8.3 摩尔线程适配可行性

**结论：完全可行，工作量可控**

需要的改动：

**Gemini 层 (C/C++)**：
1. 新建 `shim_musa.c` — 从 `shim.c` 复制，将所有 `cuda*` 替换为 `musa*`
2. 新建 `hook_musa.cpp` — 从 `hook.cpp` 复制，将所有 `cu*` 替换为 `mu*`
3. 新建 `hook_musa.h` — 定义 `MU_HOOK_*` 枚举
4. `Makefile` 添加 MUSA 编译目标，链接 `-lmusa_runtime`（或对应库名）

**Go 层**：
- Provider 已有 (`providers_mthreads_gmi.go`)
- `config.go` 已配置 `MUSA_VISIBLE_DEVICES`
- 无需额外改动

**可优化方案**：将 `shim.c` 和 `hook.cpp` 重构为模板化设计，通过编译时宏选择 API 前缀：
```c
// shim_generic.c.in
#if defined(XPU_BACKEND_CUDA)
  #define XPU_MALLOC cudaMalloc
  #define XPU_FREE   cudaFree
  #include <cuda_runtime.h>
#elif defined(XPU_BACKEND_MUSA)
  #define XPU_MALLOC musaMalloc
  #define XPU_FREE   musaFree
  #include <musa_runtime.h>
#elif defined(XPU_BACKEND_HIP)
  #define XPU_MALLOC hipMalloc
  #define XPU_FREE   hipFree
  #include <hip/hip_runtime.h>
#endif
```

### 8.4 海光 DCU 适配可行性

**结论：完全可行，ROCm 开源生态有利**

需要的改动：

**Gemini 层 (C/C++)**：
1. 新建 `shim_hip.c` — 拦截 `hip*` Runtime API
2. 新建 `hook_hip.cpp` — 由于 HIP 没有独立 driver 层，建议**只做 runtime 层拦截**
3. `Makefile` 添加 HIP 编译目标，使用 `hipcc` 编译，链接 `-lamdhip64`

**关键适配点**：
- `KUBESHARE_ENFORCEMENT_LAYER` 默认设为 `runtime`（HIP 无清晰 driver 层）
- 不需要 `cuGetProcAddress` 等价物（HIP 不使用此机制）
- `hipLaunchKernel` 签名与 `cudaLaunchKernel` 略有差异，需要调整参数映射
- ROCm 的 `hipEventSynchronize` 行为需要验证（是否有 CoreX 上的阻塞问题）

**Go 层**：
- 需要新增 `providers_hygon_rocmsmi.go`（解析 `rocm-smi` 或 `hy-smi` 输出）
- `config.go` 需要添加 `hygon` / `hygon-dcu` provider 的默认配置：
  - `VisibleDevicesEnvVars`: `["HIP_VISIBLE_DEVICES"]`
  - `VisibleDevicesValueMode`: `"index"`
  - `EnforcementLayerMode`: `"runtime"`

### 8.5 昇腾适配可行性

**结论：LD_PRELOAD 技术可行但语义适配困难，建议混合策略**

**可行部分（显存隔离）**：
- 拦截 `aclrtMalloc` / `aclrtFree` / `aclrtGetMemInfo` 实现显存配额控制
- 与 CUDA 的显存管理语义基本一致，改动量不大

**困难部分（算力调度）**：
- Gemini 的 burst predictor 基于 CUDA kernel launch 的微秒级粒度设计
- 昇腾的算子执行是毫秒级粒度，且执行路径多样（`aclmdlExecute`, `aclopExecuteV2`, `aclrtLaunch`）
- 时间片调度模型不适用于 NPU 的算子执行模式

**推荐混合策略**：
1. **显存隔离**：通过 LD_PRELOAD 拦截 `aclrtMalloc`/`aclrtFree` 实现（XPUShare 方式）
2. **算力隔离**：利用华为原生 vNPU 机制（静态/动态虚拟化）
3. **调度集成**：XPUShare scheduler 感知 vNPU 资源，但不做 API 级别的时间片控制

**Go 层**：
- 需要新增 `providers_ascend_npusmi.go`（解析 `npu-smi info` 输出）
- `config.go` 需要添加 `ascend` provider 的默认配置：
  - `VisibleDevicesEnvVars`: `["ASCEND_RT_VISIBLE_DEVICES"]`
  - `VisibleDevicesValueMode`: `"index"`
  - `EnforcementLayerMode`: `"none"` 或 `"runtime"`（仅显存隔离时）

---

## 九、推荐实施路径

### Phase 1：架构重构（1-2 周）

将 Gemini 拦截层从硬编码 CUDA 改为可插拔的多后端架构：

```
Gemini/src/
├── backends/
│   ├── cuda/          # 现有 hook.cpp + shim.c（天数智芯/壁仞/NVIDIA）
│   ├── musa/          # 摩尔线程 hook_musa.cpp + shim_musa.c
│   ├── hip/           # 海光 DCU hook_hip.cpp + shim_hip.c
│   └── ascendcl/      # 昇腾 shim_ascend.c（仅显存隔离）
├── core/
│   ├── scheduler.cpp  # gem-schd（不变）
│   ├── pod-manager.cpp # gem-pmgr（不变）
│   ├── predictor.cpp  # burst predictor（不变）
│   └── comm.cpp       # 通信协议（不变）
└── Makefile           # 多目标编译：make BACKEND=cuda/musa/hip/ascendcl
```

核心思路：`scheduler.cpp`, `pod-manager.cpp`, `predictor.cpp`, `comm.cpp` 等调度逻辑**完全不变**，只有拦截层按后端切换。

### Phase 2：摩尔线程 + 海光 DCU 适配（3-4 周）

这两个平台 API 结构与 CUDA 同构，可以并行开发：

1. 从 `shim.c` / `hook.cpp` 生成 `shim_musa.c` / `hook_musa.cpp` 和 `shim_hip.c` / `hook_hip.cpp`
2. 调整编译工具链和链接库
3. 在实机环境验证 LD_PRELOAD 拦截行为
4. 验证 burst predictor 在不同硬件上的预测准确性

### Phase 3：壁仞验证（1-2 周）

1. 获取 SUPA SDK 和实机环境
2. 直接使用现有 CUDA hook 库测试
3. 验证 `dlsym` / `cuGetProcAddress` 兼容性
4. 如有差异，创建壁仞特定的 hook 变体

### Phase 4：昇腾适配（4-6 周）

1. 实现 `shim_ascend.c`（仅显存隔离）
2. 集成华为 vNPU 机制用于算力隔离
3. 调整 scheduler 支持 vNPU 资源感知
4. 评估是否需要为 NPU 设计新的调度模型（替代 burst predictor）

### Go 侧改动（贯穿所有 Phase）

```
pkg/gpu/
├── providers_hygon_rocmsmi.go    # 新增：解析 rocm-smi / hy-smi
├── providers_ascend_npusmi.go    # 新增：解析 npu-smi info
├── config.go                     # 修改：添加 hygon/ascend 默认配置
└── provider.go                   # 修改：添加 hygon/ascend case
```

---

## 十、结论

XPUShare 的架构设计（Provider 接口 + 环境变量驱动的配置 + LD_PRELOAD 拦截）天然适合扩展到多种国产 GPU 平台。

**核心发现**：

1. **壁仞、摩尔线程、海光 DCU 三家均可通过 LD_PRELOAD 实现完整的显存隔离 + 算力调度**，因为它们的 Runtime API 结构与 CUDA 同构（仅命名不同）。Gemini 的 burst predictor + 时间片调度模型可以直接复用。

2. **昇腾是唯一需要不同策略的平台**。NPU 的算子执行模型与 GPU 的 kernel launch 模型有本质差异，Gemini 的时间片调度不适用。建议采用"LD_PRELOAD 显存隔离 + 华为原生 vNPU 算力隔离"的混合方案。

3. **Go 侧的 Provider 抽象层设计良好**，壁仞和摩尔线程已有 provider 骨架，海光和昇腾只需新增 SMI 工具解析器。`config.go` 的环境变量驱动设计使得新厂商的集成非常轻量。

4. **最大的工程挑战在 Gemini C/C++ 层**。建议将拦截层重构为多后端架构（编译时选择），核心调度逻辑保持不变。这样每个新后端只需要编写一套 API 名称映射，工作量可控。

5. **HAMi 项目是重要的参考和验证**。HAMi 已经在摩尔线程、海光 DCU、昇腾上验证了 GPU 虚拟化的可行性，其实现经验可以直接借鉴。

**优先级建议**：摩尔线程 ≈ 海光 DCU > 壁仞 > 昇腾。前两者市场需求大、API 适配清晰、有 HAMi 参考；壁仞受限于 SDK 不公开；昇腾需要不同的技术路线。
