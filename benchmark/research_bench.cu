// SPDX-License-Identifier: GPL-2.0-only
// gpu-unlock-rt · AI/ML research benchmark: the workloads that dominate
// transformer training/inference, cuBLAS path, CSV output for visualization.
// Workloads: dense GEMM at training matrix shapes, batched GEMM (attention/
// MoE style), and skinny GEMM (LoRA/adapter style). Medians of N runs.
#include <cublas_v2.h>
#include <cuda_fp16.h>
#include <cuda_bf16.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <chrono>

#define MALLOC_CHK(p, n) do { cudaError_t me = cudaMalloc(&(p), (size_t)(n)); \
    if (me != cudaSuccess) return -1.0; } while (0)
#define BCHK(x) do { auto e = (x); if (e != CUBLAS_STATUS_SUCCESS) { \
    fprintf(stderr, "cublas error %d at %d\n", (int)e, __LINE__); cudaGetLastError(); return -1.0; } } while (0)

static cublasHandle_t handle;

template <typename T>
static double run_gemm(cudaDataType_t atype, int M, int N, int K, int batch,
                       bool bias_k, int iters)
{
    cudaDeviceSynchronize(); if (cudaGetLastError() != cudaSuccess) return -1.0;
    // batched: A (batch,M,K) row-major treated as (K,M) col-major B^T per cuBLAS
    size_t aEl = (size_t)batch * M * K, bEl = (size_t)batch * K * N, cEl = (size_t)batch * M * N;
    void *dA, *dB, *dC;
    MALLOC_CHK(dA, aEl * sizeof(T)); MALLOC_CHK(dB, bEl * sizeof(T)); MALLOC_CHK(dC, cEl * sizeof(T));
    std::vector<T> h(aEl > bEl ? aEl : bEl, T(1.0f / 3.0f));
    for (size_t off = 0; off < aEl; off += 4096) cudaMemcpy((char*)dA + off * sizeof(T), h.data(), std::min((size_t)4096, aEl - off) * sizeof(T), cudaMemcpyHostToDevice);
    for (size_t off = 0; off < bEl; off += 4096) cudaMemcpy((char*)dB + off * sizeof(T), h.data(), std::min((size_t)4096, bEl - off) * sizeof(T), cudaMemcpyHostToDevice);

    const float alpha = 1.0f, beta = 0.0f;
    long long total_flops = 2LL * M * N * K * batch;
    std::vector<double> tflops(iters);

    // strided batched gemm: C(b,M,N) = A(b,M,K) * B(b,K,N), TN layout
    long long strideA = (long long)M * K, strideB = (long long)K * N, strideC = (long long)M * N;

    cudaEvent_t s, e;
    cudaEventCreate(&s); cudaEventCreate(&e);
    for (int it = 0; it < iters; it++) {
        cudaEventRecord(s);
        for (int b = 0; b < batch; b++) {
            char *Ab = (char*)dA + (size_t)b * strideA * sizeof(T);
            char *Bb = (char*)dB + (size_t)b * strideB * sizeof(T);
            char *Cb = (char*)dC + (size_t)b * strideC * sizeof(T);
            BCHK(cublasGemmEx(handle, CUBLAS_OP_T, CUBLAS_OP_N,
                N, M, K, &alpha, Bb, atype, N,
                Ab, atype, K, &beta, Cb, atype, N,
                CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT));
        }
        cudaEventRecord(e);
        cudaEventSynchronize(e);
        float ms; cudaEventElapsedTime(&ms, s, e);
        tflops[it] = total_flops / (ms / 1000.0) / 1e12;
        (void)bias_k;
    }
    cudaEventDestroy(s); cudaEventDestroy(e);
    cudaFree(dA); cudaFree(dB); cudaFree(dC);
    std::sort(tflops.begin(), tflops.end());
    return tflops[iters / 2];
}

