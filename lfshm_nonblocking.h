#ifndef LFSHM_NONBLOCKING_H
#define LFSHM_NONBLOCKING_H

#include <stdbool.h>
#include <stdatomic.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <liburing.h>
#include "benchmark.h"

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
} __attribute__((aligned(CACHE_LINE_SIZE))) LockFreeNonBlockingRingBuffer;

// Function prototypes
LockFreeNonBlockingRingBuffer* setup_lfshm_nonblocking(size_t size, bool is_server);
void free_lfshm_nonblocking(LockFreeNonBlockingRingBuffer* rb);
void run_lfshm_nonblocking_server(LockFreeNonBlockingRingBuffer* rb, int duration_secs);
void run_lfshm_nonblocking_client(LockFreeNonBlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats);

#endif // LFSHM_NONBLOCKING_H
