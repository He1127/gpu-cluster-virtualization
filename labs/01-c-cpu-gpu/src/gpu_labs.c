/* Genuine C host program: compile with cl /TC. CUDA Driver API controls device work. */
#include <windows.h>

#include <cuda.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")

static FILE *runlog = NULL, *apilog = NULL;
static CUdevice device;
static CUcontext context;
static CUmodule module;
static int debug_mode = 0, active_step = 0;
volatile int g_debug_stage = 0;
static LARGE_INTEGER qpf;
static void stamp(char *b, size_t n);
static double seconds(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (double)q.QuadPart / (double)qpf.QuadPart;
}
static int hash_file(const char *path, char output[65])
{
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    unsigned char buffer[65536], digest[32];
    DWORD length = 32;
    size_t n;
    int i, ok = 0;
    FILE *input = fopen(path, "rb");
    output[0] = 0;
    if (!input)
        return 0;
    if (!CryptAcquireContextA(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        goto done;
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash))
        goto done;
    while ((n = fread(buffer, 1, sizeof(buffer), input)) > 0)
        if (!CryptHashData(hash, buffer, (DWORD)n, 0))
            goto done;
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0))
        goto done;
    for (i = 0; i < 32; i++)
        sprintf(output + i * 2, "%02x", digest[i]);
    output[64] = 0;
    ok = 1;
