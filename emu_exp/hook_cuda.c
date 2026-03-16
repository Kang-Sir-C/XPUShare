#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <cuda_runtime_api.h>

static cudaError_t (*real_cudaGetDeviceProperties)(struct cudaDeviceProp*, int);
static cudaError_t (*real_cudaMalloc)(void**, size_t);
static cudaError_t (*real_cudaMemcpy)(void*, const void*, size_t, enum cudaMemcpyKind);
static cudaError_t (*real_cudaDeviceSynchronize)(void);

static const char* memcpyKindStr(enum cudaMemcpyKind k){
    switch(k){
        case cudaMemcpyHostToHost:   return "H2H";
        case cudaMemcpyHostToDevice: return "H2D";
        case cudaMemcpyDeviceToHost: return "D2H";
        case cudaMemcpyDeviceToDevice:return "D2D";
        case cudaMemcpyDefault:      return "Default";
        default: return "Unknown";
    }
}

__attribute__((constructor))
static void init(void){
    real_cudaGetDeviceProperties = dlsym(RTLD_NEXT, "cudaGetDeviceProperties");
    real_cudaMalloc = dlsym(RTLD_NEXT, "cudaMalloc");
    real_cudaMemcpy = dlsym(RTLD_NEXT, "cudaMemcpy");
    real_cudaDeviceSynchronize = dlsym(RTLD_NEXT, "cudaDeviceSynchronize");
    fprintf(stderr, "[HOOK] init: props=%p malloc=%p memcpy=%p sync=%p\n",
            (void*)real_cudaGetDeviceProperties, (void*)real_cudaMalloc,
            (void*)real_cudaMemcpy, (void*)real_cudaDeviceSynchronize);
}

cudaError_t cudaGetDeviceProperties(struct cudaDeviceProp *prop, int device){
    fprintf(stderr, "[HOOK] cudaGetDeviceProperties(dev=%d)\n", device);
    cudaError_t err = real_cudaGetDeviceProperties(prop, device);
    if (err == cudaSuccess && prop){
        fprintf(stderr, "[HOOK] name=%s, CC=%d.%d, SM=%d, warp=%d, L2=%d, mem=%.2fGiB\n",
            prop->name, prop->major, prop->minor, prop->multiProcessorCount, prop->warpSize,
            prop->l2CacheSize, (double)prop->totalGlobalMem/(1024.0*1024*1024));
    }
    return err;
}
cudaError_t cudaMalloc(void **ptr, size_t sz){
    fprintf(stderr, "[HOOK] cudaMalloc(sz=%zu)\n", sz);
    cudaError_t err = real_cudaMalloc(ptr, sz);
    fprintf(stderr, "[HOOK] cudaMalloc ret=%d, ptr=%p\n", (int)err, ptr ? (void*)*ptr : NULL);
    return err;
}
cudaError_t cudaMemcpy(void *dst, const void *src, size_t sz, enum cudaMemcpyKind kind){
    fprintf(stderr, "[HOOK] cudaMemcpy(sz=%zu, kind=%s)\n", sz, memcpyKindStr(kind));
    return real_cudaMemcpy(dst, src, sz, kind);
}
cudaError_t cudaDeviceSynchronize(void){
    fprintf(stderr, "[HOOK] cudaDeviceSynchronize()\n");
    return real_cudaDeviceSynchronize();
}

static cudaError_t (*real_cudaLaunchKernel)(
    const void *func, dim3 gridDim, dim3 blockDim,
    void **args, size_t sharedMem, cudaStream_t stream);

cudaError_t cudaLaunchKernel(
    const void *func, dim3 gridDim, dim3 blockDim,
    void **args, size_t sharedMem, cudaStream_t stream)
{
    if (!real_cudaLaunchKernel) {
        real_cudaLaunchKernel = dlsym(RTLD_NEXT, "cudaLaunchKernel");
    }
    fprintf(stderr,
        "[HOOK] cudaLaunchKernel grid=(%u,%u,%u) block=(%u,%u,%u) "
        "shmem=%zu stream=%p func=%p\n",
        gridDim.x, gridDim.y, gridDim.z,
        blockDim.x, blockDim.y, blockDim.z,
        sharedMem, (void*)stream, func);
    return real_cudaLaunchKernel(func, gridDim, blockDim, args, sharedMem, stream);
}