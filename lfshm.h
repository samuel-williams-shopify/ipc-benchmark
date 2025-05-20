#ifndef LFSHM_H
#define LFSHM_H

#include "benchmark.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

struct LockFreeRingBuffer {
    atomic_uint read_pos;
    atomic_uint write_pos;
    size_t size;
    volatile bool ready;
    volatile uint32_t server_futex;
    volatile uint32_t client_futex;
    char buffer[0];
};
typedef struct LockFreeRingBuffer LockFreeRingBuffer;

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#define HAVE_FUTEX 1

static inline int futex_wait(volatile uint32_t* uaddr, uint32_t val) {
    return syscall(SYS_futex, uaddr, FUTEX_WAIT, val, NULL, NULL, 0);
}

static inline int futex_wake(volatile uint32_t* uaddr, int count) {
    return syscall(SYS_futex, uaddr, FUTEX_WAKE, count, NULL, NULL, 0);
}
#endif

#ifdef HAVE_FUTEX
LockFreeRingBuffer* setup_lockfree_shared_memory(size_t size);
void run_lfshm_server(LockFreeRingBuffer* rb, int duration_secs);
void run_lfshm_client(LockFreeRingBuffer* rb, int duration_secs, BenchmarkStats* stats);
#endif

#endif /* LFSHM_H */ 