#pragma once

#include "benchmark.h"
#include <stdbool.h>

struct UDSBlockingState {
    int fd;
    bool is_server;
};

typedef struct UDSBlockingState UDSBlockingState;

/* Setup Unix Domain Socket server */
UDSBlockingState* setup_uds_blocking_server(const char* socket_path);

/* Setup Unix Domain Socket client */
UDSBlockingState* setup_uds_blocking_client(const char* socket_path);

/* Free Unix Domain Socket resources */
void free_uds_blocking(UDSBlockingState* state);

/* Run the Unix Domain Socket server benchmark */
void run_uds_blocking_server(UDSBlockingState* state, int duration_secs);

/* Run the Unix Domain Socket client benchmark */
void run_uds_blocking_client(UDSBlockingState* state, int duration_secs, BenchmarkStats* stats); 