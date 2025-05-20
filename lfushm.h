#pragma once

#include "benchmark.h"
#include <stddef.h>
#include <stdbool.h>
#include <linux/futex.h>
#include <liburing.h>

struct LockFreeURingRingBuffer {
    size_t read_pos;
    size_t write_pos;
    size_t size;
    bool message_available;
    bool response_available;
    volatile bool ready;
    char buffer[0];
};

typedef struct LockFreeURingRingBuffer LockFreeURingRingBuffer;

/* Setup shared memory with io_uring futex operations */
LockFreeURingRingBuffer* setup_lockfree_uring_shared_memory(size_t size);

/* Run the Lock-free io_uring Shared Memory server benchmark */
void run_lfushm_server(LockFreeURingRingBuffer* rb, int duration_secs);

/* Run the Lock-free io_uring Shared Memory client benchmark */
void run_lfushm_client(LockFreeURingRingBuffer* rb, int duration_secs, BenchmarkStats* stats); 