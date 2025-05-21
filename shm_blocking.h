#ifndef SHM_BLOCKING_H
#define SHM_BLOCKING_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "benchmark.h"

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t server_cond;
    pthread_cond_t client_cond;
    size_t read_pos;
    size_t write_pos;
    size_t size;
    bool message_available;
    bool response_available;
    bool ready;
    bool is_server;
    char* buffer;
} BlockingRingBuffer;

BlockingRingBuffer* setup_shm_blocking(size_t size, bool is_server);
void free_shm_blocking(BlockingRingBuffer* rb);
void run_shm_blocking_server(BlockingRingBuffer* rb, int duration_secs, float work_secs);
void run_shm_blocking_client(BlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats);

#endif // SHM_BLOCKING_H 