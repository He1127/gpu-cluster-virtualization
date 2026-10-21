/* Ordinary C host experiments, compiled by MSVC /TC. No CUDA kernel runs here. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef OPTIMIZED_BUILD
#define BUILD_MODE "O2"
#else
#define BUILD_MODE "Od"
#endif
#define COUNT_LIMIT 100000000ULL
#define CUDA_BIN "C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\v12.6\\bin"

static char project[MAX_PATH], executable[MAX_PATH];
static FILE *output_log, *api_log;
static int api_count, debug_requested;

static void emit(const char *format, ...)
{
    char text[4096];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    fputs(text, stdout); fflush(stdout);
    OutputDebugStringA(text);
    if (output_log) { fputs(text, output_log); fflush(output_log); }
}

static void path_for(char *target, const char *relative)
{
    snprintf(target, MAX_PATH, "%s\\%s", project, relative);
}

static FILE *open_result(const char *name, const char *mode)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\results\\%s", project, name);
    return fopen(path, mode);
}

static int windows_ok(const char *api, int success)
{
    DWORD error = success ? ERROR_SUCCESS : GetLastError();
    ++api_count;
    if (api_log) { fprintf(api_log, "%d,%s,%d,%lu\n", api_count, api, success, error); fflush(api_log); }
    if (!success) emit("%s failed: Win32 error %lu\n", api, error);
    return success;
}

__declspec(noinline) static void debug_checkpoint(const char *label)
{
    if (debug_requested) {
        emit("Debug checkpoint: %s\n", label);
        if (IsDebuggerPresent()) DebugBreak();
        else emit("No debugger attached; continuing without DebugBreak.\n");
    }
}

static void timestamp(char *text, size_t capacity)
{
    SYSTEMTIME value;
    GetLocalTime(&value);
    snprintf(text, capacity, "%04u-%02u-%02uT%02u:%02u:%02u.%03u", value.wYear,
        value.wMonth, value.wDay, value.wHour, value.wMinute, value.wSecond, value.wMilliseconds);
}

static int file_sha256(const char *path, char digest[65])
{
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    FILE *file = NULL;
    unsigned char block[16384], bytes[32];
    size_t count;
    DWORD length = sizeof(bytes);
    int ok = 0, i;
    if (!CryptAcquireContextA(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) goto cleanup;
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) goto cleanup;
    file = fopen(path, "rb"); if (!file) goto cleanup;
    while ((count = fread(block, 1, sizeof(block), file)) != 0)
        if (!CryptHashData(hash, block, (DWORD)count, 0)) goto cleanup;
    if (ferror(file) || !CryptGetHashParam(hash, HP_HASHVAL, bytes, &length, 0) || length != 32) goto cleanup;
    for (i = 0; i < 32; ++i) snprintf(digest + 2 * i, 3, "%02x", bytes[i]);
    digest[64] = '\0'; ok = 1;
cleanup:
    if (file) fclose(file);
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    return ok;
}

static int capture_tool(const char *tool, const char *arguments, const char *raw_name,
                        char *text, size_t capacity)
{
    SECURITY_ATTRIBUTES attributes;
    STARTUPINFOA start;
    PROCESS_INFORMATION process;
    HANDLE pipe_read = NULL, pipe_write = NULL;
    char command[2048], block[4096];
    DWORD count = 0, exit_code = 1;
    size_t used = 0;
    FILE *raw = NULL;
    int ok = 0;
    ZeroMemory(&attributes, sizeof(attributes));
    attributes.nLength = sizeof(attributes); attributes.bInheritHandle = TRUE;
    ZeroMemory(&start, sizeof(start)); ZeroMemory(&process, sizeof(process));
    if (!windows_ok("CreatePipe(tool)", CreatePipe(&pipe_read, &pipe_write, &attributes, 0))) goto cleanup;
    if (!windows_ok("SetHandleInformation(tool)", SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0))) goto cleanup;
    start.cb = sizeof(start); start.dwFlags = STARTF_USESTDHANDLES;
    start.hStdOutput = pipe_write; start.hStdError = pipe_write; start.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    snprintf(command, sizeof(command), "\"%s\" %s", tool, arguments);
    raw = open_result(raw_name, "wb"); if (!raw) goto cleanup;
    if (!windows_ok("CreateProcessA(tool)", CreateProcessA(tool, command, NULL, NULL, TRUE,
            CREATE_NO_WINDOW, NULL, project, &start, &process))) goto cleanup;
    CloseHandle(pipe_write); pipe_write = NULL;
    while (ReadFile(pipe_read, block, sizeof(block), &count, NULL) && count) {
        size_t copy = count;
        if (fwrite(block, 1, count, raw) != count) goto cleanup;
        if (copy > capacity - 1 - used) copy = capacity - 1 - used;
        memcpy(text + used, block, copy); used += copy;
    }
    text[used] = '\0';
    if (!windows_ok("WaitForSingleObject(tool)", WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0) ||
        !windows_ok("GetExitCodeProcess(tool)", GetExitCodeProcess(process.hProcess, &exit_code))) goto cleanup;
    if (exit_code != 0) { emit("Tool returned %lu: %s\n", exit_code, tool); goto cleanup; }
    ok = 1;
cleanup:
    if (raw) fclose(raw);
    if (pipe_read) CloseHandle(pipe_read);
    if (pipe_write) CloseHandle(pipe_write);
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    return ok;
}

static int find_line(const char *text, const char *needle, char *line, size_t capacity)
{
    const char *at = strstr(text, needle), *end;
    size_t count;
    if (!at) return 0;
    while (at > text && at[-1] != '\n') --at;
    end = strchr(at, '\n'); count = end ? (size_t)(end - at) : strlen(at);
    while (count && at[count - 1] == '\r') --count;
    if (count >= capacity) count = capacity - 1;
    memcpy(line, at, count); line[count] = '\0'; return 1;
}

static int step01(void)
{
    typedef LONG (WINAPI *RtlGetVersionFunction)(OSVERSIONINFOW *);
    OSVERSIONINFOW version;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    RtlGetVersionFunction get_version = (RtlGetVersionFunction)GetProcAddress(ntdll, "RtlGetVersion");
    char cpu[256], nvcc[16384], object_dump[16384], driver[16384], line[512];
    DWORD size = sizeof(cpu), cpu_type = 0;
    FILE *data;
    ZeroMemory(&version, sizeof(version)); version.dwOSVersionInfoSize = sizeof(version);
    if (!get_version || get_version(&version) != 0) { emit("RtlGetVersion failed\n"); return 1; }
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        "ProcessorNameString", RRF_RT_REG_SZ, &cpu_type, cpu, &size) != ERROR_SUCCESS) return 1;
    if (!capture_tool(CUDA_BIN "\\nvcc.exe", "--version", "01_nvcc_version_raw.txt", nvcc, sizeof(nvcc)) ||
        !capture_tool(CUDA_BIN "\\cuobjdump.exe", "--version", "01_cuobjdump_version_raw.txt", object_dump, sizeof(object_dump)) ||
        !capture_tool("C:\\Windows\\System32\\nvidia-smi.exe", "--query-gpu=name,driver_version --format=csv,noheader", "01_driver_version_raw.txt", driver, sizeof(driver))) return 1;
    emit("Hello from C.\nWindows: %lu.%lu.%lu (x64)\nCPU: %s\n", version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber, cpu);
    emit("Logical processors: %lu\nMSVC: _MSC_VER=%d, _MSC_FULL_VER=%d\n", GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), _MSC_VER, _MSC_FULL_VER);
    if (!find_line(nvcc, "release ", line, sizeof(line))) return 1;
    emit("NVCC: %s\n", line);
    if (!find_line(object_dump, "release ", line, sizeof(line))) return 1;
    emit("cuobjdump: %s\n", line);
    if (!find_line(driver, ",", line, sizeof(line))) return 1;
    emit("GPU, driver: %s\nPointer size: %zu bytes\n", line, sizeof(void *));
    data = open_result("01_environment.json", "w"); if (!data) return 1;
    fprintf(data, "{\"Windows\":\"%lu.%lu.%lu\",\"CPU\":\"%s\",\"logical_processors\":%lu,\"MSC_VER\":%d,\"MSC_FULL_VER\":%d,\"pointer_bytes\":%zu}\n",
        version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber, cpu,
        GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), _MSC_VER, _MSC_FULL_VER, sizeof(void *));
    fclose(data); return 0;
}

/* BREAKPOINT: ordinary C functions kept as real out-of-line calls in both builds. */
__declspec(noinline) uint64_t demo_add(uint64_t left, uint64_t right) { return left + right; }
__declspec(noinline) uint64_t demo_multiply(uint64_t left, uint64_t right) { return left * right; }
__declspec(noinline) uint64_t demo_sum(const uint32_t *values, size_t count)
{
    uint64_t total = 0;
    size_t index;
    for (index = 0; index < count; ++index) total += values[index];
    return total;
}

