/* Host program is ISO C plus Win32 extensions: built with cl /TC /O2. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cuda.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

static LARGE_INTEGER frequency;
static volatile LONG cancelled = 0;
static FILE *csv = NULL;
static const char *mode = "";
static const char *stop_file = "STOP";
static double seconds_requested = 10.0;
static unsigned int mib = 128;
static CUcontext ctx = NULL;
static CUmodule module = NULL;
static CUstream stream = NULL;
static CUevent ev0 = NULL, ev1 = NULL;
static CUdeviceptr da = 0, db = 0, dc = 0;
static float *ha = NULL, *hb = NULL, *hc = NULL;
static void *pinned = NULL;
static int api_errors = 0;

static double now_s(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)frequency.QuadPart;
}
static BOOL WINAPI control_handler(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        InterlockedExchange(&cancelled, 1); return TRUE;
    }
    return FALSE;
}
static int stop_requested(void)
{
    return cancelled || GetFileAttributesA(stop_file) != INVALID_FILE_ATTRIBUTES;
}
static int ck(CUresult r, const char *call, int line)
{
    const char *name = "unknown", *text = "unknown";
    if (r == CUDA_SUCCESS) return 1;
    cuGetErrorName(r, &name); cuGetErrorString(r, &text);
    fprintf(stderr, "CUDA_ERROR line=%d call=%s code=%d name=%s text=%s\n",
            line, call, (int)r, name, text);
    ++api_errors; return 0;
}
#define CU(x) do { if (!ck((x), #x, __LINE__)) goto fail; } while (0)
static void row(const char *phase, double elapsed, unsigned long long iterations,
                double gpu_ms, double metric, const char *units,
                unsigned long long errors, unsigned long long checked)
{
    if (csv) {
        fprintf(csv, "%s,%s,%.9f,%llu,%.6f,%.6f,%s,%llu,%llu\n", mode, phase,
                elapsed, iterations, gpu_ms, metric, units, errors, checked);
        fflush(csv);
    }
}
static int load_gpu(int need_module)
{
    int count = 0, major = 0, minor = 0, sm = 0, clock_khz = 0, driver = 0;
    CUdevice dev; char name[256]; size_t total = 0; SYSTEM_POWER_STATUS power;
    if (GetSystemPowerStatus(&power))
        printf("ACLineStatus=%u (1=AC 0=battery 255=unknown)\n", (unsigned int)power.ACLineStatus);
    else printf("ACLineStatus=unavailable Win32_error=%lu\n", GetLastError());
    CU(cuInit(0)); CU(cuDeviceGetCount(&count));
    if (count < 1) { fprintf(stderr, "No CUDA devices\n"); goto fail; }
    CU(cuDeviceGet(&dev, 0)); CU(cuDeviceGetName(name, sizeof(name), dev));
    CU(cuDriverGetVersion(&driver)); CU(cuDeviceTotalMem(&total, dev));
    CU(cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev));
    CU(cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev));
    CU(cuDeviceGetAttribute(&sm, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, dev));
    CU(cuDeviceGetAttribute(&clock_khz, CU_DEVICE_ATTRIBUTE_CLOCK_RATE, dev));
    printf("GPU: %s\nDevice_count=%d compute_capability=%d.%d SMs=%d\n",
           name, count, major, minor, sm);
    printf("Global_memory_MiB=%.0f driver_API_version=%d nominal_clock_kHz=%d\n",
           total / 1048576.0, driver, clock_khz);
    if (!need_module) return 1;
    CU(cuCtxCreate(&ctx, 0, dev));
    CU(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING));
    CU(cuEventCreate(&ev0, CU_EVENT_DEFAULT)); CU(cuEventCreate(&ev1, CU_EVENT_DEFAULT));
    CU(cuModuleLoad(&module, "build/kernels.cubin"));
    return 1;
fail: return 0;
}
static void clean_gpu(void)
{
    if (ctx) ck(cuCtxSynchronize(), "cleanup cuCtxSynchronize", __LINE__);
    if (da) ck(cuMemFree(da), "cleanup cuMemFree(da)", __LINE__);
    if (db) ck(cuMemFree(db), "cleanup cuMemFree(db)", __LINE__);
    if (dc) ck(cuMemFree(dc), "cleanup cuMemFree(dc)", __LINE__);
    if (pinned) ck(cuMemFreeHost(pinned), "cleanup cuMemFreeHost", __LINE__);
    if (ev0) ck(cuEventDestroy(ev0), "cleanup cuEventDestroy(ev0)", __LINE__);
    if (ev1) ck(cuEventDestroy(ev1), "cleanup cuEventDestroy(ev1)", __LINE__);
    if (stream) ck(cuStreamDestroy(stream), "cleanup cuStreamDestroy", __LINE__);
    if (module) ck(cuModuleUnload(module), "cleanup cuModuleUnload", __LINE__);
    if (ctx) ck(cuCtxDestroy(ctx), "cleanup cuCtxDestroy", __LINE__);
    free(ha); free(hb); free(hc);
}
static int allocate_arrays(unsigned int n)
{
    size_t bytes = (size_t)n * sizeof(float); unsigned int i;
    ha = (float *)malloc(bytes); hb = (float *)malloc(bytes); hc = (float *)malloc(bytes);
    if (!ha || !hb || !hc) { fprintf(stderr, "Host malloc failed\n"); goto fail; }
    for (i = 0; i < n; ++i) { ha[i] = (float)(i % 1024u); hb[i] = (float)(i % 257u) * .25f; }
    CU(cuMemAlloc(&da, bytes)); CU(cuMemAlloc(&db, bytes)); CU(cuMemAlloc(&dc, bytes));
    CU(cuMemcpyHtoD(da, ha, bytes)); CU(cuMemcpyHtoD(db, hb, bytes));
    /* Pageable HtoD may return before DMA ends; our non-blocking stream must wait. */
    CU(cuCtxSynchronize());
    return 1;
