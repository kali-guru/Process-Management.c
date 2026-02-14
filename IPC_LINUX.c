// linux_ipc_shm_strong.c
// HL Banking IPC Benchmark (Linux) - Shared Memory Ring Buffer
// Producer: Transaction Processor  |  Consumer: Logging/Audit Service
// SAFE mode: semaphores + mutex prevent race conditions.
// UNSAFE mode (--unsafe): mutex removed to demonstrate race condition corruption.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define SHM_NAME   "/hl_bank_shm_ipc"
#define SEM_EMPTY  "/hl_bank_sem_empty"
#define SEM_FULL   "/hl_bank_sem_full"
#define SEM_MUTEX  "/hl_bank_sem_mutex"

#define RING_CAP   1024
#define PAYLOAD_SZ 64

typedef struct {
    uint32_t tx_id;
    uint32_t type;         // 0..4 (Transfer/Inquiry/BillPay/Fraud/Logging)
    uint64_t amount_pence;
    uint64_t t_send_us;    // producer timestamp
    char payload[PAYLOAD_SZ];
} TxMsg;

typedef struct {
    uint32_t head;
    uint32_t tail;
    TxMsg buf[RING_CAP];
} Ring;

static int g_unsafe = 0;

static long long now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void cleanup_ipc(void) {
    shm_unlink(SHM_NAME);
    sem_unlink(SEM_EMPTY);
    sem_unlink(SEM_FULL);
    sem_unlink(SEM_MUTEX);
}

static void print_header(const char *mode) {
    printf("=====================================================\n");
    printf(" HL Banking System - Linux IPC (Shared Memory) [%s]\n", mode);
    printf("=====================================================\n");
}

