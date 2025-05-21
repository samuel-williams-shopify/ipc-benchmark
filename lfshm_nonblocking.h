#ifndef LFSHM_NONBLOCKING_H
#define LFSHM_NONBLOCKING_H

#include <stdbool.h>
#include <stdatomic.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <liburing.h>

// Structure for lock-free non-blocking shared memory ring buffer
typedef struct {
    char* buffer;
    size_t size;
    atomic_size_t read_pos;
    atomic_size_t write_pos;
    atomic_bool message_available;
    atomic_bool response_available;
    atomic_uint server_futex;
    atomic_uint client_futex;
    atomic_bool ready;
    bool is_server;
} LockFreeNonBlockingRingBuffer;

// Function prototypes
LockFreeNonBlockingRingBuffer* setup_lfshm_nonblocking(size_t size, bool is_server);
void free_lfshm_nonblocking(LockFreeNonBlockingRingBuffer* rb);
void run_lfshm_nonblocking_server(LockFreeNonBlockingRingBuffer* rb, int duration_secs);
void run_lfshm_nonblocking_client(LockFreeNonBlockingRingBuffer* rb, int duration_secs, void* stats);

#endif // LFSHM_NONBLOCKING_H