static uint32_t *make_values(size_t count, uint64_t seed, uint64_t *expected)
{
    uint32_t *values = (uint32_t *)malloc(count * sizeof(uint32_t));
    size_t index;
    uint64_t reference = 0;
    if (!values) return NULL;
    for (index = 0; index < count; ++index) {
        uint32_t value = (uint32_t)(((uint64_t)index * seed + 3ULL) % 1009ULL);
        values[index] = value; reference += value;
    }
    *expected = reference; return values;
}

static int step02(uint64_t left, uint64_t right, size_t count, uint64_t seed)
{
    uint64_t expected, added, multiplied, total;
    uint32_t *values = make_values(count, seed, &expected);
    FILE *data;
    if (!values) return 1;
    debug_checkpoint("before demo_add / demo_multiply / demo_sum");
    added = demo_add(left, right);             /* BREAKPOINT: F11 enters real C function. */
    multiplied = demo_multiply(left, right);
    total = demo_sum(values, count);
    emit("Build: %s; runtime inputs: a=%llu b=%llu n=%zu seed=%llu\n", BUILD_MODE,
        (unsigned long long)left, (unsigned long long)right, count, (unsigned long long)seed);
    emit("demo_add: %llu\ndemo_multiply: %llu\ndemo_sum: %llu\nReference sum: %llu\nChecks: %s\n",
        (unsigned long long)added, (unsigned long long)multiplied, (unsigned long long)total,
        (unsigned long long)expected,
        added == left + right && multiplied == left * right && total == expected ? "PASS" : "FAIL");
    { char name[64]; snprintf(name, sizeof(name), "02_%s_values.json", BUILD_MODE); data = open_result(name, "w"); }
    if (data) {
        fprintf(data, "{\"build\":\"%s\",\"a\":%llu,\"b\":%llu,\"n\":%zu,\"seed\":%llu,\"add\":%llu,\"multiply\":%llu,\"sum\":%llu,\"reference_sum\":%llu}\n",
            BUILD_MODE, (unsigned long long)left, (unsigned long long)right, count, (unsigned long long)seed,
            (unsigned long long)added, (unsigned long long)multiplied, (unsigned long long)total, (unsigned long long)expected);
        fclose(data);
    }
    free(values);
    return data && added == left + right && multiplied == left * right && total == expected ? 0 : 1;
}