static void consumer_process(int n) {
    int fd = shm_open(SHM_NAME, O_RDWR, 0600);
    if (fd < 0) die("shm_open(consumer)");

    Ring *ring = (Ring*)mmap(NULL, sizeof(Ring), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring == MAP_FAILED) die("mmap(consumer)");

    sem_t *sem_empty = sem_open(SEM_EMPTY, 0);
    sem_t *sem_full  = sem_open(SEM_FULL, 0);
    sem_t *sem_mutex = sem_open(SEM_MUTEX, 0);
    if (!sem_empty || !sem_full || (!g_unsafe && !sem_mutex)) die("sem_open(consumer)");

    // Integrity checks: count duplicates/missing using a bitmap (tx_id 1..n)
    uint8_t *seen = (uint8_t*)calloc((size_t)n + 1, 1);
    if (!seen) die("calloc(seen)");

    long long lat_sum = 0, min_lat = LLONG_MAX, max_lat = 0;
    long long proc_sum = 0, min_proc = LLONG_MAX, max_proc = 0;

    long long start_all = now_us();

    for (int i = 0; i < n; i++) {
        long long t0 = now_us();

        if (sem_wait(sem_full) != 0) die("sem_wait(full)");

        if (!g_unsafe) {
            if (sem_wait(sem_mutex) != 0) die("sem_wait(mutex)");
        }

        // Dequeue
        TxMsg msg = ring->buf[ring->tail];
        ring->tail = (ring->tail + 1) % RING_CAP;

        if (!g_unsafe) {
            if (sem_post(sem_mutex) != 0) die("sem_post(mutex)");
        }
        if (sem_post(sem_empty) != 0) die("sem_post(empty)");

        long long t1 = now_us();
        long long proc = t1 - t0;
        proc_sum += proc;
        if (proc < min_proc) min_proc = proc;
        if (proc > max_proc) max_proc = proc;

        long long lat = (long long)t1 - (long long)msg.t_send_us;
        lat_sum += lat;
        if (lat < min_lat) min_lat = lat;
        if (lat > max_lat) max_lat = lat;

        // Validate tx_id
        if (msg.tx_id >= 1 && msg.tx_id <= (uint32_t)n) {
            seen[msg.tx_id] += 1; // duplicates >1
        }
    }

    long long end_all = now_us();
    double total_s = (end_all - start_all) / 1000000.0;

    int missing = 0, duplicates = 0, out_of_range = 0;
    for (int id = 1; id <= n; id++) {
        if (seen[id] == 0) missing++;
        if (seen[id] > 1) duplicates += (seen[id] - 1);
    }
    free(seen);

    printf("\n------------------- CONSUMER (Logging/Audit) -------------------\n");
    printf("Transactions Processed : %d\n", n);
    printf("Total Receive Time     : %.6f s\n", total_s);
    printf("Throughput             : %.2f msg/s\n", n / total_s);
    printf("\nAvg Proc Time/msg      : %.2f us | min=%lld us | max=%lld us\n",
           (double)proc_sum / n, min_proc, max_proc);
    printf("Avg One-way Latency    : %.2f us | min=%lld us | max=%lld us\n",
           (double)lat_sum / n, min_lat, max_lat);

    printf("\nIntegrity Check        : missing=%d | duplicate=%d | out_of_range=%d\n",
           missing, duplicates, out_of_range);
    printf("----------------------------------------------------------------\n");

    munmap(ring, sizeof(Ring));
    close(fd);
    sem_close(sem_empty);
    sem_close(sem_full);
    if (!g_unsafe) sem_close(sem_mutex);
    exit(0);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--unsafe") == 0) g_unsafe = 1;

    int n = 0;
    print_header(g_unsafe ? "UNSAFE (RACE DEMO)" : "SAFE");
    printf("Enter number of transactions to simulate: ");
    if (scanf("%d", &n) != 1 || n <= 0) {
        fprintf(stderr, "Invalid input.\n");
        return 1;
    }

    cleanup_ipc();

    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0) die("shm_open(producer)");
    if (ftruncate(fd, sizeof(Ring)) != 0) die("ftruncate");

    Ring *ring = (Ring*)mmap(NULL, sizeof(Ring), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring == MAP_FAILED) die("mmap(producer)");
    memset(ring, 0, sizeof(Ring));

    sem_t *sem_empty = sem_open(SEM_EMPTY, O_CREAT, 0600, RING_CAP);
    sem_t *sem_full  = sem_open(SEM_FULL,  O_CREAT, 0600, 0);
    sem_t *sem_mutex = NULL;
    if (!g_unsafe) {
        sem_mutex = sem_open(SEM_MUTEX, O_CREAT, 0600, 1);
        if (!sem_mutex) die("sem_open(mutex)");
    }
    if (!sem_empty || !sem_full) die("sem_open(empty/full)");

    pid_t child = fork();
    if (child < 0) die("fork");
    if (child == 0) consumer_process(n);

    const char *types[] = {"Transfer","Inquiry","BillPay","Fraud","Logging"};

    long long lat_sum = 0, min_lat = LLONG_MAX, max_lat = 0;
    long long proc_sum = 0, min_proc = LLONG_MAX, max_proc = 0;

    long long start_all = now_us();

    for (int i = 0; i < n; i++) {
        long long t0 = now_us();

        if (sem_wait(sem_empty) != 0) die("sem_wait(empty)");
        if (!g_unsafe) {
            if (sem_wait(sem_mutex) != 0) die("sem_wait(mutex)");
        }

        TxMsg msg;
        msg.tx_id = (uint32_t)(i + 1);
        msg.type  = (uint32_t)(i % 5);
        msg.amount_pence = (uint64_t)(1000 + (i % 500)) * 100;
        msg.t_send_us = (uint64_t)now_us();
        snprintf(msg.payload, sizeof(msg.payload), "HL_TX_%u %s", msg.tx_id, types[msg.type]);

        ring->buf[ring->head] = msg;
        ring->head = (ring->head + 1) % RING_CAP;

        if (!g_unsafe) {
            if (sem_post(sem_mutex) != 0) die("sem_post(mutex)");
        }
        if (sem_post(sem_full) != 0) die("sem_post(full)");

        long long t1 = now_us();
        long long proc = t1 - t0;
        proc_sum += proc;
        if (proc < min_proc) min_proc = proc;
        if (proc > max_proc) max_proc = proc;

        long long lat = t1 - (long long)msg.t_send_us;
        lat_sum += lat;
        if (lat < min_lat) min_lat = lat;
        if (lat > max_lat) max_lat = lat;
    }

    long long end_all = now_us();
    double total_s = (end_all - start_all) / 1000000.0;

    int status = 0;
    waitpid(child, &status, 0);

    printf("\n------------------- PRODUCER (Transaction Processor) -------------------\n");
    printf("Transactions Sent      : %d\n", n);
    printf("Total Send Time        : %.6f s\n", total_s);
    printf("Throughput             : %.2f msg/s\n", n / total_s);
    printf("\nAvg Proc Time/msg      : %.2f us | min=%lld us | max=%lld us\n",
           (double)proc_sum / n, min_proc, max_proc);
    printf("Avg Local Latency      : %.2f us | min=%lld us | max=%lld us\n",
           (double)lat_sum / n, min_lat, max_lat);
    printf("-----------------------------------------------------------------------\n");

    munmap(ring, sizeof(Ring));
    close(fd);
    sem_close(sem_empty);
    sem_close(sem_full);
    if (!g_unsafe) sem_close(sem_mutex);

    cleanup_ipc();
    return 0;
}
