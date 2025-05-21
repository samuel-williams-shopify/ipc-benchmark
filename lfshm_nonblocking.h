#pragma once

#include "benchmark.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <liburing.h>

#define SHM_NAME "/lfnbshm_ring_buffer"

// Helper functions for futex operations
static inline int futex_wait(volatile uint32_t* uaddr, uint32_t val) {
    return syscall(SYS_futex, uaddr, FUTEX_WAIT, val, NULL, NULL, 0);
}

static inline int futex_wake(volatile uint32_t* uaddr, int count) {
    return syscall(SYS_futex, uaddr, FUTEX_WAKE, count, NULL, NULL, 0);
}

struct LockFreeNonBlockingRingBuffer {
    char* buffer;
    size_t size;
    atomic_size_t read_pos;
    atomic_size_t write_pos;
    atomic_bool message_available;
    atomic_bool response_available;
    atomic_bool ready;
    atomic_uint_least32_t server_futex;
    atomic_uint_least32_t client_futex;
    bool is_server;
};

typedef struct LockFreeNonBlockingRingBuffer LockFreeNonBlockingRingBuffer;

LockFreeNonBlockingRingBuffer* setup_lfshm_nonblocking(size_t size, bool is_server);
void free_lfshm_nonblocking(LockFreeNonBlockingRingBuffer* rb);
void run_lfshm_nonblocking_server(LockFreeNonBlockingRingBuffer* rb, int duration_secs);
void run_lfshm_nonblocking_client(LockFreeNonBlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats);
