#include <cuda_runtime.h>

extern "C" __global__ void vector_add(const float *a, const float *b,
                                      float *c, unsigned int n)
{
    unsigned int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) c[i] = a[i] + b[i];
}

extern "C" __global__ void memory_triad(const float *a, const float *b,
                                        float *c, unsigned int n)
{
    unsigned int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) c[i] = a[i] + 2.0f * b[i];
}

/* Eight independent recurrence chains. Runtime loop count and explicit rounded
   FMA prevent replacing the loop with a closed-form expression. */
extern "C" __global__ void fp32_fma(float *out, unsigned int n,
                                    unsigned int loops, float seed)
{
    unsigned int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float x0 = seed + (float)((i & 63u) +   1u) / 1024.0f;
    float x1 = seed + (float)((i & 63u) +  65u) / 1024.0f;
    float x2 = seed + (float)((i & 63u) + 129u) / 1024.0f;
    float x3 = seed + (float)((i & 63u) + 193u) / 1024.0f;
    float x4 = seed + (float)((i & 63u) + 257u) / 1024.0f;
    float x5 = seed + (float)((i & 63u) + 321u) / 1024.0f;
    float x6 = seed + (float)((i & 63u) + 385u) / 1024.0f;
    float x7 = seed + (float)((i & 63u) + 449u) / 1024.0f;
    for (unsigned int k = 0; k < loops; ++k) {
        x0 = __fmaf_rn(x0, 1.00000011920928955078125f, 0.000001f);
        x1 = __fmaf_rn(x1, 1.00000011920928955078125f, 0.000001f);
        x2 = __fmaf_rn(x2, 1.00000011920928955078125f, 0.000001f);
        x3 = __fmaf_rn(x3, 1.00000011920928955078125f, 0.000001f);
        x4 = __fmaf_rn(x4, 1.00000011920928955078125f, 0.000001f);
        x5 = __fmaf_rn(x5, 1.00000011920928955078125f, 0.000001f);
        x6 = __fmaf_rn(x6, 1.00000011920928955078125f, 0.000001f);
        x7 = __fmaf_rn(x7, 1.00000011920928955078125f, 0.000001f);
    }
    out[i * 8u + 0u] = x0; out[i * 8u + 1u] = x1;
    out[i * 8u + 2u] = x2; out[i * 8u + 3u] = x3;
    out[i * 8u + 4u] = x4; out[i * 8u + 5u] = x5;
    out[i * 8u + 6u] = x6; out[i * 8u + 7u] = x7;
}