done:
    if (hash)
        CryptDestroyHash(hash);
    if (provider)
        CryptReleaseContext(provider, 0);
    fclose(input);
    return ok;
}
static void write_session(int step, int result)
{
    const char *paths[] = {"src/gpu_labs.c", "src/kernels.cu", "build/kernels.cubin", "bin/gpu_labs.exe"};
    char path[256], hash[65], timestamp[64];
    int i;
    FILE *f;
    stamp(timestamp, sizeof(timestamp));
    snprintf(path, sizeof(path), "results/%02d_session.json", step);
    f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f,
            "{\n  \"step\":%d,\n  \"recorded_at_local\":\"%s\",\n  \"result_code\":%d,\n  \"host_language\":\"C (cl "
            "/TC)\",\n  \"controller\":\"CUDA Driver API\",\n  \"sha256\":{\n",
            step, timestamp, result);
    for (i = 0; i < 4; i++)
    {
        hash_file(paths[i], hash);
        fprintf(f, "    \"%s\":\"%s\"%s\n", paths[i], hash, i < 3 ? "," : "");
    }
    fprintf(f, "  }\n}\n");
    fclose(f);
}
static void stamp(char *b, size_t n)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(b, n, "%04u-%02u-%02uT%02u:%02u:%02u.%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
             t.wMilliseconds);
}
static void say(const char *fmt, ...)
{
    char b[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(b, sizeof(b), fmt, args);
    va_end(args);
    fputs(b, stdout);
    fflush(stdout);
    OutputDebugStringA(b);
    if (runlog)
    {
        fputs(b, runlog);
        fflush(runlog);
    }
}
static void check(CUresult r, const char *expr, int line)
{
    const char *name = "?", *desc = "?";
    char ts[64];
    stamp(ts, sizeof(ts));
    cuGetErrorName(r, &name);
    cuGetErrorString(r, &desc);
    if (apilog)
    {
        fprintf(apilog, "%s line=%d %s => %d %s\n", ts, line, expr, (int)r, name ? name : "?");
        fflush(apilog);
    }
    if (r != CUDA_SUCCESS)
    {
        say("CUDA ERROR line %d: %s => %s: %s\n", line, expr, name ? name : "?", desc ? desc : "?");
        exit(2);
    }
}
#define CU(x) check((x), #x, __LINE__)
static void *mem(size_t n)
{
    void *p = malloc(n);
    if (!p)
    {
        say("Host allocation failed: %zu bytes\n", n);
        exit(3);
    }
    return p;
}
static void pause_debug(int stage, const char *label)
{
    g_debug_stage = stage;
    if (debug_mode)
    {
        say("DEBUG stage=%d %s; host arrays inspectable in Locals\n", stage, label);
        if (IsDebuggerPresent())
            DebugBreak();
        else
            say("No debugger attached; continuing without a breakpoint.\n");
    }
}
static void init_device(int need_context)
{
    CU(cuInit(0));
    CU(cuDeviceGet(&device, 0));
    if (need_context)
    {
        CU(cuCtxCreate(&context, 0, device));
        CU(cuModuleLoad(&module, "build\\kernels.cubin"));
    }
}
static CUfunction function(const char *name)
{
    CUfunction f;
    CU(cuModuleGetFunction(&f, module, name));
    return f;
}
static void finish(void)
{
    if (module)
    {
        CU(cuModuleUnload(module));
        module = NULL;
    }
    if (context)
    {
        CU(cuCtxDestroy(context));
        context = NULL;
    }
}
static void launch_mm(CUfunction f, CUdeviceptr a, CUdeviceptr b, CUdeviceptr c, int M, int K, int N)
{
    void *args[] = {&a, &b, &c, &M, &K, &N};
    CU(cuLaunchKernel(f, (N + 15) / 16, (M + 15) / 16, 1, 16, 16, 1, 0, NULL, args, NULL));
}
/* No filesystem/debug-output calls inside timing windows. Check/log afterward. */
static CUresult launch_mm_raw(CUfunction f, CUdeviceptr a, CUdeviceptr b, CUdeviceptr c, int M, int K, int N)
{
    void *args[] = {&a, &b, &c, &M, &K, &N};
    return cuLaunchKernel(f, (N + 15) / 16, (M + 15) / 16, 1, 16, 16, 1, 0, NULL, args, NULL);
}
static unsigned rng = 20261016u;
static float random_value(void)
{
    rng = 1664525u * rng + 1013904223u;
    return ((float)((rng >> 8) & 65535u) / 32768.0f) - 1.0f;
}
static int attribute(CUdevice_attribute a)
{
    int v;
    CU(cuDeviceGetAttribute(&v, a, device));
    return v;
}
static int step05(void)
{
    char name[256];
    size_t bytes;
    int version;
    init_device(0);
    CU(cuDeviceGetName(name, sizeof(name), device));
    CU(cuDeviceTotalMem(&bytes, device));
    CU(cuDriverGetVersion(&version));
    say("GPU: %s\nDriver-supported CUDA API: %d.%d\nVRAM: %zu bytes (%.3f GiB)\n", name, version / 1000,
        (version % 1000) / 10, bytes, (double)bytes / 1073741824.0);
    say("Compute capability: %d.%d\nSM count: %d; warp: %d threads\n",
        attribute(CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR),
        attribute(CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR), attribute(CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT),
        attribute(CU_DEVICE_ATTRIBUTE_WARP_SIZE));
    say("Max threads/block: %d; resident threads/SM: %d\n", attribute(CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK),
        attribute(CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_MULTIPROCESSOR));
    say("Max block dimensions: %d x %d x %d\n", attribute(CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_X),
        attribute(CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Y), attribute(CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Z));
    say("Max grid dimensions: %d x %d x %d\n", attribute(CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X),
        attribute(CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Y), attribute(CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Z));
    say("Shared memory/block: %d bytes; registers/block: %d\n",
        attribute(CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK),
        attribute(CU_DEVICE_ATTRIBUTE_MAX_REGISTERS_PER_BLOCK));
    say("VRAM bus width: %d bits; peak clock property: %d kHz\n",
        attribute(CU_DEVICE_ATTRIBUTE_GLOBAL_MEMORY_BUS_WIDTH), attribute(CU_DEVICE_ATTRIBUTE_MEMORY_CLOCK_RATE));
    say("PASS: queried installed NVIDIA driver directly; no Python controller.\n");
    return 0;
}
static void print_file_matches(const char *path, const char *a, const char *b, int limit)
{
    FILE *f = fopen(path, "r");
    char line[2048];
    int count = 0;
    if (!f)
    {
        say("Missing: %s\n", path);
        return;
    }
    while (fgets(line, sizeof(line), f) && count < limit)
    {
        if (strstr(line, a) || (b && strstr(line, b)))
        {
            say("%s", line);
            count++;
        }
    }
    fclose(f);
}
static int step06(void)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    const char *files[] = {"build\\kernels.ptx", "build\\kernels.cubin", "results\\06_sass.txt",
                           "results\\06_resources.txt"};
    int i;
    for (i = 0; i < 4; i++)
    {
        if (!GetFileAttributesExA(files[i], GetFileExInfoStandard, &info))
        {
            say("FAIL missing %s\n", files[i]);
            return 1;
        }
        say("%s: %lu bytes\n", files[i], info.nFileSizeLow);
    }
    say("PTX real exported entries:\n");
    print_file_matches(files[0], ".visible .entry", NULL, 4);
    say("SASS real instructions (excerpt):\n");
    print_file_matches(files[2], "LDG.", "STG.", 4);
    say("ptxas real resource output (excerpt):\n");
    print_file_matches("results\\06_nvcc_cubin.log", "Used ", NULL, 4);
    say("PASS: source -> nvcc PTX/CUBIN -> cuobjdump SASS; complete files retained.\n");
    return 0;
}
static int step07(void)
{
    const int n = 4099;
    size_t bytes = (size_t)n * sizeof(float);
    float *a = mem(bytes), *back = mem(bytes);
    CUdeviceptr d;
    int i, bad = 0;
    float maxe = 0;
    init_device(1);
    for (i = 0; i < n; i++)
        a[i] = (float)(i % 257) * 0.125f;
    CU(cuMemAlloc(&d, bytes));
    CU(cuMemcpyHtoD(d, a, bytes));
    CU(cuCtxSynchronize());
    CU(cuMemcpyDtoH(back, d, bytes));
    CU(cuCtxSynchronize());
    for (i = 0; i < n; i++)
    {
        float e = fabsf(a[i] - back[i]);
        if (e > maxe)
            maxe = e;
        if (a[i] != back[i])
            bad++;
    }
    say("H2D -> synchronize -> D2H -> synchronize\nElements: %d; bytes: %zu; device address: 0x%llx\n", n, bytes,
        (unsigned long long)d);
    say("first: %.3f -> %.3f; last: %.3f -> %.3f\n", a[0], back[0], a[n - 1], back[n - 1]);
    say("Full-element equality: %d/%d; max_abs_error=%.9g\n%s\n", n - bad, n, maxe, bad ? "FAIL" : "PASS");
    CU(cuMemFree(d));
    free(a);
    free(back);
    return bad != 0;
}
static int step08(void)
{
    const int n = 257;
    size_t bytes = (size_t)n * sizeof(float);
    float *A = mem(bytes), *B = mem(bytes), *C = mem(bytes);
    CUdeviceptr da, db, dc;
    CUfunction f;
    int i, bad = 0;
    float maxe = 0;
    void *args[] = {&da, &db, &dc, (void *)&n};
    init_device(1);
    f = function("vec_add");
    for (i = 0; i < n; i++)
    {
        A[i] = (float)i * 0.25f;
        B[i] = (float)(n - i) * 0.5f;
    }
    CU(cuMemAlloc(&da, bytes));
    CU(cuMemAlloc(&db, bytes));
    CU(cuMemAlloc(&dc, bytes));
    CU(cuMemcpyHtoD(da, A, bytes));
    CU(cuMemcpyHtoD(db, B, bytes));
    pause_debug(81, "before cuLaunchKernel(vec_add)");
    CU(cuLaunchKernel(f, (n + 127) / 128, 1, 1, 128, 1, 1, 0, NULL, args, NULL));
    CU(cuCtxSynchronize());
    CU(cuMemcpyDtoH(C, dc, bytes));
    pause_debug(82, "after GPU result copied to C");
    for (i = 0; i < n; i++)
    {
        float e = fabsf(C[i] - (A[i] + B[i]));
        if (e > maxe)
            maxe = e;
        if (e != 0)
            bad++;
    }
    say("Module: build/kernels.cubin; entry: vec_add\nN=%d; grid=(3,1,1); block=(128,1,1)\n", n);
    for (i = 0; i < 4; i++)
        say("C[%d] = %.2f + %.2f = %.2f\n", i, A[i], B[i], C[i]);
    say("Last C[%d]=%.2f; exact equality=%d/%d; max_error=%.9g\n%s\n", n - 1, C[n - 1], n - bad, n, maxe,
        bad ? "FAIL" : "PASS");
    CU(cuMemFree(da));
    CU(cuMemFree(db));
    CU(cuMemFree(dc));
      free(A);
    free(B);
    free(C);
    return bad != 0;
}
static int trace_case(int block, int n)
{
    int count = n + 64, bytes = count * (int)sizeof(int), i, j, bad = 0;
    int *h[4];
    CUdeviceptr d[4];
    CUfunction f = function("index_trace");
    void *args[] = {&d[0], &d[1], &d[2], &d[3], &n};
    for (j = 0; j < 4; j++)
    {
        h[j] = mem(bytes);
        for (i = 0; i < count; i++)
            h[j][i] = (j == 3 && i < n) ? 0 : -777;
        CU(cuMemAlloc(&d[j], bytes));
        CU(cuMemcpyHtoD(d[j], h[j], bytes));
    }
    CU(cuLaunchKernel(f, 2, 1, 1, block, 1, 1, 0, NULL, args, NULL));
    CU(cuCtxSynchronize());
    for (j = 0; j < 4; j++)
        CU(cuMemcpyDtoH(h[j], d[j], bytes));
    for (i = 0; i < n; i++)
        if (h[0][i] != i / block || h[1][i] != i % block || h[2][i] != i || h[3][i] != 1)
            bad++;
    for (i = n; i < count; i++)
        for (j = 0; j < 4; j++)
            if (h[j][i] != -777)
                bad++;
    say("grid=2 block=%d N=%d: unique coverage=%s; 64 guard entries=%s\n", block, n, bad ? "FAIL" : "PASS",
        bad ? "FAIL" : "PASS");
    for (i = 0; i < 3; i++)
        say("  gid=%d blockIdx=%d threadIdx=%d visits=%d\n", h[2][i], h[0][i], h[1][i], h[3][i]);
    for (i = n - 2; i < n; i++)
        say("  gid=%d blockIdx=%d threadIdx=%d visits=%d\n", h[2][i], h[0][i], h[1][i], h[3][i]);
    for (j = 0; j < 4; j++)
    {
        CU(cuMemFree(d[j]));
        free(h[j]);
    }
    return bad != 0;
}
static int step09(void)
{
    int a, b;
    init_device(1);
    a = trace_case(32, 64);
    b = trace_case(64, 70);
    say("Formula: global_id=blockIdx.x*blockDim.x+threadIdx.x\n%s: tail threads are guarded; every valid index visited "
        "exactly once.\n",
        (a || b) ? "FAIL" : "PASS");
    return a || b;
}
static int matrix_case(const char *name, int M, int K, int N, int show)
{
    size_t ab = (size_t)M * K * sizeof(float), bb = (size_t)K * N * sizeof(float), cb = (size_t)M * N * sizeof(float);
    float *A = mem(ab), *B = mem(bb), *C = mem(cb);
    CUdeviceptr da, db, dc;
    int i, j, k, bad = 0;
    double d2 = 0, r2 = 0, maxerr = 0, maxref = 0;
    CUfunction f = function(name);
    for (i = 0; i < M * K; i++)
        A[i] = show ? (float)(i + 1) : random_value();
    for (i = 0; i < K * N; i++)
        B[i] = show ? (float)(i % N + 1) : random_value();
    CU(cuMemAlloc(&da, ab));
    CU(cuMemAlloc(&db, bb));
    CU(cuMemAlloc(&dc, cb));
    CU(cuMemcpyHtoD(da, A, ab));
    CU(cuMemcpyHtoD(db, B, bb));
    launch_mm(f, da, db, dc, M, K, N);
    CU(cuCtxSynchronize());
    CU(cuMemcpyDtoH(C, dc, cb));
    for (i = 0; i < M; i++)
        for (j = 0; j < N; j++)
        {
            double ref = 0, d;
            for (k = 0; k < K; k++)
                ref += (double)A[i * K + k] * B[k * N + j];
            d = (double)C[i * N + j] - ref;
            d2 += d * d;
            r2 += ref * ref;
            if (fabs(d) > maxerr)
                maxerr = fabs(d);
            if (fabs(ref) > maxref)
                maxref = fabs(ref);
        }
    bad = !(sqrt(d2 / (r2 > 0 ? r2 : 1)) <= 1e-5 && maxerr / (maxref > 0 ? maxref : 1) <= 1e-4);
    say("%s: [%d,%d] @ [%d,%d] -> [%d,%d]\n", name, M, K, K, N, M, N);
    say("  max_abs=%.6g; rel_L2=%.6g; CPU FP64 reference: %s\n", maxerr, sqrt(d2 / (r2 > 0 ? r2 : 1)),
        bad ? "FAIL" : "PASS");
    if (show)
        for (i = 0; i < M; i++)
        {
            say("  C[%d]:", i);
            for (j = 0; j < N; j++)
                say(" %7.2f", C[i * N + j]);
            say("\n");
        }
    CU(cuMemFree(da));
    CU(cuMemFree(db));
    CU(cuMemFree(dc));
    free(A);
    free(B);
    free(C);
    return bad;
}
static int step10(void)
{
    int bad;
    init_device(1);
    bad = matrix_case("matmul_naive", 4, 4, 4, 1);
    bad |= matrix_case("matmul_naive", 3, 5, 2, 0);
    bad |= matrix_case("matmul_naive", 17, 19, 13, 0);
    say("%s: all output elements checked, including non-square/tail dimensions.\n", bad ? "FAIL" : "PASS");
    return bad;
}
static int step11(void)
{
    int bad = 0, i;
    int sizes[3][3] = {{4, 4, 4}, {128, 128, 128}, {257, 259, 263}};
    init_device(1);
    say("Tile=16x16; two shared 16x16 FP32 arrays (2048 bytes).\nOut-of-range loads are zero; all threads reach both "
        "barriers.\n");
    for (i = 0; i < 3; i++)
    {
        rng = 20261016u + (unsigned)i;
        bad |= matrix_case("matmul_naive", sizes[i][0], sizes[i][1], sizes[i][2], 0);
        rng = 20261016u + (unsigned)i;
        bad |= matrix_case("matmul_tiled", sizes[i][0], sizes[i][1], sizes[i][2], 0);
    }
    say("%s: six full-output checks; actual ptxas usage in 06_nvcc_cubin.log.\n", bad ? "FAIL" : "PASS");
    return bad;
}
static int smi_to(const char *arguments, const char *path)
{
    char command[4096];
    snprintf(command, sizeof(command), "nvidia-smi %s > \"%s\" 2>&1", arguments, path);
    return system(command);
}
static int step12(void)
{
    FILE *f;
    char line[2048], header[2048];
    int rc, gmax, wmax, gcur, wcur;
    double theoretical;
    init_device(0);
    rc = smi_to("-i 0 --query-gpu=pcie.link.gen.max,pcie.link.width.max,pcie.link.gen.current,pcie.link.width.current "
                "--format=csv,noheader,nounits",
                "results\\12_pcie_raw.txt");
    f = fopen("results\\12_pcie_raw.txt", "r");
    if (!f)
        return 1;
    line[0] = 0;
    fgets(line, sizeof(line), f);
    fclose(f);
    say("nvidia-smi return code: %d\nPCIe raw: %s", rc, line);
    if (rc || sscanf(line, "%d , %d , %d , %d", &gmax, &wmax, &gcur, &wcur) != 4)
        return 1;
    say("Max link: Gen%d x%d; current sample: Gen%d x%d\n", gmax, wmax, gcur, wcur);
    say("Driver VRAM interface width: %d bits (not PCIe lanes).\n",
        attribute(CU_DEVICE_ATTRIBUTE_GLOBAL_MEMORY_BUS_WIDTH));
    say("VRAM type: CUDA Driver API does not provide the memory technology name.\n");
    if (gmax >= 3 && gmax <= 5)
    {
        double gt = (gmax == 3 ? 8.0 : gmax == 4 ? 16.0 : 32.0);
        theoretical = gt * wmax * (128.0 / 130.0) / 8.0;
        say("Theoretical one-way PCIe: %.0f GT/s * %d * 128/130 /8 = %.6f GB/s\n", gt, wmax, theoretical);
        say("Theoretical line capacity; protocol overhead excluded; step13 measures payload.\n");
    }
    snprintf(header, sizeof(header), "returncode=%d; max=Gen%d x%d; current=Gen%d x%d\n", rc, gmax, wmax, gcur, wcur);
    f = fopen("results\\12_link_summary.txt", "w");
    if (f)
    {
        fputs(header, f);
        fclose(f);
    }
    say("PASS: PCIe lanes, VRAM bit width, and payload bandwidth kept distinct.\n");
    return 0;
}
static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double median(double *x, int n)
{
    double y[64];
    memcpy(y, x, n * sizeof(double));
    qsort(y, n, sizeof(double), cmp_double);
    return (y[(n - 1) / 2] + y[n / 2]) * 0.5;
}
static int step13(void)
{
    size_t sizes[] = {4096, 65536, 1048576, 16777216, 67108864};
    const char *size_names[] = {"4KiB", "64KiB", "1MiB", "16MiB", "64MiB"};
    double rates[5][5] = {{0}};
    int s, mode, dir, r, bad = 0;
    FILE *raw, *summary;
    init_device(1);
    raw = fopen("results\\13_transfer_raw.csv", "w");
    summary = fopen("results\\13_transfer_summary.csv", "w");
    if (!raw || !summary)
        return 1;
    fprintf(raw, "host_memory,payload_bytes,direction,repeat,elapsed_ms\n");
    fprintf(summary, "host_memory,payload_bytes,direction,median_ms,payload_GB_s,traffic_GB_s,full_validation\n");
    say("Transfer: QPC wall time; synchronous Driver copy + cuCtxSynchronize.\n5 warmups +30 repetitions; "
        "allocation/initialization/validation excluded.\nPayload GB/s=bytes/time/1e9; D2D traffic GB/s=2*payload "
        "GB/s.\n");
    for (s = 0; s < 5; s++)
        for (mode = 0; mode < 2; mode++)
        {
            size_t bytes = sizes[s], i;
            unsigned char *host, *back;
            CUdeviceptr d1, d2;
            const char *kind = mode ? "pinned" : "pageable";
            if (mode)
            {
                CU(cuMemAllocHost((void **)&host, bytes));
                CU(cuMemAllocHost((void **)&back, bytes));
            }
            else
            {
                host = mem(bytes);
                back = mem(bytes);
            }
            for (i = 0; i < bytes; i++)
                host[i] = (unsigned char)((i * 17 + 3) % 251);
            CU(cuMemAlloc(&d1, bytes));
            CU(cuMemAlloc(&d2, bytes));
            CU(cuMemcpyHtoD(d1, host, bytes));
            for (dir = 0; dir < 3; dir++)
            {
                double ms[30], med;
                int fail = 0;
                const char *direction = dir == 0 ? "H2D" : dir == 1 ? "D2H" : "D2D";
                if (dir == 2 && mode == 1)
                    continue;
                for (r = -5; r < 30; r++)
                {
                    CUresult copy_result, sync_result;
                    double elapsed_ms;
                    double start = seconds();
                    if (dir == 0)
                    {
                        copy_result = cuMemcpyHtoD(d1, host, bytes);
                    }
                    else if (dir == 1)
                    {
                        copy_result = cuMemcpyDtoH(back, d1, bytes);
                    }
                    else
                    {
                        copy_result = cuMemcpyDtoD(d2, d1, bytes);
                    }
                    sync_result = cuCtxSynchronize();
                    elapsed_ms = (seconds() - start) * 1000.0;
                    check(copy_result, "timed transfer (see direction CSV)", __LINE__);
                    check(sync_result, "timed cuCtxSynchronize", __LINE__);
                    if (r >= 0)
                    {
                        ms[r] = elapsed_ms;
                        fprintf(raw, "%s,%zu,%s,%d,%.9f\n", dir == 2 ? "device" : kind, bytes, direction, r, ms[r]);
                    }
                }
                CU(cuMemcpyDtoH(back, dir == 2 ? d2 : d1, bytes));
                for (i = 0; i < bytes; i++)
                    if (host[i] != back[i])
                    {
                        fail = 1;
                        break;
                    }
                bad |= fail;
                med = median(ms, 30);
                rates[s][dir == 2 ? 4 : mode * 2 + dir] = bytes / (med * 1e6);
                fprintf(summary, "%s,%zu,%s,%.9f,%.9f,%.9f,%s\n", dir == 2 ? "device" : kind, bytes, direction, med,
                        bytes / (med * 1e6), (dir == 2 ? 2 : 1) * bytes / (med * 1e6), fail ? "FAIL" : "PASS");
            }
            CU(cuMemFree(d1));
            CU(cuMemFree(d2));
            if (mode)
            {
                CU(cuMemFreeHost(host));
                CU(cuMemFreeHost(back));
            }
            else
            {
                free(host);
                free(back);
            }
        }
    fclose(raw);
    fclose(summary);
    say("Payload GB/s: size |page H2D|page D2H|pin H2D|pin D2H| D2D\n");
    for (s = 0; s < 5; s++)
        say("%6s %9.3f %9.3f %8.3f %8.3f %8.3f\n", size_names[s], rates[s][0], rates[s][1], rates[s][2], rates[s][3],
            rates[s][4]);
    /* Separate matrix kernel vs whole round trip: same CPU FP32 inputs, no profiler. */
    {
        int M = 512, K = 512, N = 512, i;
        size_t bytes = (size_t)M * K * sizeof(float);
        float *a = mem(bytes), *b = mem(bytes), *c = mem(bytes);
        CUdeviceptr da, db, dc;
        CUfunction fn = function("matmul_tiled");
        CUevent begin, end;
        double kms[30], ems[30];
        float elapsed;
        FILE *timing = fopen("results\\13_matrix_raw.csv", "w");
        if (!timing)
            return 1;
        for (i = 0; i < M * K; i++)
        {
            a[i] = (float)(i % 13) / 13.0f;
            b[i] = (float)(i % 7) / 7.0f;
        }
        CU(cuMemAlloc(&da, bytes));
        CU(cuMemAlloc(&db, bytes));
        CU(cuMemAlloc(&dc, bytes));
        CU(cuMemcpyHtoD(da, a, bytes));
        CU(cuMemcpyHtoD(db, b, bytes));
        CU(cuEventCreate(&begin, 0));
        CU(cuEventCreate(&end, 0));
        fprintf(timing, "repeat,gpu_kernel_event_ms,end_to_end_wall_ms\n");
        for (i = -5; i < 30; i++)
        {
            double start, elapsed_wall;
            CUresult status[6];
            int j;
            status[0] = cuEventRecord(begin, NULL);
            status[1] = launch_mm_raw(fn, da, db, dc, M, K, N);
            status[2] = cuEventRecord(end, NULL);
            status[3] = cuEventSynchronize(end);
            for (j = 0; j < 4; j++)
                check(status[j], "CUDA-event timing API sequence", __LINE__);
            CU(cuEventElapsedTime(&elapsed, begin, end));
            if (i >= 0)
                kms[i] = elapsed;
            start = seconds();
            status[0] = cuMemcpyHtoD(da, a, bytes);
            status[1] = cuMemcpyHtoD(db, b, bytes);
            status[2] = launch_mm_raw(fn, da, db, dc, M, K, N);
            status[3] = cuCtxSynchronize();
            status[4] = cuMemcpyDtoH(c, dc, bytes);
            status[5] = cuCtxSynchronize();
            elapsed_wall = (seconds() - start) * 1000;
            for (j = 0; j < 6; j++)
                check(status[j], "E2E timing API sequence", __LINE__);
            if (i >= 0)
            {
                ems[i] = elapsed_wall;
                fprintf(timing, "%d,%.9f,%.9f\n", i, kms[i], ems[i]);
            }
        }
        say("512x512 tile: GPU event kernel=%.4fms; end-to-end wall=%.4fms\n", median(kms, 30), median(ems, 30));
        say("Matrix E2E=two H2D+kernel+sync+D2H; buffers preallocated.\n");
        {
            int row, col, k;
            double d2 = 0, r2 = 0, maxerr = 0;
            for (row = 0; row < M; row++)
                for (col = 0; col < N; col++)
                {
                    double ref = 0, d;
                    for (k = 0; k < K; k++)
                        ref += (double)a[row * K + k] * b[k * N + col];
                    d = (double)c[row * N + col] - ref;
                    d2 += d * d;
                    r2 += ref * ref;
                    if (fabs(d) > maxerr)
                        maxerr = fabs(d);
                }
            if (sqrt(d2 / r2) > 1e-5)
                bad = 1;
            say("Matrix full-output FP64 check: rel_L2=%.6g max_abs=%.6g %s\n", sqrt(d2 / r2), maxerr,
                sqrt(d2 / r2) <= 1e-5 ? "PASS" : "FAIL");
        }
        fclose(timing);
        CU(cuEventDestroy(begin));
        CU(cuEventDestroy(end));
        CU(cuMemFree(da));
        CU(cuMemFree(db));
        CU(cuMemFree(dc));
        free(a);
        free(b);
        free(c);
    }
    say("%s: all copy bytes validated; 25 transfer groups x30 samples saved.\n", bad ? "FAIL" : "PASS");
    return bad;
}
typedef struct
{
    volatile LONG stop, phase;
    double origin;
    FILE *csv, *raw;
} Telemetry;
static DWORD WINAPI sample_thread(LPVOID arg)
{
    Telemetry *t = (Telemetry *)arg;
    double next = t->origin;
    char command[2048], buffer[2048];
    const char *labels[] = {"idle", "load", "cooldown"};
    while (!InterlockedCompareExchange(&t->stop, 0, 0))
    {
        double started;
        LONG phase;
        FILE *p;
        int rc;
        double remaining = next - seconds();
        if (remaining > 0)
            Sleep((DWORD)(remaining * 1000));
        if (InterlockedCompareExchange(&t->stop, 0, 0))
            break;
        started = seconds();
        phase = InterlockedCompareExchange(&t->phase, 0, 0);
        snprintf(command, sizeof(command),
                 "nvidia-smi -i 0 "
                 "--query-gpu=timestamp,pstate,clocks.current.graphics,clocks.current.sm,clocks.current.memory,"
                 "temperature.gpu,power.draw,utilization.gpu --format=csv,noheader,nounits 2>&1");
        p = _popen(command, "r");
        buffer[0] = 0;
        if (p)
        {
            fgets(buffer, sizeof(buffer), p);
            rc = _pclose(p);
        }
        else
        {
            strcpy(buffer, "QUERY_FAILED\n");
            rc = -1;
        }
        fprintf(t->raw, "phase=%s elapsed=%.6f returncode=%d command=%s\n%s", labels[phase], started - t->origin, rc,
                command, buffer);
        fflush(t->raw);
        fprintf(t->csv, "%s,%.6f,%d,%s", labels[phase], started - t->origin, rc, buffer);
        fflush(t->csv);
        next += 1.0;
        if (next < seconds())
            next = seconds();
    }
    return 0;
}
static int step14(void)
{
    Telemetry t = {0};
    HANDLE worker;
    CUdeviceptr da, db, dc;
    CUfunction fn;
    int M = 2048, K = 2048, N = 2048, i, batches = 0, bad = 0;
    size_t bytes = (size_t)M * K * sizeof(float);
    float *a = mem(bytes), *b = mem(bytes), *c = mem(bytes);
    double start, end;
    FILE *voltage;
    char line[1024];
    int voltage_rc;
    init_device(1);
    fn = function("matmul_tiled");
    for (i = 0; i < M * K; i++)
    {
        a[i] = random_value();
        b[i] = random_value();
    }
    CU(cuMemAlloc(&da, bytes));
    CU(cuMemAlloc(&db, bytes));
    CU(cuMemAlloc(&dc, bytes));
    CU(cuMemcpyHtoD(da, a, bytes));
    CU(cuMemcpyHtoD(db, b, bytes));
    for (i = 0; i < 3; i++)
        launch_mm(fn, da, db, dc, M, K, N);
    CU(cuCtxSynchronize());
    voltage_rc =
        smi_to("-i 0 --query-gpu=voltage.graphics --format=csv,noheader,nounits", "results\\14_voltage_raw.txt");
    say("Voltage query returncode=%d; raw feedback saved (no invented voltage).\n", voltage_rc);
    voltage = fopen("results\\14_voltage_raw.txt", "r");
    if (voltage)
    {
        if (fgets(line, sizeof(line), voltage))
            say("%s", line);
        fclose(voltage);
    }
    t.csv = fopen("results\\14_telemetry.csv", "w");
    t.raw = fopen("results\\14_telemetry_raw.log", "w");
    if (!t.csv || !t.raw)
        return 1;
    fprintf(t.csv, "phase,elapsed_s,returncode,timestamp,pstate,graphics_MHz,SM_MHz,memory_MHz,temperature_C,power_W,"
                   "utilization_percent\n");
    t.origin = seconds();
    worker = CreateThread(NULL, 0, sample_thread, &t, 0, NULL);
    if (!worker)
        return 1;
    say("Idle 5s: this program submits no GPU work.\n");
    Sleep(5000);
    InterlockedExchange(&t.phase, 1);
    start = seconds();
    say("Load ~12s: preallocated 2048x2048 tile GEMM, batches of 10.\n");
    while (seconds() - start < 12)
    {
        for (i = 0; i < 10; i++)
            launch_mm(fn, da, db, dc, M, K, N);
        CU(cuCtxSynchronize());
        batches++;
    }
    end = seconds();
    InterlockedExchange(&t.phase, 2);
    say("Cooldown 5s: no new GPU work from this program.\n");
    Sleep(5000);
    InterlockedExchange(&t.stop, 1);
    WaitForSingleObject(worker, 10000);
    CloseHandle(worker);
    fclose(t.csv);
    fclose(t.raw);
    CU(cuMemcpyDtoH(c, dc, bytes));
    {
        double maxe = 0, maxscaled = 0;
        int probe;
        for (probe = 0; probe < 32; probe++)
        {
            int row = (probe * 67) % M, col = (probe * 113) % N, k;
            double ref = 0, error;
            for (k = 0; k < K; k++)
                ref += (double)a[row * K + k] * b[k * N + col];
            error = fabs((double)c[row * N + col] - ref);
            if (error > maxe)
                maxe = error;
            if (error / (fabs(ref) + 1e-5) > maxscaled)
                maxscaled = error / (fabs(ref) + 1e-5);
        }
        bad = !(maxe < 0.001);
        say("Load completed %d GEMMs in %.3fs; sampled 32 output elements max_abs_error=%.6g %s\n", batches * 10,
            end - start, maxe, bad ? "FAIL" : "PASS");
    }
    say("Actual telemetry: results/14_telemetry.csv (1s target interval).\nCurrent MHz/mV cannot measure voltage "
        "waveform or regulator switching frequency.\n%s: read-only observations; no clock/voltage/power setting "
        "changed.\n",
        bad ? "FAIL" : "PASS");
    CU(cuMemFree(da));
    CU(cuMemFree(db));
    CU(cuMemFree(dc));
    free(a);
    free(b);
    free(c);
    return bad;
}
int main(int argc, char **argv)
{
    int step, result = 1, i;
    char file[256], ts[64];
    QueryPerformanceFrequency(&qpf);
    if (argc < 2)
    {
        printf("Usage: bin\\gpu_labs.exe 05..14 [--debug]\n");
        return 1;
    }
    step = atoi(argv[1]);
    active_step = step;
    for (i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--debug"))
            debug_mode = 1;
    for (i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--show-log"))
        {
            FILE *saved;
            char line[4096];
            snprintf(file, sizeof(file), "results\\%02d_run.log", step);
            saved = fopen(file, "r");
            if (!saved)
            {
                say("No saved actual run log: %s\n", file);
                return 1;
            }
            say("SAVED ACTUAL RUN LOG: %s (not re-executed for this view)\n", file);
            while (fgets(line, sizeof(line), saved))
                say("%s", line);
            fclose(saved);
            return 0;
        }
    CreateDirectoryA("results", NULL);
    snprintf(file, sizeof(file), "results\\%02d_run.log", step);
    runlog = fopen(file, "w");
    snprintf(file, sizeof(file), "results\\%02d_api.log", step);
    apilog = fopen(file, "w");
    stamp(ts, sizeof(ts));
    say("GPU foundation step %02d | actual run %s | C + CUDA Driver API\n", step, ts);
    switch (step)
    {
    case 5:
        result = step05();
        break;
    case 6:
        result = step06();
        break;
    case 7:
        result = step07();
        break;
    case 8:
        result = step08();
        break;
    case 9:
        result = step09();
        break;
    case 10:
        result = step10();
        break;
    case 11:
        result = step11();
        break;
    case 12:
        result = step12();
        break;
    case 13:
        result = step13();
        break;
    case 14:
        result = step14();
        break;
    default:
        say("Invalid GPU step. Choose 05..14.\n");
    }
    finish();
    write_session(step, result);
    say("Exit status: %d; API trace: results/%02d_api.log\n", result, step);
    if (apilog)
        fclose(apilog);
    if (runlog)
        fclose(runlog);
    return result;
}
