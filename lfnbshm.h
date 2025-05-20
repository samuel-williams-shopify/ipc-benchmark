#pragma once

#include "benchmark.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __linux__
#include <linux/futex.h>
#include <liburing.h>

struct LockFreeNonBlockingRingBuffer {
    size_t read_pos;
    size_t write_pos;
    size_t size;
    bool message_available;
    bool response_available;
    volatile bool ready;
    char buffer[0];
};

typedef struct LockFreeNonBlockingRingBuffer LockFreeNonBlockingRingBuffer;

/* Setup shared memory with io_uring futex operations */
LockFreeNonBlockingRingBuffer* setup_lock_free_non_blocking_shared_memory(size_t size);

/* Run the Lock-free io_uring Shared Memory server benchmark */
void run_lfnbshm_server(LockFreeNonBlockingRingBuffer* rb, int duration_secs);

/* Run the Lock-free io_uring Shared Memory client benchmark */
void run_lfnbshm_client(LockFreeNonBlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats); 

#endif /* __linux__ */
