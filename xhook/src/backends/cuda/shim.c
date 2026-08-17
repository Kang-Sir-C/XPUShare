// shim.c - Iluvatar CoreX Runtime API Interceptor
// 方案：
//  - 显存总量/剩余：通过 hook.cpp 暴露的接口获取（后端是 xhook pod-manager）
//  - 显存分配/释放：在 Runtime API 层拦截 cudaMalloc/cudaFree，并调用 hook.cpp 的 update_mem_usage_for_shim
//  - 算力调度：在 Runtime API 层拦截 cudaLaunchKernel/cudaLaunch，并在发射 kernel 前调用
//              hook.cpp 暴露的 request_launch_token_for_shim（内部负责 REQ_QUOTA 协议）。
//  - 注意：需要在 Pod 里 LD_PRELOAD 确保本 .so 在真正的 libcudart 之前被加载

#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <cuda_runtime.h>

// Shared TLS flag with hook.cpp to prevent double enforcement when runtime API
// calls transitively enter driver APIs (cuda* -> cu*).
__thread int g_xpushare_in_runtime_shim = 0;

static int shim_enter_runtime_scope(void) {
    int prev = g_xpushare_in_runtime_shim;
    g_xpushare_in_runtime_shim = 1;
    return prev;
}

static void shim_leave_runtime_scope(int prev) {
    g_xpushare_in_runtime_shim = prev;
}

typedef enum ShimSymbolsEnum {
    SHIM_CUDA_GET_DEVICE_PROPERTIES,
    SHIM_CUDA_MEM_GET_INFO,
    SHIM_CUDA_MALLOC,
    SHIM_CUDA_FREE,
    SHIM_CUDA_LAUNCH_KERNEL,
    SHIM_CUDA_LAUNCH,
    SHIM_CUDA_DEVICE_SYNCHRONIZE,
    SHIM_CUDA_STREAM_SYNCHRONIZE,
    NUM_SHIM_SYMBOLS,
} ShimSymbols;

static const char *kShimSymbolNames[NUM_SHIM_SYMBOLS] = {
    "cudaGetDeviceProperties",
    "cudaMemGetInfo",
    "cudaMalloc",
    "cudaFree",
    "cudaLaunchKernel",
    "cudaLaunch",
    "cudaDeviceSynchronize",
    "cudaStreamSynchronize",
};

static long long g_shim_call_count[NUM_SHIM_SYMBOLS];
static int g_shim_coverage = -1;
static int g_enforcement_layer = -1;

static int shim_coverage_enabled(void) {
    if (g_shim_coverage < 0) {
        const char *e = getenv("XPUSHARE_SHIM_COVERAGE");
        const char *p = getenv("XPUSHARE_PROFILING");
        g_shim_coverage = ((e && e[0] == '1') || (p && p[0] == '1')) ? 1 : 0;
    }
    return g_shim_coverage == 1;
}

static int shim_enforcement_layer(void) {
    if (g_enforcement_layer >= 0) return g_enforcement_layer;

    const char *p = getenv("XPUSHARE_PROFILING");
    if (p && p[0] == '1') {
        g_enforcement_layer = 3; // profile
        return g_enforcement_layer;
    }

    const char *e = getenv("XPUSHARE_ENFORCEMENT_LAYER");
    if (!e || !e[0]) {
        g_enforcement_layer = 0; // driver
        return g_enforcement_layer;
    }

    if (!strcasecmp(e, "driver")) {
        g_enforcement_layer = 0;
    } else if (!strcasecmp(e, "runtime")) {
        g_enforcement_layer = 1;
    } else if (!strcasecmp(e, "none") || !strcasecmp(e, "off") || !strcasecmp(e, "disable") ||
               !strcasecmp(e, "disabled")) {
        g_enforcement_layer = 2;
    } else if (!strcasecmp(e, "profile") || !strcasecmp(e, "profiling")) {
        g_enforcement_layer = 3;
    } else {
        g_enforcement_layer = 0;
    }
    return g_enforcement_layer;
}

static int shim_runtime_virtualize_enabled(void) {
    return shim_enforcement_layer() == 1;
}

static int shim_runtime_enforce_enabled(void) {
    return shim_enforcement_layer() == 1;
}

static void shim_cov_inc(ShimSymbols s) {
    if (!shim_coverage_enabled()) return;
    if ((int)s < 0 || (int)s >= (int)NUM_SHIM_SYMBOLS) return;
    __sync_fetch_and_add(&g_shim_call_count[(int)s], 1);
}