fail: return 0;
}
static unsigned long long check_array(unsigned int n, int triad)
{
    unsigned int i; unsigned long long errors = 0;
    for (i = 0; i < n; ++i) {
        float expected = ha[i] + (triad ? 2.0f : 1.0f) * hb[i];
        if (hc[i] != expected) ++errors;
    }
    return errors;
}
static int vector_test(void)
{
    CUfunction fn; unsigned int n = 1048576u;
    void *args[] = { &da, &db, &dc, &n }; float ms; unsigned long long errors;
    double start;
    if (!load_gpu(1) || !allocate_arrays(n)) goto fail;
    CU(cuModuleGetFunction(&fn, module, "vector_add")); start = now_s();
    CU(cuEventRecord(ev0, stream));
    CU(cuLaunchKernel(fn, (n + 255u) / 256u, 1, 1, 256, 1, 1, 0, stream, args, NULL));
    CU(cuEventRecord(ev1, stream)); CU(cuEventSynchronize(ev1));
    CU(cuEventElapsedTime(&ms, ev0, ev1)); CU(cuMemcpyDtoH(hc, dc, (size_t)n * 4));
    errors = check_array(n, 0);
    printf("VECTOR n=%u kernel_ms=%.6f checked=%u mismatches=%llu\n", n, ms, n, errors);
    printf("sample: C[0]=%.2f C[1]=%.2f C[last]=%.2f\n", hc[0], hc[1], hc[n - 1]);
    row("vector_add", now_s() - start, 1, ms, 0, "none", errors, n);
    return errors == 0;
fail: return 0;
}

