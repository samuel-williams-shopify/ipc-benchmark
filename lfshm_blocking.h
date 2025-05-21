#ifndef LFSHM_BLOCKING_H
#define LFSHM_BLOCKING_H

#include <stdbool.h>
#include <stdatomic.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

// Structure for lock-free blocking shared memory ring buffer
typedef struct {
    char* buffer;
    size_t size;
    atomic_size_t read_pos;
    atomic_size_t write_pos;
    atomic_uint server_futex;
    atomic_uint client_futex;
    atomic_bool ready;
    bool is_server;
} LockFreeBlockingRingBuffer;

// Function prototypes
LockFreeBlockingRingBuffer* setup_lfbshm(size_t size, bool is_server);
void free_lfbshm(LockFreeBlockingRingBuffer* rb);
void run_lfbshm_server(LockFreeBlockingRingBuffer* rb, int duration_secs);
void run_lfbshm_client(LockFreeBlockingRingBuffer* rb, int duration_secs, void* stats);

#endif // LFSHM_BLOCKING_H 