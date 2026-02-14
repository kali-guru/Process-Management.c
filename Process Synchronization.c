#include <windows.h>
#include <stdio.h>
#include <locale.h>
#include <stdarg.h>
#include <wchar.h>

#define N_TX 3

CRITICAL_SECTION balance_lock;
double account_balance = 10000.0;

typedef struct {
    int tx_id;
    double amount;
} Transaction;

typedef struct {
    long long lock_wait_us;
    long long cs_time_us;
    long long total_time_us;
} Metrics;

LARGE_INTEGER freq;
Metrics metrics[N_TX];

long long now_us() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (c.QuadPart * 1000000LL) / freq.QuadPart;
}

void print_time_hms() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    printf("%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

/* Write formatted wide (Unicode) string directly to the Windows console */
void wprintf_console(const wchar_t *fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, fmt, ap);
    va_end(ap);
    fflush(stdout);
    DWORD written = 0;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteConsoleW(h, buf, (DWORD)wcslen(buf), &written, NULL);
}

DWORD WINAPI process_transaction(LPVOID arg) {
    Transaction *tx = (Transaction*)arg;
    int idx = tx->tx_id - 1;

    long long start = now_us();
    long long wait_s = now_us();
    EnterCriticalSection(&balance_lock);
    long long wait_e = now_us();

    long long cs_s = now_us();

    double before = account_balance;
    Sleep(2); // simulate processing
    double after = before - tx->amount;
    account_balance = after;

    long long cs_e = now_us();
    
    // Print output while holding the lock to ensure atomicity
    printf("\n----------------------------------------------------\n");
    printf("[Transaction %d] Started at ", tx->tx_id);
    print_time_hms();
    printf("\n");
    wprintf_console(L"Amount Deducted         : \u00A3%.2f\n", tx->amount);
    wprintf_console(L"Balance Before          : \u00A3%.2f\n", before);
    wprintf_console(L"Balance After           : \u00A3%.2f\n", after);
    printf("Lock Wait Time          : %lld us\n", wait_e - wait_s);
    printf("Critical Section Time   : %lld us\n", cs_e - cs_s);
    printf("Total Execution Time    : %lld us\n", (long long)(now_us() - start));
    printf("----------------------------------------------------\n");
    
    LeaveCriticalSection(&balance_lock);

    long long end = now_us();

    metrics[idx].lock_wait_us  = wait_e - wait_s;
    metrics[idx].cs_time_us    = cs_e - cs_s;
    metrics[idx].total_time_us = end - start;

    return 0;
}

static void print_avg_min_max(const char* label, long long* arr, int n) {
    long long sum = 0, mn = arr[0], mx = arr[0];
    for (int i = 0; i < n; i++) {
        sum += arr[i];
        if (arr[i] < mn) mn = arr[i];
        if (arr[i] > mx) mx = arr[i];
    }
    long long avg = sum / n;
    printf("%-24s: avg=%lld us | min=%lld us | max=%lld us\n", label, avg, mn, mx);
}

int main() {
    QueryPerformanceFrequency(&freq);
    InitializeCriticalSection(&balance_lock);

    /* Ensure Windows console is set to UTF-8 so '£' prints correctly */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    setlocale(LC_ALL, "");

    HANDLE th[N_TX];
    Transaction txs[N_TX] = {
        {1, 1000.0},
        {2,  500.0},
        {3, 1200.0}
    };

    printf("====================================================\n");
    printf(" HL Banking System - Windows Sync + Timing + Averages\n");
    printf("====================================================\n");
    wprintf_console(L"Initial Account Balance : \u00A3%.2f\n", account_balance);
    printf("Total Transactions      : %d\n", N_TX);

    long long program_start = now_us();

    for (int i = 0; i < N_TX; i++) {
        th[i] = CreateThread(NULL, 0, process_transaction, &txs[i], 0, NULL);
    }

    WaitForMultipleObjects(N_TX, th, TRUE, INFINITE);

    for (int i = 0; i < N_TX; i++) CloseHandle(th[i]);

    long long program_end = now_us();

    // build arrays for avg/min/max
    long long lock_wait[N_TX], cs_time[N_TX], total_time[N_TX];
    for (int i = 0; i < N_TX; i++) {
        lock_wait[i]  = metrics[i].lock_wait_us;
        cs_time[i]    = metrics[i].cs_time_us;
        total_time[i] = metrics[i].total_time_us;
    }

    printf("\n===================== SUMMARY ======================\n");
    wprintf_console(L"Final Account Balance   : \u00A3%.2f\n", account_balance);
    printf("Total Program Time      : %lld us\n", (program_end - program_start));
    printf("----------------------------------------------------\n");
    print_avg_min_max("Lock Wait Time",        lock_wait,  N_TX);
    print_avg_min_max("Critical Section Time", cs_time,    N_TX);
    print_avg_min_max("Total Execution Time",  total_time, N_TX);
    printf("====================================================\n");

    DeleteCriticalSection(&balance_lock);
    return 0;
}
