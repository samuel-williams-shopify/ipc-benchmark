#pragma once

#include "benchmark.h"
#include <stdbool.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdatomic.h>

#define SHM_NAME "/lfbshm_ring_buffer"

// Helper functions for futex operations
static inline int futex_wait(volatile uint32_t* uaddr, uint32_t val) {
    return syscall(SYS_futex, uaddr, FUTEX_WAIT, val, NULL, NULL, 0);
}

static inline int futex_wake(volatile uint32_t* uaddr, int count) {
    return syscall(SYS_futex, uaddr, FUTEX_WAKE, count, NULL, NULL, 0);
}

struct LockFreeBlockingRingBuffer {
    char* buffer;
    size_t size;
    _Atomic size_t read_pos;
    _Atomic size_t write_pos;
    _Atomic uint32_t server_futex;
    _Atomic uint32_t client_futex;
    _Atomic bool ready;
    bool is_server;
};

typedef struct LockFreeBlockingRingBuffer LockFreeBlockingRingBuffer;

LockFreeBlockingRingBuffer* setup_lfshm_blocking(size_t size, bool is_server);
void free_lfshm_blocking(LockFreeBlockingRingBuffer* rb);
void run_lfshm_blocking_server(LockFreeBlockingRingBuffer* rb, int duration_secs);
void run_lfshm_blocking_client(LockFreeBlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats); 