#pragma once

#include "benchmark.h"
#include <stdbool.h>
#include <pthread.h>

struct NonBlockingRingBuffer {
    char* buffer;
    size_t size;
    size_t read_pos;
    size_t write_pos;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    bool is_server;
};

typedef struct NonBlockingRingBuffer NonBlockingRingBuffer;

NonBlockingRingBuffer* setup_shm_notification(size_t size, bool is_server);
void free_shm_notification(NonBlockingRingBuffer* rb);
void run_shm_notification_server(NonBlockingRingBuffer* rb, int duration_secs);
void run_shm_notification_client(NonBlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats); 