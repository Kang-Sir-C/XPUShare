#ifndef _MUSAHOOK_H_
#define _MUSAHOOK_H_

// MUSA Driver API hook symbols
// TODO: Populate when MUSA SDK is available

typedef enum MuHookSymbolsEnum {
  MU_HOOK_GET_PROC_ADDRESS,
  MU_HOOK_MEM_ALLOC,
  MU_HOOK_MEM_FREE,
  MU_HOOK_LAUNCH_KERNEL,
  MU_HOOK_DEVICE_TOTAL_MEM,
  MU_HOOK_MEM_INFO,
  MU_HOOK_CTX_SYNC,
  NUM_MU_HOOK_SYMBOLS,
} MuHookSymbols;

#endif /* _MUSAHOOK_H_ */
