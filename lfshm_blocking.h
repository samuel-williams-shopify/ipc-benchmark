#pragma once

#include "benchmark.h"
#include <stdbool.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

struct LockFreeBlockingRingBuffer {
    char* buffer;
    size_t size;
    size_t read_pos;
    size_t write_pos;
    int futex;
    bool is_server;
};

typedef struct LockFreeBlockingRingBuffer LockFreeBlockingRingBuffer;

LockFreeBlockingRingBuffer* setup_lfshm_blocking(size_t size, bool is_server);
void free_lfshm_blocking(LockFreeBlockingRingBuffer* rb);
void run_lfshm_blocking_server(LockFreeBlockingRingBuffer* rb, int duration_secs);
void run_lfshm_blocking_client(LockFreeBlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats); 