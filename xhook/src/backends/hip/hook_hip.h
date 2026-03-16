#ifndef _HIPHOOK_H_
#define _HIPHOOK_H_

// HIP Driver API hook symbols — for Hygon DCU (海光 DCU) support
// HIP does not have a clean driver/runtime separation like CUDA.
// Enforcement should default to "runtime" layer.

typedef enum HipHookSymbolsEnum {
  HIP_HOOK_MEM_ALLOC,
  HIP_HOOK_MEM_FREE,
  HIP_HOOK_LAUNCH_KERNEL,
  HIP_HOOK_MODULE_LAUNCH_KERNEL,
  HIP_HOOK_DEVICE_TOTAL_MEM,
  HIP_HOOK_MEM_GET_INFO,
  HIP_HOOK_CTX_SYNC,
  NUM_HIP_HOOK_SYMBOLS,
} HipHookSymbols;

#endif /* _HIPHOOK_H_ */