static int step03(void)
{
    SYSTEM_INFO info;
    unsigned char *memory = NULL, *returned = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD written = 0, read = 0;
    size_t index, bytes;
    unsigned long long checksum = 0, expected = 0;
    LARGE_INTEGER beginning;
    char path[MAX_PATH];
    int ok = 0;
    GetSystemInfo(&info); bytes = info.dwPageSize;
    debug_checkpoint("before VirtualAlloc");
    memory = (unsigned char *)VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); /* BREAKPOINT */
    if (!windows_ok("VirtualAlloc", memory != NULL)) goto cleanup;
    returned = (unsigned char *)malloc(bytes); if (!returned) goto cleanup;
    for (index = 0; index < bytes; ++index) { memory[index] = (unsigned char)(index % 251); expected += index % 251; }
    for (index = 0; index < bytes; ++index) checksum += memory[index];
    if (checksum != expected) goto cleanup;
    emit("VirtualAlloc: %zu bytes at %p\nMemory checksum: %llu; check=PASS\n", bytes, (void *)memory, checksum);
    path_for(path, "results\\03_virtualalloc_roundtrip.bin");
    file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!windows_ok("CreateFileA", file != INVALID_HANDLE_VALUE)) goto cleanup;
    emit("CreateFileA: success\n");
    if (!windows_ok("WriteFile", WriteFile(file, memory, (DWORD)bytes, &written, NULL)) || written != bytes) goto cleanup;
    emit("WriteFile: %lu bytes\n", written);
    beginning.QuadPart = 0;
    if (!windows_ok("SetFilePointerEx", SetFilePointerEx(file, beginning, NULL, FILE_BEGIN))) goto cleanup;
    if (!windows_ok("ReadFile", ReadFile(file, returned, (DWORD)bytes, &read, NULL)) || read != bytes) goto cleanup;
    emit("ReadFile: %lu bytes\n", read);
    if (memcmp(memory, returned, bytes)) { emit("Round-trip bytes: FAIL\n"); goto cleanup; }
    emit("Round-trip bytes: PASS\n"); ok = 1;
