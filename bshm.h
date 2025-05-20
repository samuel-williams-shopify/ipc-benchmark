#pragma once

#include "benchmark.h"
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

struct BlockingRingBuffer {
    pthread_mutex_t mutex;
    pthread_cond_t server_cond;
    pthread_cond_t client_cond;
    size_t read_pos;
    size_t write_pos;
    size_t size;
    bool message_available;
    bool response_available;
    volatile bool ready;
    char buffer[0];
};

typedef struct BlockingRingBuffer BlockingRingBuffer;

/* Setup shared memory with pthread mutex and condition variables */
BlockingRingBuffer* setup_blocking_shared_memory(size_t size, bool is_server);

/* Run the Blocking Shared Memory server benchmark */
void run_bshm_server(BlockingRingBuffer* rb, int duration_secs);

/* Run the Blocking Shared Memory client benchmark */
void run_bshm_client(BlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats); 