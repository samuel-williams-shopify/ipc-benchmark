#pragma once

#include "benchmark.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

#ifdef __linux__
#define HAVE_FUTEX 1
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CACHE_LINE_SIZE 64

struct LockFreeRingBuffer {
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
} __attribute__((aligned(CACHE_LINE_SIZE)));

typedef struct LockFreeRingBuffer LockFreeRingBuffer;

static inline int futex_wait(volatile uint32_t* uaddr, uint32_t val) {
    return syscall(SYS_futex, uaddr, FUTEX_WAIT, val, NULL, NULL, 0);
}

static inline int futex_wake(volatile uint32_t* uaddr, int count) {
    return syscall(SYS_futex, uaddr, FUTEX_WAKE, count, NULL, NULL, 0);
}

// Memory ordering helpers
static inline void atomic_store_release(atomic_uint* ptr, uint32_t val) {
    atomic_store_explicit(ptr, val, memory_order_release);
}

static inline uint32_t atomic_load_acquire(atomic_uint* ptr) {
    return atomic_load_explicit(ptr, memory_order_acquire);
}

static inline uint32_t atomic_fetch_add_release(atomic_uint* ptr, uint32_t val) {
    return atomic_fetch_add_explicit(ptr, val, memory_order_release);
}

// Function declarations
LockFreeRingBuffer* setup_lockfree_shared_memory(size_t size);
void run_lfshm_server(LockFreeRingBuffer* rb, int duration_secs);
void run_lfshm_client(LockFreeRingBuffer* rb, int duration_secs, BenchmarkStats* stats);

#endif /* __linux__ */ 