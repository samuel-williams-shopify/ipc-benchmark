#ifndef LFSHM_BLOCKING_H
#define LFSHM_BLOCKING_H

#include <stdbool.h>
#include <stdatomic.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "benchmark.h"

// Structure for lock-free blocking shared memory ring buffer
typedef struct {
    // Server-side fields (aligned to cache line)
    atomic_uint write_pos;
    atomic_uint server_futex;
    char _pad1[CACHE_LINE_SIZE - sizeof(atomic_uint) * 2];
    
    // Client-side fields (aligned to cache line)
    atomic_uint read_pos;
    atomic_uint client_futex;
    char _pad2[CACHE_LINE_SIZE - sizeof(atomic_uint) * 2];
    
    // Shared fields
    size_t size;
    atomic_bool ready;
    char buffer[0];
} __attribute__((aligned(CACHE_LINE_SIZE))) LockFreeBlockingRingBuffer;

// Function prototypes
LockFreeBlockingRingBuffer* setup_lfshm_blocking(size_t size, bool is_server);
void free_lfshm_blocking(LockFreeBlockingRingBuffer* rb);
void run_lfshm_blocking_server(LockFreeBlockingRingBuffer* rb, int duration_secs);
void run_lfshm_blocking_client(LockFreeBlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats);

#endif // LFSHM_BLOCKING_H 