typedef struct Worker {
    const unsigned int *values; size_t first, last;
    HANDLE gate; unsigned long long sum; volatile LONG *completed;
} Worker;
static DWORD WINAPI worker_main(LPVOID parameter)
{
    Worker *w = (Worker *)parameter; size_t i; unsigned long long sum = 0;
    if (WaitForSingleObject(w->gate, INFINITE) != WAIT_OBJECT_0) return 1;
    for (i = w->first; i < w->last; ++i) sum += w->values[i];
    w->sum = sum; InterlockedIncrement(w->completed); return 0;
}
static int cpu_test(void)
{
    const size_t n = 4194304; const DWORD threads = 4;
    unsigned int *values = NULL; HANDLE gate = NULL, handles[4] = {0};
    Worker workers[4]; SYSTEM_INFO si; DWORD i, created = 0, code = 1;
    volatile LONG completed = 0; unsigned long long actual = 0, expected;
    double start, elapsed; int ok = 0;
    GetSystemInfo(&si);
    printf("CPU logical_processors=%lu worker_threads=%lu\n", si.dwNumberOfProcessors, threads);
    values = (unsigned int *)VirtualAlloc(NULL, n * sizeof(unsigned int),
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!values) { fprintf(stderr, "VirtualAlloc failed=%lu\n", GetLastError()); goto done; }
    for (i = 0; i < n; ++i) values[i] = i % 1024u;
    gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!gate) { fprintf(stderr, "CreateEvent failed=%lu\n", GetLastError()); goto done; }
    for (i = 0; i < threads; ++i) {
        workers[i].values = values; workers[i].first = n * i / threads;
        workers[i].last = n * (i + 1) / threads; workers[i].gate = gate;
        workers[i].sum = 0; workers[i].completed = &completed;
        handles[i] = CreateThread(NULL, 0, worker_main, &workers[i], 0, NULL);
        if (!handles[i]) { fprintf(stderr, "CreateThread failed=%lu\n", GetLastError()); goto done; }
        ++created;
    }
    start = now_s();
    if (!SetEvent(gate)) goto done;
    if (WaitForMultipleObjects(threads, handles, TRUE, INFINITE) != WAIT_OBJECT_0) goto done;
    elapsed = now_s() - start;
    for (i = 0; i < threads; ++i) {
        if (!GetExitCodeThread(handles[i], &code) || code != 0) goto done;
        actual += workers[i].sum;
    }
    expected = (unsigned long long)(n / 1024) * 1023u * 1024u / 2u;
    printf("CPU VirtualAlloc_bytes=%llu completed=%ld sum=%llu expected=%llu\n",
           (unsigned long long)(n * sizeof(unsigned int)), completed, actual, expected);
    printf("CPU thread_work_and_wait_ms=%.6f mismatches=%d\n", elapsed * 1000.0, actual != expected);
    row("four_thread_sum", elapsed, threads, 0, elapsed * 1000.0, "ms", actual != expected, n);
    ok = actual == expected && completed == (LONG)threads;
