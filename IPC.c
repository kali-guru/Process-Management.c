#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define RING_CAP    1024
#define PAYLOAD_SZ  64

static const wchar_t *MAP_NAME   = L"HL_BANK_IPC_MAP";
static const wchar_t *SEM_EMPTY  = L"HL_BANK_IPC_EMPTY";
static const wchar_t *SEM_FULL   = L"HL_BANK_IPC_FULL";
static const wchar_t *SEM_MUTEX  = L"HL_BANK_IPC_MUTEX";

typedef struct {
    uint32_t tx_id;
    uint32_t type;
    uint64_t amount_pence;
    uint64_t t_send_us;
    char payload[PAYLOAD_SZ];
} TxMsg;

typedef struct {
    LONG head;
    LONG tail;
    TxMsg buf[RING_CAP];
} Ring;

static LARGE_INTEGER g_freq;
static int g_unsafe = 0;

static long long now_us(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (long long)((c.QuadPart * 1000000LL) / g_freq.QuadPart);
}

static void die_last(const char *m) {
    fprintf(stderr, "%s (err=%lu)\n", m, (unsigned long)GetLastError());
    ExitProcess(1);
}

static void print_header(const char *mode) {
    printf("=====================================================\n");
    printf(" HL Banking System - Windows IPC (Shared Memory) [%s]\n", mode);
    printf("=====================================================\n");
}

static int run_child(int n) {
    HANDLE hMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MAP_NAME);
    if (!hMap) die_last("OpenFileMappingW");

    Ring *ring = (Ring*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ring));
    if (!ring) die_last("MapViewOfFile");

    HANDLE hEmpty = OpenSemaphoreW(SEMAPHORE_ALL_ACCESS, FALSE, SEM_EMPTY);
    HANDLE hFull  = OpenSemaphoreW(SEMAPHORE_ALL_ACCESS, FALSE, SEM_FULL);
    HANDLE hMutex = NULL;
    if (!g_unsafe) hMutex = OpenSemaphoreW(SEMAPHORE_ALL_ACCESS, FALSE, SEM_MUTEX);

    if (!hEmpty || !hFull || (!g_unsafe && !hMutex)) die_last("OpenSemaphoreW");

    uint8_t *seen = (uint8_t*)calloc((size_t)n + 1, 1);
    if (!seen) die_last("calloc");

    long long lat_sum = 0, min_lat = LLONG_MAX, max_lat = 0;
    long long proc_sum = 0, min_proc = LLONG_MAX, max_proc = 0;

    long long start_all = now_us();

    for (int i = 0; i < n; i++) {
        long long t0 = now_us();

        WaitForSingleObject(hFull, INFINITE);
        if (!g_unsafe) WaitForSingleObject(hMutex, INFINITE);

        LONG t = ring->tail;
        TxMsg msg = ring->buf[t];
        ring->tail = (t + 1) % RING_CAP;

        if (!g_unsafe) ReleaseSemaphore(hMutex, 1, NULL);
        ReleaseSemaphore(hEmpty, 1, NULL);

        long long t1 = now_us();
        long long proc = t1 - t0;
        proc_sum += proc;
        if (proc < min_proc) min_proc = proc;
        if (proc > max_proc) max_proc = proc;

        long long lat = t1 - (long long)msg.t_send_us;
        lat_sum += lat;
        if (lat < min_lat) min_lat = lat;
        if (lat > max_lat) max_lat = lat;

        if (msg.tx_id >= 1 && msg.tx_id <= (uint32_t)n) {
            seen[msg.tx_id] += 1;
        }
    }

    long long end_all = now_us();
    double total_s = (end_all - start_all) / 1000000.0;

    int missing = 0, dup = 0;
    for (int id = 1; id <= n; id++) {
        if (seen[id] == 0) missing++;
        if (seen[id] > 1) dup += (seen[id] - 1);
    }
    free(seen);

    printf("\n------------------- CONSUMER (Logging/Audit) -------------------\n");
    printf("Transactions Processed : %d\n", n);
    printf("Total Receive Time     : %.6f s\n", total_s);
    printf("Throughput             : %.2f msg/s\n", n / total_s);
    printf("\nAvg Proc Time/msg      : %.2f us | min=%I64d us | max=%I64d us\n",
           (double)proc_sum / n, min_proc, max_proc);
    printf("Avg One-way Latency    : %.2f us | min=%I64d us | max=%I64d us\n",
           (double)lat_sum / n, min_lat, max_lat);
    printf("\nIntegrity Check        : missing=%d | duplicate=%d\n", missing, dup);
    printf("----------------------------------------------------------------\n");

    UnmapViewOfFile(ring);
    CloseHandle(hMap);
    CloseHandle(hEmpty);
    CloseHandle(hFull);
    if (!g_unsafe) CloseHandle(hMutex);
    return 0;
}

