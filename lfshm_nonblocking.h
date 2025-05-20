#pragma once

#include "benchmark.h"
#include <stdbool.h>
#include <stdatomic.h>

struct LockFreeNonBlockingRingBuffer {
    char* buffer;
    size_t size;
    atomic_size_t read_pos;
    atomic_size_t write_pos;
    bool is_server;
};

typedef struct LockFreeNonBlockingRingBuffer LockFreeNonBlockingRingBuffer;

LockFreeNonBlockingRingBuffer* setup_lfshm_nonblocking(size_t size, bool is_server);
void free_lfshm_nonblocking(LockFreeNonBlockingRingBuffer* rb);
void run_lfshm_nonblocking_server(LockFreeNonBlockingRingBuffer* rb, int duration_secs);
void run_lfshm_nonblocking_client(LockFreeNonBlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats);