done:
    /* On partial startup, release the gate before waiting for created workers. */
    if (gate) SetEvent(gate);
    if (created) WaitForMultipleObjects(created, handles, TRUE, INFINITE);
    for (i = 0; i < created; ++i) CloseHandle(handles[i]);
    if (gate) CloseHandle(gate);
    if (values) VirtualFree(values, 0, MEM_RELEASE);
    return ok;
}
static int transfer_test(void)
{
    size_t bytes = (size_t)mib * 1048576; size_t i; int kind, direction, r;
    const int repeats = 12; unsigned long long errors = 0, checked = 0;
    unsigned char *normal = NULL, *download = NULL; double start, elapsed, rate;
    const char *kind_name[] = { "pageable", "pinned" };
    const char *direction_name[] = { "HtoD", "DtoH", "DtoD" };
    if (!load_gpu(1)) goto fail;
    normal = (unsigned char *)malloc(bytes); download = (unsigned char *)malloc(bytes);
    if (!normal || !download) goto fail;
    for (i = 0; i < bytes; ++i) normal[i] = (unsigned char)((i * 17u + 3u) & 255u);
    CU(cuMemHostAlloc(&pinned, bytes, CU_MEMHOSTALLOC_PORTABLE));
    memcpy(pinned, normal, bytes); CU(cuMemAlloc(&da, bytes)); CU(cuMemAlloc(&db, bytes));
    printf("TRANSFER size_MiB=%u repeats=%d timing=QPC_sync_calls_plus_CtxSynchronize\n", mib, repeats);
    for (kind = 0; kind < 2; ++kind) {
        void *host = kind ? pinned : normal;
        for (direction = 0; direction < 2; ++direction) {
            char phase[40];
            CU(cuMemcpyHtoD(da, normal, bytes)); CU(cuCtxSynchronize());
            start = now_s();
            for (r = 0; r < repeats; ++r) {
                if (direction == 0) { CU(cuMemcpyHtoD(da, host, bytes)); }
                else { CU(cuMemcpyDtoH(host, da, bytes)); }
            }
            CU(cuCtxSynchronize()); elapsed = now_s() - start;
            CU(cuMemcpyDtoH(download, da, bytes));
            for (i = 0; i < bytes; ++i) if (download[i] != (unsigned char)((i * 17u + 3u) & 255u)) ++errors;
            if (direction == 1)
                for (i = 0; i < bytes; ++i) if (((unsigned char *)host)[i] != (unsigned char)((i * 17u + 3u) & 255u)) ++errors;
            checked += bytes * (direction == 1 ? 2 : 1);
            rate = (double)bytes * repeats / elapsed / 1e9;
            sprintf(phase, "%s_%s", kind_name[kind], direction_name[direction]);
            printf("%-17s elapsed_ms=%.3f effective_GBps=%.3f cumulative_mismatches=%llu\n", phase, elapsed * 1000, rate, errors);
            row(phase, elapsed, repeats, 0, rate, "GB/s", errors, checked);
        }
    }
    start = now_s();
    for (r = 0; r < repeats; ++r) CU(cuMemcpyDtoD(db, da, bytes));
    CU(cuCtxSynchronize()); elapsed = now_s() - start;
    CU(cuMemcpyDtoH(download, db, bytes));
    for (i = 0; i < bytes; ++i) if (download[i] != (unsigned char)((i * 17u + 3u) & 255u)) ++errors;
    checked += bytes; rate = (double)bytes * repeats / elapsed / 1e9;
    printf("DtoD              elapsed_ms=%.3f copied_payload_GBps=%.3f mismatches=%llu\n", elapsed * 1000, rate, errors);
    printf("DtoD counts copied payload once; a read+write traffic convention would be twice this number.\n");
    row("DtoD_payload", elapsed, repeats, 0, rate, "GB/s", errors, checked);
    free(normal); free(download); return errors == 0;
fail: free(normal); free(download); return 0;
}