cleanup:
    if (file != INVALID_HANDLE_VALUE) { int closed = windows_ok("CloseHandle(file)", CloseHandle(file)); if (!closed) ok = 0; else emit("CloseHandle: success\n"); }
    if (memory) { int freed = windows_ok("VirtualFree", VirtualFree(memory, 0, MEM_RELEASE)); if (!freed) ok = 0; else emit("VirtualFree: success\n"); }
    free(returned); return ok ? 0 : 1;
}

typedef struct Worker {
    const uint32_t *values;
    size_t first, end;
    uint64_t total;
    DWORD thread_id;
} Worker;

static DWORD WINAPI sum_worker(LPVOID argument)
{
    Worker *worker = (Worker *)argument;
    worker->thread_id = GetCurrentThreadId();
    worker->total = demo_sum(worker->values + worker->first, worker->end - worker->first);
    return 0; /* No printf, malloc, or other CRT state operation in the worker. */
}

static int step04(size_t count, uint64_t seed)
{
    uint64_t expected;
    uint32_t *values = make_values(count, seed, &expected);
    int trial, ok = 0;
    FILE *data = open_result("04_threads.csv", "w");
    if (!values || !data) goto cleanup;
    fprintf(data, "workers,worker,TID,first_inclusive,end_exclusive,partial_sum,total,reference,wait_result,elapsed_ms_including_thread_creation\n");
    emit("n=%zu seed=%llu reference=%llu\n", count, (unsigned long long)seed, (unsigned long long)expected);
    for (trial = 1; trial <= 4; trial *= 2) {
        HANDLE handles[4] = {NULL, NULL, NULL, NULL};
        Worker workers[4];
        int index, created = 0, trial_ok = 1;
        uint64_t total = 0;
        DWORD waited = WAIT_FAILED;
        LARGE_INTEGER frequency, started, ended;
        double elapsed_ms;
        ZeroMemory(workers, sizeof(workers));
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&started);
        debug_checkpoint("before CreateThread");
        for (index = 0; index < trial; ++index) {
            workers[index].values = values;
            workers[index].first = count * (size_t)index / (size_t)trial;
            workers[index].end = count * (size_t)(index + 1) / (size_t)trial;
            handles[index] = CreateThread(NULL, 0, sum_worker, &workers[index], 0, NULL); /* BREAKPOINT */
            if (!windows_ok("CreateThread", handles[index] != NULL)) { trial_ok = 0; break; }
            ++created;
        }
        if (created) {
            waited = WaitForMultipleObjects((DWORD)created, handles, TRUE, INFINITE);
            if (!windows_ok("WaitForMultipleObjects", waited == WAIT_OBJECT_0)) trial_ok = 0;
        }
        QueryPerformanceCounter(&ended);
        elapsed_ms = 1000.0 * (double)(ended.QuadPart - started.QuadPart) / (double)frequency.QuadPart;
        for (index = 0; index < created; ++index) {
            DWORD exit_code;
            if (!windows_ok("GetExitCodeThread", GetExitCodeThread(handles[index], &exit_code)) || exit_code != 0) trial_ok = 0;
            if (!windows_ok("CloseHandle(thread)", CloseHandle(handles[index]))) trial_ok = 0;
            total += workers[index].total;
        }
        if (created != trial || total != expected) trial_ok = 0;
        emit("workers=%d total=%llu wait=%lu check=%s time=%.3f ms\n", trial,
            (unsigned long long)total, waited, trial_ok ? "PASS" : "FAIL", elapsed_ms);
        for (index = 0; index < created; ++index) {
            emit("  TID=%lu range=[%zu,%zu) sum=%llu\n", workers[index].thread_id,
                workers[index].first, workers[index].end, (unsigned long long)workers[index].total);
            fprintf(data, "%d,%d,%lu,%zu,%zu,%llu,%llu,%llu,%lu,%.6f\n", trial, index, workers[index].thread_id,
                workers[index].first, workers[index].end, (unsigned long long)workers[index].total,
                (unsigned long long)total, (unsigned long long)expected, waited, elapsed_ms);
        }
        if (!trial_ok) goto cleanup;
    }
    ok = 1;