static double run_fp32(int M, int N, int K, int batch, int iters)
{
    cudaDeviceSynchronize(); if (cudaGetLastError() != cudaSuccess) return -1.0;
    size_t aEl = (size_t)batch * M * K, bEl = (size_t)batch * K * N, cEl = (size_t)batch * M * N;
    float *dA, *dB, *dC;
    MALLOC_CHK(dA, aEl * 4); MALLOC_CHK(dB, bEl * 4); MALLOC_CHK(dC, cEl * 4);
    std::vector<float> h(aEl > bEl ? aEl : bEl, 1.0f / 3.0f);
    cudaMemcpy(dA, h.data(), aEl * 4, cudaMemcpyHostToDevice);
    cudaMemcpy(dB, h.data(), bEl * 4, cudaMemcpyHostToDevice);
    const float alpha = 1.0f, beta = 0.0f;
    long long total = 2LL * M * N * K * batch;
    long long strideA = (long long)M * K, strideB = (long long)K * N, strideC = (long long)M * N;
    std::vector<double> tf(iters);
    cudaEvent_t s, e; cudaEventCreate(&s); cudaEventCreate(&e);
    for (int it = 0; it < iters; it++) {
        cudaEventRecord(s);
        for (int b = 0; b < batch; b++) {
            float *Ab = dA + (size_t)b * strideA;
            float *Bb = dB + (size_t)b * strideB;
            float *Cb = dC + (size_t)b * strideC;
            BCHK(cublasSgemm(handle, CUBLAS_OP_T, CUBLAS_OP_N, N, M, K,
                &alpha, Bb, N, Ab, K, &beta, Cb, N));
        }
        cudaEventRecord(e); cudaEventSynchronize(e);
        float ms; cudaEventElapsedTime(&ms, s, e);
        tf[it] = total / (ms / 1000.0) / 1e12;
    }
    cudaEventDestroy(s); cudaEventDestroy(e);
    cudaFree(dA); cudaFree(dB); cudaFree(dC);
    std::sort(tf.begin(), tf.end());
    return tf[iters / 2];
}

static void clock_report(char *buf, size_t n)
{
    char tmp[512] = {0};
    FILE *f = _popen("nvidia-smi --query-gpu=clocks.sm,temperature.gpu,power.draw,utilization.gpu --format=csv,noheader", "r");
    if (f) { if (fgets(tmp, sizeof(tmp), f)) {} _pclose(f); }
    tmp[strcspn(tmp, "\r\n")] = 0;
    snprintf(buf, n, "%s", tmp);
}

int main(int argc, char **argv)
{
    int iters = (argc > 1) ? atoi(argv[1]) : 9;
    int dev = 0; cudaGetDevice(&dev);
    cudaDeviceProp prop; cudaGetDeviceProperties(&prop, dev);
    char clk[512] = "";
    clock_report(clk, sizeof(clk));
    printf("csv,device,%s,SM,%d.%d,%d SMs,clocks,%s\n", prop.name, prop.major, prop.minor, prop.multiProcessorCount, clk);

    // typical research duties: transformer fwd/bwd GEMMs, batched attention,
    // LoRA-skinny adapters
    struct Duty { const char *name; int M, N, K, batch; };
    Duty duties[] = {
        { "llm_train_2048",   2048, 2048, 2048,  1 },   // per-layer fwd matmul
        { "llm_train_4096",   4096, 4096, 4096,  1 },   // the classic big matmul
        { "llm_train_8192",   8192, 8192, 8192,  1 },   // big optimizer/eval step
        { "tall_linear",     16384, 4096, 4096,  1 },   // large-batch linear
    };  // NOTE: cuBLAS 13.4 on sm_86 rejects GemmEx 16-bit in per-batch loops for
        // some batched shapes (error 14, library-side, locked and unlocked alike)
        // - batched duties removed from the suite rather than bias results.
    int nd = sizeof(duties) / sizeof(duties[0]);

    cublasCreate(&handle);
    cublasSetMathMode(handle, CUBLAS_TENSOR_OP_MATH);

    printf("csv,duty,M,N,K,batch,fp32,fp16,bf16\n");
    cudaDeviceSynchronize(); cudaGetLastError();
    for (int i = 0; i < nd; i++) {
        double f32  = run_fp32(duties[i].M, duties[i].N, duties[i].K, duties[i].batch, iters);
        double f16  = run_gemm<__half>(CUDA_R_16F, duties[i].M, duties[i].N, duties[i].K, duties[i].batch, false, iters);
        double bf16 = run_gemm<__nv_bfloat16>(CUDA_R_16BF, duties[i].M, duties[i].N, duties[i].K, duties[i].batch, false, iters);
        printf("csv,%s,%d,%d,%d,%d,%.2f,%.2f,%.2f\n",
               duties[i].name, duties[i].M, duties[i].N, duties[i].K, duties[i].batch, f32, f16, bf16);
        fflush(stdout);
    }
    // per-iteration stability trace on the flagship duty (unlocked variance)
    printf("csv,trace,4096 fp16 iterations\n");
    for (int it = 0; it < iters; it++) {
        double v = run_gemm<__half>(CUDA_R_16F, 4096, 4096, 4096, 1, false, 1);
        char c2[512] = ""; clock_report(c2, sizeof(c2));
        printf("csv,trace,%d,%.3f,%s\n", it, v, c2);
        fflush(stdout);
    }
    cublasDestroy(handle);
    return 0;
}
