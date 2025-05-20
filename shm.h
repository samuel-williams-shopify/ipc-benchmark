#ifndef SHM_H
#define SHM_H

#include "benchmark.h"
#include "interrupt.h"
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

struct RingBuffer {
    pthread_mutex_t mutex;
    pthread_cond_t server_cond;
    pthread_cond_t client_cond;
    size_t read_pos;
    size_t write_pos;
    size_t size;
    bool message_available;
    bool response_available;
    volatile bool ready;
    uint64_t mutex_lock_time;
    uint64_t cond_wait_time;
    uint64_t notify_time;
    uint64_t copy_time;
    uint64_t total_ops;
    char buffer[0];
};

typedef struct RingBuffer RingBuffer;

/* Setup shared memory with pthread mutex and condition variables */
RingBuffer* setup_shared_memory(size_t size, bool is_server);

/* Run the Shared Memory server benchmark */
void run_shm_server(RingBuffer* rb, int duration_secs);

/* Run the Shared Memory client benchmark */
void run_shm_client(RingBuffer* rb, int duration_secs, BenchmarkStats* stats);

#endif /* SHM_H */ 