static void shim_dump_coverage(void) {
    if (!shim_coverage_enabled()) return;

    FILE *f = fopen("/xpushare/log/hook.log", "a");
    if (!f) f = stderr;

    fprintf(f, "[COVERAGE][runtime] begin pid=%d\n", getpid());
    for (int i = 0; i < (int)NUM_SHIM_SYMBOLS; i++) {
        const char *name = kShimSymbolNames[i] ? kShimSymbolNames[i] : "unknown";
        fprintf(f, "[COVERAGE][runtime] %s=%lld\n", name, g_shim_call_count[i]);
    }
    fprintf(f, "[COVERAGE][runtime] end pid=%d\n", getpid());
    fflush(f);

    if (f != stderr) fclose(f);
}

__attribute__((destructor)) static void shim_dump_coverage_destructor(void) {
    shim_dump_coverage();
}

// -----------------------------
// 来自 hook.cpp 的 C 接口
// -----------------------------
extern size_t get_total_memory_limit_for_shim(void);
extern void   get_mem_info_for_shim(size_t *free_bytes, size_t *total_bytes);
extern size_t get_cuda_free_mem_for_shim(void);
extern int    update_mem_usage_for_shim(size_t bytes, int is_allocate);

// 这个接口在 hook.cpp 里需要实现成“请求/更新算力 token”的逻辑
//（当前你的版本里是一个返回 1 的 stub，需要按 cuLaunchKernel_prehook 的逻辑补全）
extern int    request_launch_token_for_shim(void);

// 通知 hook.cpp 发生了一次 host-GPU 同步，用于更新 burst predictor
extern void   host_sync_call_for_shim(const char *func_name);

// -----------------------------
// 实际 libcudart 函数指针类型
// -----------------------------
typedef cudaError_t (*real_cudaGetDeviceProperties_t)(struct cudaDeviceProp*, int);
typedef cudaError_t (*real_cudaMemGetInfo_t)(size_t*, size_t*);
typedef cudaError_t (*real_cudaMalloc_t)(void **, size_t);
typedef cudaError_t (*real_cudaFree_t)(void *);
typedef cudaError_t (*real_cudaLaunchKernel_t)(const void *func,
                                               dim3 gridDim,
                                               dim3 blockDim,
                                               void **args,
                                               size_t sharedMem,
                                               cudaStream_t stream);
typedef cudaError_t (*real_cudaLaunch_t)(const void *func);
typedef cudaError_t (*real_cudaDeviceSynchronize_t)(void);
typedef cudaError_t (*real_cudaStreamSynchronize_t)(cudaStream_t stream);

// -----------------------------
// 保存真实函数指针
// -----------------------------
static real_cudaGetDeviceProperties_t real_cudaGetDeviceProperties = NULL;
static real_cudaMemGetInfo_t          real_cudaMemGetInfo          = NULL;
static real_cudaMalloc_t              real_cudaMalloc              = NULL;
static real_cudaFree_t                real_cudaFree                = NULL;
static real_cudaLaunchKernel_t        real_cudaLaunchKernel        = NULL;
static real_cudaLaunch_t              real_cudaLaunch              = NULL;
static real_cudaDeviceSynchronize_t   real_cudaDeviceSynchronize   = NULL;
static real_cudaStreamSynchronize_t   real_cudaStreamSynchronize   = NULL;


static int g_shim_debug = -1;
static int shim_debug_enabled(void) {
    if (g_shim_debug < 0) {
        const char *e = getenv("XPUSHARE_SHIM_DEBUG");
        g_shim_debug = (e && e[0] == '1') ? 1 : 0;
    }
    return g_shim_debug == 1;
}

// -----------------------------
// 简单的 devPtr -> size 映射
// 用单链表实现，足够本场景使用
// -----------------------------
typedef struct AllocNode {
    void *ptr;
    size_t size;
    struct AllocNode *next;
} AllocNode;

static AllocNode *g_alloc_head = NULL;
static pthread_mutex_t g_alloc_lock = PTHREAD_MUTEX_INITIALIZER;

static void record_alloc(void *ptr, size_t size) {
    if (!ptr || size == 0) return;

    AllocNode *node = (AllocNode*)malloc(sizeof(AllocNode));
    if (!node) return;  // 内存不足时，宁可不记账也不要崩

    node->ptr  = ptr;
    node->size = size;
    pthread_mutex_lock(&g_alloc_lock);
    node->next = g_alloc_head;
    g_alloc_head = node;
    pthread_mutex_unlock(&g_alloc_lock);
}

static size_t erase_alloc(void *ptr) {
    if (!ptr) return 0;

    pthread_mutex_lock(&g_alloc_lock);
    AllocNode *prev = NULL;
    AllocNode *cur  = g_alloc_head;
    while (cur) {
        if (cur->ptr == ptr) {
            size_t sz = cur->size;
            if (prev) prev->next = cur->next;
            else      g_alloc_head = cur->next;
            free(cur);
            pthread_mutex_unlock(&g_alloc_lock);
            return sz;
        }
        prev = cur;
        cur  = cur->next;
    }
    pthread_mutex_unlock(&g_alloc_lock);
    return 0;  // 未找到
}

