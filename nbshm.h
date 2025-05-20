#pragma once

#include "benchmark.h"
#include "interrupt.h"
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

struct NonBlockingRingBuffer {
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

typedef struct NonBlockingRingBuffer NonBlockingRingBuffer;

/* Setup shared memory with pthread mutex and condition variables */
NonBlockingRingBuffer* setup_non_blocking_shared_memory(size_t size, bool is_server);

/* Run the Shared Memory server benchmark */
void run_nbshm_server(NonBlockingRingBuffer* rb, int duration_secs);

/* Run the Shared Memory client benchmark */
void run_nbshm_client(NonBlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats); 