int main(int argc, char **argv) {
    QueryPerformanceFrequency(&g_freq);

    if (argc == 2 && strcmp(argv[1], "--unsafe") == 0) g_unsafe = 1;

    // Check for child mode arguments
    if (argc == 3) {
        if (strcmp(argv[1], "--child") == 0) {
            int cn = atoi(argv[2]);
            return run_child(cn);
        }
        if (strcmp(argv[1], "--unsafe_child") == 0) {
            g_unsafe = 1;
            int cn = atoi(argv[2]);
            return run_child(cn);
        }
    }

    int n = 0;
    print_header(g_unsafe ? "UNSAFE (RACE DEMO)" : "SAFE");
    printf("Enter number of transactions to simulate: ");
    fflush(stdout);
    if (scanf("%d", &n) != 1 || n <= 0) {
        fprintf(stderr, "Invalid input.\n");
        return 1;
    }

    // Create shared memory
    HANDLE hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)sizeof(Ring), MAP_NAME);
    if (!hMap) die_last("CreateFileMappingW");

    Ring *ring = (Ring*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ring));
    if (!ring) die_last("MapViewOfFile");
    ZeroMemory(ring, sizeof(Ring));

    // Semaphores
    HANDLE hEmpty = CreateSemaphoreW(NULL, RING_CAP, RING_CAP, SEM_EMPTY);
    HANDLE hFull  = CreateSemaphoreW(NULL, 0,        RING_CAP, SEM_FULL);
    HANDLE hMutex = NULL;
    if (!g_unsafe) hMutex = CreateSemaphoreW(NULL, 1, 1, SEM_MUTEX);

    if (!hEmpty || !hFull || (!g_unsafe && !hMutex)) die_last("CreateSemaphoreW");

    // Spawn child process (same exe)
    char exe[MAX_PATH];
    char cmdline[2 * MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    
    if (g_unsafe) 
        snprintf(cmdline, sizeof(cmdline), "\"%s\" --unsafe_child %d", exe, n);
    else 
        snprintf(cmdline, sizeof(cmdline), "\"%s\" --child %d", exe, n);

    STARTUPINFOA si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        die_last("CreateProcessA");
    }

    const char *types[] = {"Transfer","Inquiry","BillPay","Fraud","Logging"};

    long long proc_sum = 0, min_proc = LLONG_MAX, max_proc = 0;

    long long start_all = now_us();

    for (int i = 0; i < n; i++) {
        long long t0 = now_us();

        WaitForSingleObject(hEmpty, INFINITE);
        if (!g_unsafe) WaitForSingleObject(hMutex, INFINITE);

        TxMsg msg;
        msg.tx_id = (uint32_t)(i + 1);
        msg.type = (uint32_t)(i % 5);
        msg.amount_pence = (uint64_t)(1000 + (i % 500)) * 100;
        msg.t_send_us = (uint64_t)now_us();
        _snprintf(msg.payload, sizeof(msg.payload), "HL_TX_%u %s", msg.tx_id, types[msg.type]);

        LONG h = ring->head;
        ring->buf[h] = msg;
        ring->head = (h + 1) % RING_CAP;

        if (!g_unsafe) ReleaseSemaphore(hMutex, 1, NULL);
        ReleaseSemaphore(hFull, 1, NULL);

        long long t1 = now_us();
        long long proc = t1 - t0;
        proc_sum += proc;
        if (proc < min_proc) min_proc = proc;
        if (proc > max_proc) max_proc = proc;
    }

    long long end_all = now_us();
    double total_s = (end_all - start_all) / 1000000.0;

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    printf("\n------------------- PRODUCER (Transaction Processor) -------------------\n");
    printf("Transactions Sent      : %d\n", n);
    printf("Total Send Time        : %.6f s\n", total_s);
    printf("Throughput             : %.2f msg/s\n", n / total_s);
    printf("\nAvg Proc Time/msg      : %.2f us | min=%I64d us | max=%I64d us\n",
           (double)proc_sum / n, min_proc, max_proc);
    printf("-----------------------------------------------------------------------\n");

    UnmapViewOfFile(ring);
    CloseHandle(hMap);
    CloseHandle(hEmpty);
    CloseHandle(hFull);
    if (!g_unsafe) CloseHandle(hMutex);

    return 0;
}
