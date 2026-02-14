// linux_sync_avg_time_windows_format.c
// HL Banking System - Linux Synchronization (pthread_mutex) + Timing + Averages
// OUTPUT FORMAT MATCHES your Windows sample exactly.
//
// Compile: gcc -O2 -pthread linux_sync_avg_time_windows_format.c -o Linux_syn
// Run:     ./Linux_syn

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

#define N_TX 3

// Shared state
static double account_balance = 10000.0;

// Locks
static pthread_mutex_t balance_lock; // protects shared balance (correctness)
static pthread_mutex_t print_lock;   // prevents interleaved printf blocks

// Transaction + Metrics
typedef struct {
    int tx_id;
    double amount;
} Transaction;

typedef struct {
    long long lock_wait_us;
    long long cs_time_us;
    long long total_time_us;
} Metrics;

static Metrics metrics[N_TX];

// ------------------------------------------------------------
// Timing helpers
// ------------------------------------------------------------
static long long now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000000LL) + tv.tv_usec;
}

// Human-readable local time: HH:MM:SS.mmm (like Windows output)
static void get_time_hms_ms(char out[32]) {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm lt;
    localtime_r(&tv.tv_sec, &lt);

    int ms = (int)(tv.tv_usec / 1000);
    snprintf(out, 32, "%02d:%02d:%02d.%03d",
             lt.tm_hour, lt.tm_min, lt.tm_sec, ms);
}

// ------------------------------------------------------------
// Stats helper (avg/min/max)
// ------------------------------------------------------------
static void stats(const long long *arr, int n, long long *avg, long long *mn, long long *mx) {
    long long sum = 0;
    *mn = arr[0];
    *mx = arr[0];
    for (int i = 0; i < n; i++) {
        sum += arr[i];
        if (arr[i] < *mn) *mn = arr[i];
        if (arr[i] > *mx) *mx = arr[i];
    }
    *avg = sum / n;
}

// ------------------------------------------------------------
// Thread function
// ------------------------------------------------------------
static void* process_transaction(void* arg) {
    Transaction *tx = (Transaction*)arg;
    int idx = tx->tx_id - 1;

    char started_at[32];
    get_time_hms_ms(started_at);

    long long start = now_us();

    // Lock wait timing
    long long wait_s = now_us();
    pthread_mutex_lock(&balance_lock);
    long long wait_e = now_us();

    // Critical section timing
    long long cs_s = now_us();

    double before = account_balance;

    // Simulate work inside critical section (same idea as Windows Sleep(2))
    usleep(2000);

    double after = before - tx->amount;
    account_balance = after;

    long long cs_e = now_us();
    pthread_mutex_unlock(&balance_lock);

    long long end = now_us();

    metrics[idx].lock_wait_us  = wait_e - wait_s;
    metrics[idx].cs_time_us    = cs_e - cs_s;
    metrics[idx].total_time_us = end - start;

    // Print whole transaction block exactly like Windows sample
    pthread_mutex_lock(&print_lock);

    printf("\n----------------------------------------------------\n");
    printf("[Transaction %d] Started at %s\n", tx->tx_id, started_at);
    printf("Amount Deducted         : £%.2f\n", tx->amount);
    printf("Balance Before          : £%.2f\n", before);
    printf("Balance After           : £%.2f\n", after);
    printf("Lock Wait Time          : %lld us\n", metrics[idx].lock_wait_us);
    printf("Critical Section Time   : %lld us\n", metrics[idx].cs_time_us);
    printf("Total Execution Time    : %lld us\n", metrics[idx].total_time_us);
    printf("----------------------------------------------------\n");

    pthread_mutex_unlock(&print_lock);

    return NULL;
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------
int main(void) {
    pthread_mutex_init(&balance_lock, NULL);
    pthread_mutex_init(&print_lock, NULL);

    pthread_t th[N_TX];
    Transaction txs[N_TX] = {
        {1, 1000.0},
        {2,  500.0},
        {3, 1200.0}
    };

    printf("====================================================\n");
    printf(" HL Banking System - Linux Sync + Timing + Averages\n");
    printf("====================================================\n");
    printf("Initial Account Balance : £%.2f\n", account_balance);
    printf("Total Transactions      : %d\n", N_TX);

    long long program_start = now_us();

    for (int i = 0; i < N_TX; i++) {
        if (pthread_create(&th[i], NULL, process_transaction, &txs[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    for (int i = 0; i < N_TX; i++) {
        pthread_join(th[i], NULL);
    }

    long long program_end = now_us();

    // Build arrays for summary
    long long lock_wait[N_TX], cs_time[N_TX], total_time[N_TX];
    for (int i = 0; i < N_TX; i++) {
        lock_wait[i]  = metrics[i].lock_wait_us;
        cs_time[i]    = metrics[i].cs_time_us;
        total_time[i] = metrics[i].total_time_us;
    }

    long long avg_lw, min_lw, max_lw;
    long long avg_cs, min_cs, max_cs;
    long long avg_tt, min_tt, max_tt;

    stats(lock_wait,  N_TX, &avg_lw, &min_lw, &max_lw);
    stats(cs_time,    N_TX, &avg_cs, &min_cs, &max_cs);
    stats(total_time, N_TX, &avg_tt, &min_tt, &max_tt);

    printf("\n===================== SUMMARY ======================\n");
    printf("Final Account Balance   : £%.2f\n", account_balance);
    printf("Total Program Time      : %lld us\n", (program_end - program_start));
    printf("----------------------------------------------------\n");
    printf("Lock Wait Time          : avg=%lld us | min=%lld us | max=%lld us\n", avg_lw, min_lw, max_lw);
    printf("Critical Section Time   : avg=%lld us | min=%lld us | max=%lld us\n", avg_cs, min_cs, max_cs);
    printf("Total Execution Time    : avg=%lld us | min=%lld us | max=%lld us\n", avg_tt, min_tt, max_tt);
    printf("====================================================\n");

    pthread_mutex_destroy(&balance_lock);
    pthread_mutex_destroy(&print_lock);

    return 0;
}

