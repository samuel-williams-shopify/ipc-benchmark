#ifndef SHM_NONBLOCKING_H
#define SHM_NONBLOCKING_H

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
    bool message_processed;
    bool ready;
    bool is_server;
    char* buffer;
} NonBlockingRingBuffer;

NonBlockingRingBuffer* setup_shm_nonblocking(size_t size, bool is_server);
void free_shm_nonblocking(NonBlockingRingBuffer* rb);
void run_shm_nonblocking_server(NonBlockingRingBuffer* rb, int duration_secs);
void run_shm_nonblocking_client(NonBlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats);

#endif // SHM_NONBLOCKING_H 