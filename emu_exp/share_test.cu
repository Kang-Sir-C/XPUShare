// share_test.cu
// 用于测试 XPUShare/xhook 的算力隔离与时间片共享效果。
// 在多个 Pod 内同时运行本程序，结合不同的 gpu_request/gpu_limit，观察每个 Job 的迭代速率差异。

#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define N          2048
#define BLOCK_SIZE 32

// 简单的矩阵乘法 Kernel，计算量较大，适合作为持续负载
__global__ void matrixMul(const float *a, const float *b, float *c, int n) {
    int row = blockIdx.y * blockDim.y + threadIdx.y;
    int col = blockIdx.x * blockDim.x + threadIdx.x;
    float sum = 0.0f;
    if (row < n && col < n) {
        for (int i = 0; i < n; ++i) {
            sum += a[row * n + i] * b[i * n + col];
        }
        c[row * n + col] = sum;
    }
}

static double get_time_sec() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

int main(int argc, char **argv) {
    const char *job_name = getenv("JOB_NAME");
    if (!job_name || job_name[0] == '\0') job_name = "default";

    // 运行总时长（秒）可以通过命令行参数指定，默认 120 秒
    double total_duration = 120.0;
    if (argc >= 2) {
        total_duration = atof(argv[1]);
        if (total_duration <= 0.0) total_duration = 120.0;
    }

    printf("====== [share_test] Job=%s, N=%d, total_duration=%.1f s ======\n",
           job_name, N, total_duration);
    fflush(stdout);

    size_t size = (size_t)N * (size_t)N * sizeof(float);
    float *h_a = (float *)malloc(size);
    float *h_b = (float *)malloc(size);
    float *h_c = (float *)malloc(size);
    if (!h_a || !h_b || !h_c) {
        fprintf(stderr, "[%s] Host malloc failed\n", job_name);
        return -1;
    }

    // 初始化 host 数据
    for (size_t i = 0; i < (size_t)N * (size_t)N; ++i) {
        h_a[i] = 1.0f;
        h_b[i] = 2.0f;
    }

    // 显示一下虚拟显存视图（会经过 shim+hook）
    size_t free_b = 0, total_b = 0;
    if (cudaMemGetInfo(&free_b, &total_b) == cudaSuccess) {
        printf("[%s] Before alloc: cudaMemGetInfo: free=%.2f MB, total=%.2f MB\n",
               job_name,
               (double)free_b / 1024 / 1024,
               (double)total_b / 1024 / 1024);
    }

    // 分配 GPU 内存
    float *d_a = NULL, *d_b = NULL, *d_c = NULL;
    cudaError_t err;
    err = cudaMalloc(&d_a, size);
    if (err != cudaSuccess) {
        fprintf(stderr, "[%s] cudaMalloc d_a failed: %s\n",
                job_name, cudaGetErrorString(err));
        return -1;
    }
    err = cudaMalloc(&d_b, size);
    if (err != cudaSuccess) {
        fprintf(stderr, "[%s] cudaMalloc d_b failed: %s\n",
                job_name, cudaGetErrorString(err));
        return -1;
    }
    err = cudaMalloc(&d_c, size);
    if (err != cudaSuccess) {
        fprintf(stderr, "[%s] cudaMalloc d_c failed: %s\n",
                job_name, cudaGetErrorString(err));
        return -1;
    }

    // 拷贝数据到 GPU
    err = cudaMemcpy(d_a, h_a, size, cudaMemcpyHostToDevice);
    if (err != cudaSuccess) {
        fprintf(stderr, "[%s] cudaMemcpy d_a failed: %s\n",
                job_name, cudaGetErrorString(err));
        return -1;
    }
    err = cudaMemcpy(d_b, h_b, size, cudaMemcpyHostToDevice);
    if (err != cudaSuccess) {
        fprintf(stderr, "[%s] cudaMemcpy d_b failed: %s\n",
                job_name, cudaGetErrorString(err));
        return -1;
    }

    dim3 threadsPerBlock(BLOCK_SIZE, BLOCK_SIZE);
    dim3 blocksPerGrid((N + BLOCK_SIZE - 1) / BLOCK_SIZE,
                       (N + BLOCK_SIZE - 1) / BLOCK_SIZE);

    printf("[%s] Start compute loop...\n", job_name);
    fflush(stdout);

    double start_time = get_time_sec();
    double last_window_time = start_time;
    const double WINDOW_SEC = 1.0;  // 每 1 秒打印一次统计
    long long total_iters = 0;
    long long window_iters = 0;
    int window_idx = 0;

    while (1) {
        double now = get_time_sec();
        if (now - start_time >= total_duration) break;

        // 发射一个 kernel
        matrixMul<<<blocksPerGrid, threadsPerBlock>>>(d_a, d_b, d_c, N);
        err = cudaDeviceSynchronize();  // 确保这一轮算完
        if (err != cudaSuccess) {
            fprintf(stderr, "\n[%s] cudaDeviceSynchronize error: %s\n",
                    job_name, cudaGetErrorString(err));
            break;
        }

        total_iters++;
        window_iters++;

        now = get_time_sec();
        if (now - last_window_time >= WINDOW_SEC) {
            double window_elapsed = now - last_window_time;
            double avg_iter_time = (window_iters > 0)
                                     ? window_elapsed / window_iters
                                     : 0.0;
            printf("[%s] window=%-4d iters=%-6lld "
                   "window_elapsed=%.3f s avg_time/iter=%.4f s\n",
                   job_name, window_idx, window_iters,
                   window_elapsed, avg_iter_time);
            fflush(stdout);
            last_window_time = now;
            window_iters = 0;
            window_idx++;
        }
    }

    double total_elapsed = get_time_sec() - start_time;
    double avg_per_iter = (total_iters > 0) ? total_elapsed / total_iters : 0.0;
    printf("[%s] DONE. total_iters=%lld total_elapsed=%.2f s avg_time/iter=%.4f s\n",
           job_name, total_iters, total_elapsed, avg_per_iter);

    cudaFree(d_a);
    cudaFree(d_b);
    cudaFree(d_c);
    free(h_a);
    free(h_b);
    free(h_c);

    return 0;
}
