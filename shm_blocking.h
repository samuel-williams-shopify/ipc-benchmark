#pragma once

#include "benchmark.h"
#include <stdbool.h>
#include <pthread.h>

struct BlockingRingBuffer {
    char* buffer;
    size_t size;
    size_t read_pos;
    size_t write_pos;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    bool is_server;
};

typedef struct BlockingRingBuffer BlockingRingBuffer;

BlockingRingBuffer* setup_shm_blocking(size_t size, bool is_server);
void free_shm_blocking(BlockingRingBuffer* rb);
void run_shm_blocking_server(BlockingRingBuffer* rb, int duration_secs);
void run_shm_blocking_client(BlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats); 