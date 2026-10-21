// CUDA device kernels. All exported entry points use the C ABI and row-major data.
extern "C" __global__ void vec_add(const float *A, const float *B, float *C, int n)
{
    int i = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    if (i < n)
        C[i] = A[i] + B[i];
}
extern "C" __global__ void index_trace(int *bid, int *tid, int *gid, int *visits, int n)
{
    int i = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    if (i < n)
    {
        bid[i] = (int)blockIdx.x;
        tid[i] = (int)threadIdx.x;
        gid[i] = i;
        atomicAdd(visits + i, 1);
    }
}
extern "C" __global__ void matmul_naive(const float *A, const float *B, float *C, int M, int K, int N)
{
    int row = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    int col = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    if (row < M && col < N)
    {
        float sum = 0.0f;
        for (int k = 0; k < K; k++)
            sum += A[row * K + k] * B[k * N + col];
        C[row * N + col] = sum;
    }
}
extern "C" __global__ void matmul_tiled(const float *A, const float *B, float *C, int M, int K, int N)
{
    __shared__ float tileA[16][16], tileB[16][16];
    int row = (int)(blockIdx.y * 16 + threadIdx.y), col = (int)(blockIdx.x * 16 + threadIdx.x);
    float sum = 0.0f;
    for (int base = 0; base < K; base += 16)
    {
        int ak = base + (int)threadIdx.x, bk = base + (int)threadIdx.y;
        tileA[threadIdx.y][threadIdx.x] = (row < M && ak < K) ? A[row * K + ak] : 0.0f;
        tileB[threadIdx.y][threadIdx.x] = (bk < K && col < N) ? B[bk * N + col] : 0.0f;
        // Every thread reaches both barriers, including out-of-range threads.
        __syncthreads();
#pragma unroll
        for (int k = 0; k < 16; k++)
            sum += tileA[threadIdx.y][k] * tileB[k][threadIdx.x];
        __syncthreads();
    }
    if (row < M && col < N)
        C[row * N + col] = sum;
}
