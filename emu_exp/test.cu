#include<stdio.h>
#include <cuda_runtime.h>

__global__ void hello_world(void)
{
  printf("GPU: Hello world!\n"); // GPU输出的Hello World!
}

int main(int argc,char **argv)
{
  printf("CPU: Hello world!\n"); // CPU输出的Hello World!
  int devCount;
  cudaGetDeviceCount(&devCount);
  printf("The host has %d GPUs.\n", devCount);

  cudaDeviceProp devProp;
  for(unsigned int i = 0; i < devCount; i++) 
  {
    cudaGetDeviceProperties(&devProp, i); // Decide if device has sufficient resources/capabilities
    printf("GPU %d: %s\n" ,i, devProp.name);
    printf("\tNum of SMs: %d\n", devProp.multiProcessorCount);  
    printf("\tmaxThreadsPerBlock: %d\n", devProp.maxThreadsPerBlock);  
    printf("\tclockRate: %d\n", devProp.clockRate);  
    printf("\tGPU %d has compute capability %d.%d.\n", i, devProp.major, devProp.minor);
    printf("\tregsPerBlock: %d\n", devProp.regsPerBlock);  
    printf("\twarpSize: %d\n", devProp.warpSize);  
    printf("\tTotal amount of global memory: %.2f MBytes (%llu bytes)\n", (float)devProp.totalGlobalMem/(1024.0*1024*1024), \
		    (unsigned long long) devProp.totalGlobalMem);
    printf("\tGPU Clock rate: %.0f MHz (%0.2f GHz)\n", devProp.clockRate * 1e-3f, devProp.clockRate * 1e-6f);
    printf("\tMemory Clock rate: %.0f Mhz\n", devProp.memoryClockRate * 1e-3f);
    printf("\t Memory Bus Width: %d-bit\n", devProp.memoryBusWidth);
    if (devProp.l2CacheSize) {
      printf("\tL2 Cache Size: %d bytes\n", devProp.l2CacheSize);
    }


    printf("\tMax Texture Dimension Size (x,y,z) "
           " 1D=(%d), 2D=(%d,%d), 3D=(%d,%d,%d)\n",
             devProp.maxTexture1D , devProp.maxTexture2D[0], devProp.maxTexture2D[1],
             devProp.maxTexture3D[0], devProp.maxTexture3D[1], devProp.maxTexture3D[2]);
    printf("\tMax Layered Texture Size (dim) x layers 1D=(%d) x %d, 2D=(%d,%d) x %d\n",
             devProp.maxTexture1DLayered[0], devProp.maxTexture1DLayered[1], devProp.maxTexture2DLayered[0], 
	     devProp.maxTexture2DLayered[1], devProp.maxTexture2DLayered[2]);
    printf("\tTotal amount of constant memory: %lu bytes\n", devProp.totalConstMem);
printf("\tTotal amount of shared memory per block: %lu bytes\n", devProp.sharedMemPerBlock);
printf("\tTotal number of registers available per block: %d\n", devProp.regsPerBlock);
printf("\tWarp size: %d\n", devProp.warpSize);
printf("\tMaximum number of threads per multiprocessor: %d\n", devProp.maxThreadsPerMultiProcessor);
printf("\tMaximum number of threads per block: %d\n", devProp.maxThreadsPerBlock);
printf("\tMaximum sizes of each dimension of a block: %d x %d x %d\n", devProp.maxThreadsDim[0], devProp.maxThreadsDim[1], devProp.maxThreadsDim[2]);
printf("\tMaximum sizes of each dimension of a grid: %d x %d x %d\n", devProp.maxGridSize[0], devProp.maxGridSize[1], devProp.maxGridSize[2]);
printf("\tMaximum memory pitch: %lu bytes\n", devProp.memPitch);

  
  }

  hello_world<<<1,10>>>();
  cudaDeviceReset();             // 如果没有这一行就看不到GPU输出的Hello World!
  return 0;
}