static float expected_compute[4][64][8];
static void make_compute_reference(unsigned int loops)
{
    unsigned int seed, i, a, k;
    for (seed = 0; seed < 4; ++seed)
        for (i = 0; i < 64; ++i)
            for (a = 0; a < 8; ++a) {
                float x = (float)seed * .0625f + (float)(i + a * 64 + 1) / 1024.0f;
                for (k = 0; k < loops; ++k) x = fmaf(x, 1.00000011920928955078125f, .000001f);
                expected_compute[seed][i][a] = x;
            }
}
static unsigned long long check_compute(unsigned int n, unsigned int seed_index)
{
    unsigned int i, a; unsigned long long errors = 0;
    for (i = 0; i < n; ++i)
        for (a = 0; a < 8; ++a) {
            float v = hc[(size_t)i * 8 + a], expected = expected_compute[seed_index][i & 63u][a];
            if (!isfinite(v) || v != expected) ++errors;
        }
    return errors;
}
static int stress_test(int compute)
{
    CUfunction fn; unsigned int n, loops = 512, seed_index = 0;
    size_t bytes; void *args[4]; float seed = 0, ms = 0;
    double start, elapsed, last_check = 0, last_print = 0, gpu_ms = 0, metric, work;
    float min_ms = 1e30f, max_ms = 0;
    unsigned int warmup;
    unsigned long long iterations = 0, checks = 0, checked = 0, errors = 0;
    if (!load_gpu(1)) goto fail;
    if (compute) {
        n = 262144; bytes = (size_t)n * 8 * sizeof(float);
        hc = (float *)malloc(bytes); if (!hc) goto fail;
        CU(cuMemAlloc(&dc, bytes)); CU(cuModuleGetFunction(&fn, module, "fp32_fma"));
        make_compute_reference(loops);
        args[0] = &dc; args[1] = &n; args[2] = &loops; args[3] = &seed;
        work = (double)n * 8.0 * loops * 2.0;
        printf("COMPUTE threads=%u accumulators=8 FMA_loops=%u FLOP_per_launch=%.0f output_MiB=%.1f\n",
               n, loops, work, bytes / 1048576.0);
        printf("Reference: CPU fmaf, 4 seeds, 64 input patterns; exact full-output comparison every ~1s.\n");
        printf("Comparison tolerance=0 (exact finite float values), any unequal value stops.\n");
    } else {
        n = mib * 1048576u / sizeof(float); bytes = (size_t)n * sizeof(float);
        if (!allocate_arrays(n)) goto fail;
        CU(cuModuleGetFunction(&fn, module, "memory_triad"));
        args[0] = &da; args[1] = &db; args[2] = &dc; args[3] = &n;
        work = (double)bytes * 3.0;
        printf("MEMORY per_array_MiB=%u arrays=3 elements=%u traffic_bytes_per_launch=%.0f\n", mib, n, work);
        printf("C[i]=A[i]+2*B[i]; two reads + one write; exact full-array check every ~1s.\n");
    }
    printf("requested_seconds=%.1f grid=%u block=256 stop_file=%s\n", seconds_requested, (n + 255u) / 256u, stop_file);
    printf("Every launch is followed by an event wait; each kernel is bounded. Ctrl+C or stop_file cancels.\n");
    /* Check every seed before collecting timed sustained-work statistics. */
    for (warmup = 0; warmup < (compute ? 4u : 1u); ++warmup) {
        if (stop_requested()) { printf("STOP requested before warmup\n"); goto fail; }
        seed = warmup * .0625f;
        CU(cuLaunchKernel(fn, (n + 255u) / 256u, 1, 1, 256, 1, 1, 0, stream, args, NULL));
        CU(cuStreamSynchronize(stream)); CU(cuMemcpyDtoH(hc, dc, bytes));
        errors += compute ? check_compute(n, warmup) : check_array(n, 1);
        if (errors) { fprintf(stderr, "Warmup correctness failed mismatches=%llu\n", errors); goto fail; }
    }
    printf("Warmup_correctness=PASS validated_seed_count=%u (excluded from timed counters)\n", compute ? 4u : 1u);
    start = now_s();
    do {
        if (stop_requested()) { printf("STOP requested\n"); goto fail; }
        if (compute) { seed_index = (unsigned int)(iterations % 4); seed = seed_index * .0625f; }
        CU(cuEventRecord(ev0, stream));
        CU(cuLaunchKernel(fn, (n + 255u) / 256u, 1, 1, 256, 1, 1, 0, stream, args, NULL));
        CU(cuEventRecord(ev1, stream)); CU(cuEventSynchronize(ev1));
        CU(cuEventElapsedTime(&ms, ev0, ev1));
        if (ms > 250.0f) { fprintf(stderr, "STOP kernel_ms=%.3f exceeds lab limit 250ms\n", ms); goto fail; }
        if (ms < min_ms) min_ms = ms;
        if (ms > max_ms) max_ms = ms;
        gpu_ms += ms; ++iterations; elapsed = now_s() - start;
        if (elapsed - last_check >= 1.0 || elapsed >= seconds_requested) {
            CU(cuMemcpyDtoH(hc, dc, bytes));
            errors += compute ? check_compute(n, seed_index) : check_array(n, 1);
            ++checks; checked += compute ? (unsigned long long)n * 8 : n;
            last_check = elapsed;
            if (errors) { fprintf(stderr, "CHECK FAILED mismatches=%llu\n", errors); goto fail; }
            metric = work * (double)iterations / (gpu_ms / 1000) / 1e9;
            row("progress", elapsed, iterations, gpu_ms, metric, compute ? "GFLOP/s" : "GB/s", errors, checked);
        }
        if (elapsed - last_print >= 10.0) {
            printf("progress elapsed_s=%.1f launches=%llu checks=%llu mismatches=%llu\n", elapsed, iterations, checks, errors);
            last_print = elapsed;
        }
    } while (elapsed < seconds_requested);
    elapsed = now_s() - start;
    metric = work * (double)iterations / (gpu_ms / 1000) / 1e9;
    printf("%s elapsed_s=%.3f launches=%llu gpu_total_ms=%.3f\n", compute ? "COMPUTE" : "MEMORY", elapsed, iterations, gpu_ms);
    printf("kernel_average_ms=%.6f kernel_effective_%s=%.3f wall_effective_%s=%.3f\n", gpu_ms / iterations,
           compute ? "GFLOPs" : "GBps", metric, compute ? "GFLOPs" : "GBps", work * iterations / elapsed / 1e9);
    printf("kernel_min_ms=%.6f kernel_max_ms=%.6f lab_stop_threshold_ms=250\n", min_ms, max_ms);
    printf("checks=%llu checked_values=%llu mismatches=%llu max_abs_error=%s tolerance=0\n", checks, checked, errors, errors ? "see_failure" : "0");
    row("final_kernel_rate", elapsed, iterations, gpu_ms, metric, compute ? "GFLOP/s" : "GB/s", errors, checked);
    row("final_wall_rate", elapsed, iterations, gpu_ms, work * iterations / elapsed / 1e9,
        compute ? "GFLOP/s" : "GB/s", errors, checked);
    return 1;
fail: return 0;
}
static void usage(void)
{
    printf("Usage from lab directory:\n  bin\\api_stress.exe cpu|info|vector|transfer|compute|memory\n");
    printf("    [--seconds 1..600] [--mib 8..256] [--csv FILE] [--stop-file FILE]\n");
}
int main(int argc, char **argv)
{
    int i, ok = 0; const char *csv_path = NULL; time_t stamp; char stamp_text[64];
    QueryPerformanceFrequency(&frequency); SetConsoleCtrlHandler(control_handler, TRUE);
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    if (argc < 2) { usage(); return 2; } mode = argv[1];
    for (i = 2; i < argc; ++i) {
        if (i + 1 >= argc) { usage(); return 2; }
        if (!strcmp(argv[i], "--seconds")) seconds_requested = atof(argv[++i]);
        else if (!strcmp(argv[i], "--mib")) mib = (unsigned int)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--csv")) csv_path = argv[++i];
        else if (!strcmp(argv[i], "--stop-file")) stop_file = argv[++i];
        else { usage(); return 2; }
    }
    if (!isfinite(seconds_requested) || seconds_requested < 1 || seconds_requested > 600 || mib < 8 || mib > 256) {
        fprintf(stderr, "Parameter out of laboratory bounds\n"); return 2;
    }
    if (csv_path) {
        csv = fopen(csv_path, "wb"); if (!csv) { perror(csv_path); return 2; }
        fprintf(csv, "mode,phase,elapsed_s,iterations,gpu_ms,metric,units,errors,checked_values\n");
    }
    stamp = time(NULL); strftime(stamp_text, sizeof(stamp_text), "%Y-%m-%d %H:%M:%S", localtime(&stamp));
    printf("C / CUDA Driver API laboratory\nLocal_time=%s host_compiler=%d C_mode=%d CUDA_header=%d\n", stamp_text, _MSC_FULL_VER,
#ifdef __cplusplus
           0,
#else
           1,
#endif
           CUDA_VERSION);
    printf("Command:"); for (i = 0; i < argc; ++i) printf(" %s", argv[i]); printf("\n");
    if (!strcmp(mode, "cpu")) ok = cpu_test();
    else if (!strcmp(mode, "info")) ok = load_gpu(0);
    else if (!strcmp(mode, "vector")) ok = vector_test();
    else if (!strcmp(mode, "transfer")) ok = transfer_test();
    else if (!strcmp(mode, "compute")) ok = stress_test(1);
    else if (!strcmp(mode, "memory")) ok = stress_test(0);
    else usage();
    clean_gpu();
    if (api_errors) ok = 0;
    printf("RESULT=%s CUDA_API_errors=%d\n", ok ? "PASS" : "FAIL", api_errors);
    if (csv) fclose(csv);
    return ok ? 0 : 1;
}
