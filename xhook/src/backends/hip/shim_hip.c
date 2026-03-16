/**
 * ============================================================================
 * HIP Runtime API Interceptor — Hygon DCU (海光 DCU) Support
 * ============================================================================
 *
 * This file mirrors shim.c (CUDA backend) but intercepts HIP Runtime APIs:
 *   hipMalloc, hipFree, hipLaunchKernel,
 *   hipMemGetInfo, hipGetDeviceProperties,
 *   hipDeviceSynchronize, hipStreamSynchronize
 *
 * Note: In HIP, the runtime API IS the primary API (no separate driver layer).
 * Therefore, all enforcement happens here in the shim, and the default
 * enforcement layer should be "runtime".
 *
 * TODO: This skeleton requires the ROCm/HIP SDK (hip/hip_runtime.h) to compile.
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

/* TODO: Uncomment when HIP SDK is available */
/* #include <hip/hip_runtime.h> */

/* ============================================================================
 * Placeholder types — remove when real HIP headers are included
 * ============================================================================ */
typedef int hipError_t;
typedef void* hipStream_t;
struct hipDeviceProp_t { size_t totalGlobalMem; /* ... */ };
typedef struct { unsigned int x, y, z; } dim3_hip;
#define hipSuccess 0
#define hipErrorOutOfMemory 2

/* ============================================================================
 * Extern C interface to hook_hip.cpp
 * ============================================================================ */
extern size_t get_hip_total_memory_limit_for_shim(void);
extern void   get_hip_mem_info_for_shim(size_t *free_bytes, size_t *total_bytes);
extern size_t get_hip_free_mem_for_shim(void);
extern int    update_hip_mem_usage_for_shim(size_t bytes, int is_allocate);
extern int    request_hip_launch_token_for_shim(void);
extern void   hip_host_sync_call_for_shim(const char *func_name);

/* ============================================================================
 * TODO: Implement all interceptors below when HIP SDK is available.
 *       The logic is identical to shim.c (CUDA backend) — only the function
 *       names and types change (cuda* → hip*).
 * ============================================================================ */

/*
hipError_t hipGetDeviceProperties(struct hipDeviceProp_t *prop, int device) {
    // TODO: dlsym(RTLD_NEXT, "hipGetDeviceProperties"), virtualize totalGlobalMem
    return hipSuccess;
}

hipError_t hipMemGetInfo(size_t *free, size_t *total) {
    // TODO: Return virtualized memory info from hook_hip.cpp
    return hipSuccess;
}

hipError_t hipMalloc(void **devPtr, size_t size) {
    // TODO: Check quota, call real hipMalloc, update accounting
    return hipSuccess;
}

hipError_t hipFree(void *devPtr) {
    // TODO: Look up size, update accounting, call real hipFree
    return hipSuccess;
}

hipError_t hipLaunchKernel(const void *func,
                            dim3_hip gridDim,
                            dim3_hip blockDim,
                            void **args,
                            size_t sharedMem,
                            hipStream_t stream) {
    // TODO: request_hip_launch_token_for_shim(), then call real hipLaunchKernel
    return hipSuccess;
}

hipError_t hipDeviceSynchronize(void) {
    // TODO: Call real hipDeviceSynchronize, then hip_host_sync_call_for_shim
    return hipSuccess;
}

hipError_t hipStreamSynchronize(hipStream_t stream) {
    // TODO: Call real hipStreamSynchronize, then hip_host_sync_call_for_shim
    return hipSuccess;
}
*/
