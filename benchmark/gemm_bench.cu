// SPDX-License-Identifier: GPL-2.0-only
// gpu-unlock-rt · CUDA judge: cuBLAS 4096x4096 GEMM, three-run medians.
// Methodology matches upstream reports (CUDA 12.6, FP16/BF16 tensor with FP32
// accumulation, FP32 pedantic reference). TFLOPS = 2*M*N*K / median_seconds.
#include <cublas_v2.h>
#include <cuda_fp16.h>
#include <cuda_bf16.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>

#define CHK(x) do { cudaError_t _ce = (x); if (_ce != cudaSuccess) { \
    printf("CUDA error %s at %d\n", cudaGetErrorString(_ce), __LINE__); exit(1); } } while (0)
#define BCHK(x) do { cublasStatus_t _bs = (x); if (_bs != CUBLAS_STATUS_SUCCESS) { \
    printf("cuBLAS error %d at %d\n", (int)_bs, __LINE__); exit(1); } } while (0)

static const int N = 4096;
static const int RUNS = 5;   // discard first, median of the rest

template <typename T>
double bench(cublasHandle_t handle, cudaDataType_t atype, cublasComputeType_t ctype, cublasGemmAlgo_t algo)
{
    size_t bytes = (size_t)N * N * sizeof(T);
    void *dA, *dB, *dC;
    CHK(cudaMalloc(&dA, bytes)); CHK(cudaMalloc(&dB, bytes)); CHK(cudaMalloc(&dC, bytes));
    std::vector<T> h(N * N);
    for (auto &v : h) v = T(1.0f / 3.0f);
    CHK(cudaMemcpy(dA, h.data(), bytes, cudaMemcpyHostToDevice));
    CHK(cudaMemcpy(dB, h.data(), bytes, cudaMemcpyHostToDevice));

    const float alpha = 1.0f, beta = 0.0f;
    cudaEvent_t s, e;
    CHK(cudaEventCreate(&s)); CHK(cudaEventCreate(&e));
    std::vector<float> ms(RUNS);
    for (int r = 0; r < RUNS; r++) {
        CHK(cudaEventRecord(s));
        BCHK(cublasGemmEx(handle, CUBLAS_OP_N, CUBLAS_OP_N, N, N, N,
                          &alpha, dA, atype, N, dB, atype, N,
                          &beta, dC, atype, N, ctype, algo));
        CHK(cudaEventRecord(e));
        CHK(cudaEventSynchronize(e));
        CHK(cudaEventElapsedTime(&ms[r], s, e));
    }
    std::sort(ms.begin(), ms.end());
    double sec = ms[RUNS / 2] / 1000.0;
    double tflops = 2.0 * N * N * N / sec / 1e12;
    CHK(cudaFree(dA)); CHK(cudaFree(dB)); CHK(cudaFree(dC));
    CHK(cudaEventDestroy(s)); CHK(cudaEventDestroy(e));
    return tflops;
}

int main()
{
    int dev; CHK(cudaGetDevice(&dev));
    cudaDeviceProp prop; CHK(cudaGetDeviceProperties(&prop, dev));
    printf("device : %s (SM %d.%d, %d SMs)\n", prop.name, prop.major, prop.minor, prop.multiProcessorCount);
    cublasHandle_t h; BCHK(cublasCreate(&h));
    BCHK(cublasSetMathMode(h, CUBLAS_TENSOR_OP_MATH));

    printf("FP32 pedantic      : %8.2f TFLOPS\n", bench<float>(h, CUDA_R_32F, CUBLAS_COMPUTE_32F_PEDANTIC, CUBLAS_GEMM_DEFAULT));
    printf("FP16 tensor/FP32acc: %8.2f TFLOPS\n", bench<__half>(h, CUDA_R_16F, CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT_TENSOR_OP));
    printf("BF16 tensor/FP32acc: %8.2f TFLOPS\n", bench<__nv_bfloat16>(h, CUDA_R_16BF, CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT_TENSOR_OP));
    cublasDestroy(h);
    return 0;
}
