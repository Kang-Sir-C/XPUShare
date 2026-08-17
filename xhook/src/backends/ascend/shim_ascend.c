/**
 * ============================================================================
 * AscendCL Backend Skeleton — Huawei Ascend NPU (华为昇腾) Support
 * ============================================================================
 *
 * This file provides MEMORY ISOLATION ONLY for Huawei Ascend NPUs.
 *
 * Why only memory isolation (no compute scheduling)?
 * --------------------------------------------------
 * Ascend NPUs use a fundamentally different execution model from GPUs:
 *   - Operators are compiled into static graphs (via ATC/GE) and dispatched
 *     as opaque "model executions" (aclmdlExecute), NOT individual kernels.
 *   - There is no user-visible "kernel launch" API equivalent to
 *     cudaLaunchKernel / hipLaunchKernel.
 *   - The Ascend runtime (CANN) manages operator scheduling internally via
 *     its own task scheduler on the AI Core / AI CPU.
 *   - Intercepting aclmdlExecute would only give model-level granularity,
 *     which is too coarse for the burst-predictor-based scheduling used in
 *     XPUShare's core logic.
 *
 * Therefore, this backend only intercepts:
 *   - aclrtMalloc  — memory allocation with quota enforcement
 *   - aclrtFree    — memory deallocation with accounting
 *   - aclrtGetMemInfo — memory info virtualization
 *
 * The CANN SDK header is <acl/acl.h> (from Huawei CANN Toolkit).
 *
 * TODO: This skeleton requires the CANN SDK to compile.
 * ============================================================================
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* TODO: Uncomment when CANN SDK is available */
/* #include <acl/acl.h> */

/* ============================================================================
 * Placeholder types — remove when real CANN headers are included
 * ============================================================================ */
typedef int aclError;
typedef int aclrtMemMallocPolicy;
#define ACL_SUCCESS 0
#define ACL_ERROR_MEMORY_ALLOCATION_FAILURE 200000

/* ============================================================================
 * Memory accounting — simple linked list (same approach as CUDA shim.c)
 * ============================================================================ */
typedef struct AscendAllocNode {
    void *ptr;
    size_t size;
    struct AscendAllocNode *next;
} AscendAllocNode;

static AscendAllocNode *g_alloc_head = NULL;
static pthread_mutex_t g_alloc_lock = PTHREAD_MUTEX_INITIALIZER;

static void record_alloc(void *ptr, size_t size) {
    if (!ptr || size == 0) return;
    AscendAllocNode *node = (AscendAllocNode*)malloc(sizeof(AscendAllocNode));
    if (!node) return;
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
    AscendAllocNode *prev = NULL;
    AscendAllocNode *cur  = g_alloc_head;
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
    return 0;
}

/* ============================================================================
 * TODO: Extern interface to a future hook_ascend.cpp (or direct comm.h usage)
 *
 * For now, these are stubs. When implementing, either:
 *   (a) Create a hook_ascend.cpp that links against core/ and provides these, or
 *   (b) Link this .c file directly with core/ objects (requires C++ linkage).
 * ============================================================================ */

/*
extern size_t get_ascend_free_mem_for_shim(void);
extern int    update_ascend_mem_usage_for_shim(size_t bytes, int is_allocate);
extern void   get_ascend_mem_info_for_shim(size_t *free_bytes, size_t *total_bytes);
*/

/* ============================================================================
 * TODO: Implement interceptors when CANN SDK is available.
 * ============================================================================ */

/*
aclError aclrtMalloc(void **devPtr, size_t size, aclrtMemMallocPolicy policy) {
    // TODO: Check quota, call real aclrtMalloc, update accounting
    // typedef aclError (*real_fn_t)(void **, size_t, aclrtMemMallocPolicy);
    // static real_fn_t real_fn = NULL;
    // if (!real_fn) real_fn = (real_fn_t)dlsym(RTLD_NEXT, "aclrtMalloc");
    return ACL_SUCCESS;
}

aclError aclrtFree(void *devPtr) {
    // TODO: Look up size, update accounting, call real aclrtFree
    return ACL_SUCCESS;
}

aclError aclrtGetMemInfo(int32_t attr, size_t *free, size_t *total) {
    // TODO: Return virtualized memory info
    return ACL_SUCCESS;
}
*/
