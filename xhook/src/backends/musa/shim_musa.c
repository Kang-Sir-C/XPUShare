/**
 * ============================================================================
 * MUSA Runtime API Interceptor — Moore Threads (摩尔线程) GPU Support
 * ============================================================================
 *
 * This file mirrors shim.c (CUDA backend) but intercepts MUSA Runtime APIs:
 *   musaMalloc, musaFree, musaLaunchKernel, musaLaunch,
 *   musaMemGetInfo, musaGetDeviceProperties,
 *   musaDeviceSynchronize, musaStreamSynchronize
 *
 * The MUSA Runtime API is largely CUDA-compatible. The interception strategy
 * is identical: LD_PRELOAD this .so before the real libmusart.so.
 *
 * TODO: This skeleton requires the MUSA Toolkit (musa_runtime.h) to compile.
 *       Fill in real implementations when the SDK is available.
 * ============================================================================
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

// TODO: Uncomment when MUSA SDK is available
// #include <musa_runtime.h>  /* from MUSA Toolkit */

// ============================================================================
// Placeholder types — remove when real MUSA headers are included
// ============================================================================
typedef int musaError_t;
typedef void* musaStream_t;
struct musaDeviceProp { size_t totalGlobalMem; /* ... */ };
typedef struct { unsigned int x, y, z; } dim3_musa;
#define musaSuccess 0
#define musaErrorMemoryAllocation 2

// ============================================================================
// Extern C interface to hook_musa.cpp
// ============================================================================
extern size_t get_musa_total_memory_limit_for_shim(void);
extern void   get_musa_mem_info_for_shim(size_t *free_bytes, size_t *total_bytes);
extern size_t get_musa_free_mem_for_shim(void);
extern int    update_musa_mem_usage_for_shim(size_t bytes, int is_allocate);
extern int    request_musa_launch_token_for_shim(void);
extern void   musa_host_sync_call_for_shim(const char *func_name);

// ============================================================================
// TODO: Implement all interceptors below when MUSA SDK is available.
//       The logic is identical to shim.c (CUDA backend) — only the function
//       names and types change (cuda* → musa*).
// ============================================================================

/*
musaError_t musaGetDeviceProperties(struct musaDeviceProp *prop, int device) {
    // TODO: dlsym(RTLD_NEXT, "musaGetDeviceProperties"), virtualize totalGlobalMem
    return musaSuccess;
}

musaError_t musaMemGetInfo(size_t *free, size_t *total) {
    // TODO: Return virtualized memory info from hook_musa.cpp
    return musaSuccess;
}

musaError_t musaMalloc(void **devPtr, size_t size) {
    // TODO: Check quota, call real musaMalloc, update accounting
    return musaSuccess;
}

musaError_t musaFree(void *devPtr) {
    // TODO: Look up size, update accounting, call real musaFree
    return musaSuccess;
}

musaError_t musaLaunchKernel(const void *func,
                              dim3_musa gridDim,
                              dim3_musa blockDim,
                              void **args,
                              size_t sharedMem,
                              musaStream_t stream) {
    // TODO: request_musa_launch_token_for_shim(), then call real musaLaunchKernel
    return musaSuccess;
}

musaError_t musaLaunch(const void *func) {
    // TODO: Legacy launch path
    return musaSuccess;
}

musaError_t musaDeviceSynchronize(void) {
    // TODO: Call real musaDeviceSynchronize, then musa_host_sync_call_for_shim
    return musaSuccess;
}

musaError_t musaStreamSynchronize(musaStream_t stream) {
    // TODO: Call real musaStreamSynchronize, then musa_host_sync_call_for_shim
    return musaSuccess;
}
*/