// ============================================================================
// 1. 显存视图隔离：拦截 cudaGetDeviceProperties / cudaMemGetInfo
// ============================================================================

// 拦截 cudaGetDeviceProperties，篡改 totalGlobalMem 为配额上限
cudaError_t cudaGetDeviceProperties(struct cudaDeviceProp *prop, int device) {
    shim_cov_inc(SHIM_CUDA_GET_DEVICE_PROPERTIES);
    if (!real_cudaGetDeviceProperties) {
        real_cudaGetDeviceProperties =
            (real_cudaGetDeviceProperties_t)dlsym(RTLD_NEXT, "cudaGetDeviceProperties");
    }
    int prev = shim_enter_runtime_scope();
    cudaError_t err = real_cudaGetDeviceProperties(prop, device);
    if (err == cudaSuccess && prop && shim_runtime_virtualize_enabled()) {
        size_t limit = get_total_memory_limit_for_shim();
        if (limit > 0) {
            prop->totalGlobalMem = limit;
        }
    }
    shim_leave_runtime_scope(prev);
    return err;
}

// 拦截 cudaMemGetInfo，返回虚拟 free/total（由 pod-manager 决定）
cudaError_t cudaMemGetInfo(size_t *free, size_t *total) {
    shim_cov_inc(SHIM_CUDA_MEM_GET_INFO);
    int prev = shim_enter_runtime_scope();
    cudaError_t err;

    // 不调用真实的 cudaMemGetInfo，直接从 hook.cpp/后端获取
    if (shim_runtime_virtualize_enabled()) {
        get_mem_info_for_shim(free, total);
        err = cudaSuccess;
    } else {
        if (!real_cudaMemGetInfo) {
            real_cudaMemGetInfo = (real_cudaMemGetInfo_t)dlsym(RTLD_NEXT, "cudaMemGetInfo");
        }
        err = real_cudaMemGetInfo(free, total);
    }
    shim_leave_runtime_scope(prev);
    return err;
}

// ============================================================================
// 2. 显存配额控制：拦截 cudaMalloc / cudaFree
// ============================================================================

// 拦截 cudaMalloc，申请前检查“虚拟剩余”，成功后通知后端增加已用显存
cudaError_t cudaMalloc(void **devPtr, size_t size) {
    shim_cov_inc(SHIM_CUDA_MALLOC);
    if (!real_cudaMalloc) {
        real_cudaMalloc = (real_cudaMalloc_t)dlsym(RTLD_NEXT, "cudaMalloc");
    }

    if (!shim_runtime_enforce_enabled()) {
        return real_cudaMalloc(devPtr, size);
    }

    // 通过 hook.cpp 查询剩余显存（单位：字节）
    size_t free_bytes = get_cuda_free_mem_for_shim();
    if (size > free_bytes) {
        // 超额：直接返回 cudaErrorMemoryAllocation
        return cudaErrorMemoryAllocation;
    }

    int prev = shim_enter_runtime_scope();

    // 交给真实的 cudaMalloc 做分配
    cudaError_t err = real_cudaMalloc(devPtr, size);
    if (err == cudaSuccess && devPtr && *devPtr) {
        // 1) 通知 pod-manager：已用显存 +size
        //    内部会调用 hook.cpp::update_memory_usage -> 发 REQ_MEM_UPDATE
        int ok = update_mem_usage_for_shim(size, 1);
        if (!ok) {
            // 后端认为超限，理论上不应该出现（前面已经做过检查）
            // 这里我们仍然保持分配成功，只打印日志的话需要在 hook.cpp 里做
            // 如需更严格，可以在这里调用 cudaFree(*devPtr) 再返回错误
        }

        // 2) 在本地保存 devPtr -> size，用于 cudaFree 时扣账
        record_alloc(*devPtr, size);
    }

    shim_leave_runtime_scope(prev);
    return err;
}

// 拦截 cudaFree，释放前从本地表拿到 size，并通知后端减少已用显存
cudaError_t cudaFree(void *devPtr) {
    shim_cov_inc(SHIM_CUDA_FREE);
    if (!real_cudaFree) {
        real_cudaFree = (real_cudaFree_t)dlsym(RTLD_NEXT, "cudaFree");
    }

    if (!shim_runtime_enforce_enabled()) {
        return real_cudaFree(devPtr);
    }

    // 从 map 中查出 size（如果没有记录，就不扣账）
    size_t sz = erase_alloc(devPtr);
    if (sz > 0) {
        update_mem_usage_for_shim(sz, 0);
    }

    int prev = shim_enter_runtime_scope();
    cudaError_t err = real_cudaFree(devPtr);
    shim_leave_runtime_scope(prev);
    return err;
}