cleanup:
    if (data) fclose(data); free(values); return ok ? 0 : 1;
}

static int read_u64(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long parsed;
    if (!text[0] || text[0] == '-') return 0;
    parsed = _strtoui64(text, &end, 10);
    if (*end) return 0;
    *value = parsed; return 1;
}

int main(int argc, char **argv)
{
    int step, index, code = 1;
    uint64_t left = 37, right = 19, count = 100000, seed = 17;
    char *slash, directory[MAX_PATH], name[128], started[64], ended[64];
    char source[MAX_PATH], source_hash[65], exe_hash[65];
    FILE *session;
    GetModuleFileNameA(NULL, executable, sizeof(executable));
    strcpy(project, executable);
    slash = strrchr(project, '\\'); if (!slash) return 2; *slash = '\0';
    slash = strrchr(project, '\\'); if (!slash) return 2; *slash = '\0';
    path_for(directory, "results");
    if (!CreateDirectoryA(directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 2;
    if (argc < 2 || (step = atoi(argv[1])) < 1 || step > 4) {
        emit("Usage: cpu_labs_Od.exe 01|02|03|04 [--a 37 --b 19 --n 100000 --seed 17] [--debug]\n"); return 2;
    }
    for (index = 2; index < argc; ++index) {
        uint64_t value;
        const char *flag = argv[index];
        if (!strcmp(flag, "--debug")) { debug_requested = 1; continue; }
        if (++index >= argc || !read_u64(argv[index], &value)) return 2;
        if (!strcmp(flag, "--a")) left = value;
        else if (!strcmp(flag, "--b")) right = value;
        else if (!strcmp(flag, "--n")) count = value;
        else if (!strcmp(flag, "--seed")) seed = value;
        else return 2;
    }
    if (!count || count > COUNT_LIMIT || seed > 1000000000ULL) return 2;
    snprintf(name, sizeof(name), "%02d_%s_stdout.txt", step, BUILD_MODE); output_log = open_result(name, "w");
    snprintf(name, sizeof(name), "%02d_%s_api.csv", step, BUILD_MODE); api_log = open_result(name, "w");
    if (!output_log || !api_log) return 2;
    fprintf(api_log, "order,api,success,win32_error_when_failed\n"); timestamp(started, sizeof(started));
    switch (step) {
        case 1: code = step01(); break;
        case 2: code = step02(left, right, (size_t)count, seed); break;
        case 3: code = step03(); break;
        case 4: code = step04((size_t)count, seed); break;
    }
    path_for(source, "src\\cpu_labs.c");
    if (!file_sha256(source, source_hash) || !file_sha256(executable, exe_hash)) { emit("Source/executable SHA256 failed\n"); code = 1; strcpy(source_hash, "unavailable"); strcpy(exe_hash, "unavailable"); }
    timestamp(ended, sizeof(ended));
    snprintf(name, sizeof(name), "%02d_%s_session.json", step, BUILD_MODE); session = open_result(name, "w");
    if (session) {
        fprintf(session, "{\"step\":%d,\"language\":\"C\",\"build\":\"%s\",\"MSC_FULL_VER\":%d,\"started_local\":\"%s\",\"ended_local\":\"%s\",\"status\":\"%s\",\"exit_code\":%d,\"source_sha256\":\"%s\",\"executable_sha256\":\"%s\",\"a\":%llu,\"b\":%llu,\"n\":%llu,\"seed\":%llu,\"debug_requested\":%s}\n",
            step, BUILD_MODE, _MSC_FULL_VER, started, ended, code == 0 ? "complete" : "failed", code,
            source_hash, exe_hash, (unsigned long long)left, (unsigned long long)right,
            (unsigned long long)count, (unsigned long long)seed, debug_requested ? "true" : "false");
        fclose(session);
    } else code = 1;
    fclose(api_log); fclose(output_log); api_log = NULL; output_log = NULL;
    return code;
}
