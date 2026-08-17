/**
 * ============================================================================
 * HIP Backend Skeleton — Hygon DCU (海光 DCU) Support
 * ============================================================================
 *
 * This file is a skeleton for the HIP driver-level API interception layer.
 * It targets Hygon DCU devices which use the HIP programming model.
 *
 * Key differences from CUDA backend:
 *   - HIP does NOT have a cuGetProcAddress equivalent. Symbol interception
 *     must be done entirely via LD_PRELOAD / dlsym(RTLD_NEXT, ...).
 *   - HIP runtime and driver layers are NOT cleanly separated. There is no
 *     distinct "driver API" vs "runtime API" — hipMalloc IS the primary API.
 *   - Therefore, enforcement should default to "runtime" layer since there's
 *     no clean driver/runtime split.
 *
 * Interception targets:
 *   hipMalloc, hipFree, hipModuleLaunchKernel,
 *   hipMemGetInfo, hipDeviceTotalMem
 *
 * TODO: This skeleton requires the ROCm/HIP SDK (hip/hip_runtime.h) to compile.
 * ============================================================================
 */

#include "hook_hip.h"

// Core headers — backend-agnostic scheduling and communication logic
#include "../../core/comm.h"
#include "../../core/debug.h"
#include "../../core/predictor.h"
#include "../../core/util.h"

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <map>

// TODO: Uncomment when HIP SDK is available
// #include <hip/hip_runtime.h>

// ============================================================================
// Placeholder types — remove when real HIP headers are included
// ============================================================================
typedef int hipError_t;
typedef void* hipDeviceptr_t;
typedef void* hipFunction_t;
typedef void* hipStream_t;
#define hipSuccess 0
#define hipErrorOutOfMemory 2

// ============================================================================
// Connection & scheduling state (mirrors CUDA backend)
// ============================================================================
static const char* log_name = "/xpushare/log/hook.log";

// GPU memory allocation tracking
static pthread_mutex_t allocation_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::map<hipDeviceptr_t, size_t> allocation_map;
static size_t gpu_mem_used = 0;

// ============================================================================
// Core scheduling logic
// ============================================================================

/**
 * Called before every kernel launch to request a scheduling token.
 * TODO: Implement full scheduling logic (same as CUDA on_kernel_launch_request)
 */
static void on_kernel_launch_request() {
    // TODO: Implement — the core scheduling protocol (REQ_QUOTA) is identical.
    // Note: For HIP, this is always called from the "runtime" layer since
    // there is no separate driver layer.
}

// ============================================================================
// Memory management stubs
// ============================================================================

static hipError_t hipMemAlloc_prehook(hipDeviceptr_t *dptr, size_t bytesize) {
    // TODO: Check memory quota via get_gpu_memory_info()
    (void)dptr; (void)bytesize;
    return hipSuccess;
}

static hipError_t hipMemAlloc_posthook(hipDeviceptr_t *dptr, size_t bytesize) {
    // TODO: update_memory_usage(bytesize, 1) and record in allocation_map
    (void)dptr; (void)bytesize;
    return hipSuccess;
}

static hipError_t hipMemFree_prehook(hipDeviceptr_t ptr) {
    // TODO: Look up allocation_map, call update_memory_usage(sz, 0)
    (void)ptr;
    return hipSuccess;
}

// ============================================================================
// Kernel launch hook
// ============================================================================

/**
 * HIP uses hipModuleLaunchKernel as the primary low-level launch API.
 * hipLaunchKernel (the runtime version) is a wrapper around it.
 */
static hipError_t hipModuleLaunchKernel_prehook(hipFunction_t f,
                                                 unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
                                                 unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
                                                 unsigned int sharedMemBytes, hipStream_t hStream,
                                                 void **kernelParams, void **extra) {
    (void)f;
    (void)gridDimX; (void)gridDimY; (void)gridDimZ;
    (void)blockDimX; (void)blockDimY; (void)blockDimZ;
    (void)sharedMemBytes; (void)hStream;
    (void)kernelParams; (void)extra;

    // TODO: Call on_kernel_launch_request() when enforcement is enabled
    return hipSuccess;
}

// ============================================================================
// Extern "C" interface for shim_hip.c
// ============================================================================
extern "C" {

size_t get_hip_free_mem_for_shim(void) {
    // TODO: return remaining memory from get_gpu_memory_info()
    return 0;
}

int update_hip_mem_usage_for_shim(size_t bytes, int is_allocate) {
    // TODO: return update_memory_usage(bytes, is_allocate)
    (void)bytes; (void)is_allocate;
    return 1;
}

int request_hip_launch_token_for_shim(void) {
    // TODO: Call on_kernel_launch_request()
    return 1;
}

size_t get_hip_total_memory_limit_for_shim(void) {
    // TODO: return total from get_gpu_memory_info()
    return 0;
}

void get_hip_mem_info_for_shim(size_t *free_bytes, size_t *total_bytes) {
    // TODO: Fill from get_gpu_memory_info()
    if (free_bytes)  *free_bytes  = 0;
    if (total_bytes) *total_bytes = 0;
}

void hip_host_sync_call_for_shim(const char *func_name) {
    // TODO: Call host_sync_call(func_name)
    (void)func_name;
}

} // extern "C"
