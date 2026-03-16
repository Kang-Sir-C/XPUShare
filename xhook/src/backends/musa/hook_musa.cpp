/**
 * ============================================================================
 * MUSA Backend Skeleton — Moore Threads (摩尔线程) GPU Support
 * ============================================================================
 *
 * This file is a skeleton for the MUSA (Moore Threads Unified Software
 * Architecture) driver-level API interception layer. It mirrors the structure
 * of the CUDA backend (backends/cuda/hook.cpp) but targets MUSA Driver APIs.
 *
 * Key interception targets:
 *   muMemAlloc, muMemFree, muLaunchKernel,
 *   muMemGetInfo, muDeviceTotalMem, muCtxSynchronize
 *
 * The MUSA SDK provides a CUDA-compatible programming model. Most of the
 * scheduling and memory isolation logic in core/ can be reused directly.
 *
 * TODO: This skeleton requires the MUSA Toolkit (musa_runtime.h, libmusa.so)
 *       to compile. Fill in real implementations when the SDK is available.
 * ============================================================================
 */

#include "hook_musa.h"

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

// TODO: Uncomment when MUSA SDK is available
// #include <musa.h>
// #include <musa_runtime.h>

// ============================================================================
// Placeholder types — remove when real MUSA headers are included
// ============================================================================
typedef int MUresult;
typedef void* MUdeviceptr;
typedef void* MUfunction;
typedef void* MUstream;
typedef unsigned long long muuint64_t;
#define MU_SUCCESS 0
#define MU_ERROR_OUT_OF_MEMORY 2

// ============================================================================
// Connection & scheduling state (mirrors CUDA backend)
// ============================================================================
static const char* log_name = "/xpushare/log/hook.log";

extern double quota_time;
extern double overuse;
extern Predictor burst_predictor;
extern Predictor window_predictor;

// GPU memory allocation tracking
static pthread_mutex_t allocation_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::map<MUdeviceptr, size_t> allocation_map;
static size_t gpu_mem_used = 0;

// ============================================================================
// Core scheduling logic — delegates to the same core/ routines as CUDA
// ============================================================================

/**
 * Called before every kernel launch to request a scheduling token.
 * TODO: Implement full scheduling logic (same as CUDA on_kernel_launch_request)
 */
static void on_kernel_launch_request() {
    // TODO: Implement — copy logic from CUDA backend's on_kernel_launch_request()
    // The core scheduling protocol (REQ_QUOTA via comm.h) is identical.
}

// ============================================================================
// Memory management stubs
// ============================================================================

static MUresult muMemAlloc_prehook(MUdeviceptr *dptr, size_t bytesize) {
    // TODO: Check memory quota via get_gpu_memory_info()
    (void)dptr; (void)bytesize;
    return MU_SUCCESS;
}

static MUresult muMemAlloc_posthook(MUdeviceptr *dptr, size_t bytesize) {
    // TODO: update_memory_usage(bytesize, 1) and record in allocation_map
    (void)dptr; (void)bytesize;
    return MU_SUCCESS;
}

static MUresult muMemFree_prehook(MUdeviceptr ptr) {
    // TODO: Look up allocation_map, call update_memory_usage(sz, 0)
    (void)ptr;
    return MU_SUCCESS;
}

// ============================================================================
// Kernel launch hook
// ============================================================================

static MUresult muLaunchKernel_prehook(MUfunction f,
                                        unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
                                        unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
                                        unsigned int sharedMemBytes, MUstream hStream,
                                        void **kernelParams, void **extra) {
    (void)f;
    (void)gridDimX; (void)gridDimY; (void)gridDimZ;
    (void)blockDimX; (void)blockDimY; (void)blockDimZ;
    (void)sharedMemBytes; (void)hStream;
    (void)kernelParams; (void)extra;

    // TODO: Call on_kernel_launch_request() when enforcement is enabled
    return MU_SUCCESS;
}

// ============================================================================
// Extern "C" interface for shim_musa.c
// ============================================================================
extern "C" {

size_t get_musa_free_mem_for_shim(void) {
    // TODO: return remaining memory from get_gpu_memory_info()
    return 0;
}

int update_musa_mem_usage_for_shim(size_t bytes, int is_allocate) {
    // TODO: return update_memory_usage(bytes, is_allocate)
    (void)bytes; (void)is_allocate;
    return 1;
}

int request_musa_launch_token_for_shim(void) {
    // TODO: Call on_kernel_launch_request()
    return 1;
}

size_t get_musa_total_memory_limit_for_shim(void) {
    // TODO: return total from get_gpu_memory_info()
    return 0;
}

void get_musa_mem_info_for_shim(size_t *free_bytes, size_t *total_bytes) {
    // TODO: Fill from get_gpu_memory_info()
    if (free_bytes)  *free_bytes  = 0;
    if (total_bytes) *total_bytes = 0;
}

void musa_host_sync_call_for_shim(const char *func_name) {
    // TODO: Call host_sync_call(func_name)
    (void)func_name;
}

} // extern "C"