// ============================================================================
// 3. 算力调度：拦截 cudaLaunchKernel / cudaLaunch
// ============================================================================
//
// 说明：
//  - compute_test.cu 使用 <<< >>> 启动 kernel，Host stub 在大多数 CUDA/COREX 实现中
//    最终会调用 cudaLaunchKernel 或（较老实现）cudaLaunch。
//  - 我们在这里统一拦截，在真正发射 kernel 前调用
//    request_launch_token_for_shim()，由 hook.cpp 负责：
//      * 调用 Predictor / get_token_from_scheduler
//      * 通过 pod-manager 向 xhook-scheduler 发送 REQ_QUOTA
//      * 更新 quota_time / overuse 等状态
//  - 这样，矩阵乘法测试在运行时就会在 xhook-pmgr.log / xhook-scheduler.log 中
//    产生 REQ_QUOTA / kernel 调度相关日志。

cudaError_t cudaLaunchKernel(const void *func,
                             dim3 gridDim,
                             dim3 blockDim,
                             void **args,
                             size_t sharedMem,
                             cudaStream_t stream) {
    shim_cov_inc(SHIM_CUDA_LAUNCH_KERNEL);
    if (!real_cudaLaunchKernel) {
        real_cudaLaunchKernel =
            (real_cudaLaunchKernel_t)dlsym(RTLD_NEXT, "cudaLaunchKernel");
    }

    if (shim_debug_enabled()) {
        fprintf(stderr,
                "[shim] cudaLaunchKernel intercepted: func=%p "
                "grid=(%u,%u,%u) block=(%u,%u,%u) sharedMem=%zu stream=%p\n",
                func,
                gridDim.x, gridDim.y, gridDim.z,
                blockDim.x, blockDim.y, blockDim.z,
                sharedMem, (void*)stream);
        fflush(stderr);
    }

    if (shim_runtime_enforce_enabled()) {
        request_launch_token_for_shim();
        int prev = shim_enter_runtime_scope();
        cudaError_t err =
            real_cudaLaunchKernel(func, gridDim, blockDim, args, sharedMem, stream);
        shim_leave_runtime_scope(prev);
        return err;
    }

    return real_cudaLaunchKernel(func, gridDim, blockDim, args, sharedMem, stream);
}

// 兼容老接口：有些实现可能仍然通过 cudaLaunch 调用 kernel
cudaError_t cudaLaunch(const void *func) {
    shim_cov_inc(SHIM_CUDA_LAUNCH);
    if (!real_cudaLaunch) {
        real_cudaLaunch = (real_cudaLaunch_t)dlsym(RTLD_NEXT, "cudaLaunch");
    }

    if (shim_debug_enabled()) {
        fprintf(stderr,
                "[shim] cudaLaunch intercepted: func=%p\n", func);
        fflush(stderr);
    }

    if (shim_runtime_enforce_enabled()) {
        request_launch_token_for_shim();
        int prev = shim_enter_runtime_scope();
        cudaError_t err = real_cudaLaunch(func);
        shim_leave_runtime_scope(prev);
        return err;
    }

    return real_cudaLaunch(func);
}

// ============================================================================
// 4. 同步拦截：cudaDeviceSynchronize / cudaStreamSynchronize
//    在 CoreX 上，torch.cuda.synchronize() 调用 cudaDeviceSynchronize，
//    但不会经过 driver 层的 cuCtxSynchronize。必须在 runtime 层拦截，
//    通知 burst_predictor 记录 burst 结束，否则 predictor 永远处于
//    ongoing 状态，导致客户端不再请求新 token。
// ============================================================================

cudaError_t cudaDeviceSynchronize(void) {
    shim_cov_inc(SHIM_CUDA_DEVICE_SYNCHRONIZE);
    if (!real_cudaDeviceSynchronize) {
        real_cudaDeviceSynchronize =
            (real_cudaDeviceSynchronize_t)dlsym(RTLD_NEXT, "cudaDeviceSynchronize");
    }

    int prev = shim_enter_runtime_scope();
    cudaError_t err = real_cudaDeviceSynchronize();
    shim_leave_runtime_scope(prev);

    // 同步完成后通知 burst predictor（无论 enforcement layer 是什么）
    host_sync_call_for_shim("cudaDeviceSynchronize");
    return err;
}

cudaError_t cudaStreamSynchronize(cudaStream_t stream) {
    shim_cov_inc(SHIM_CUDA_STREAM_SYNCHRONIZE);
    if (!real_cudaStreamSynchronize) {
        real_cudaStreamSynchronize =
            (real_cudaStreamSynchronize_t)dlsym(RTLD_NEXT, "cudaStreamSynchronize");
    }

    int prev = shim_enter_runtime_scope();
    cudaError_t err = real_cudaStreamSynchronize(stream);
    shim_leave_runtime_scope(prev);

    host_sync_call_for_shim("cudaStreamSynchronize");
    return err;
}
