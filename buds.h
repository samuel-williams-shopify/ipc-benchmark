#pragma once

#include "benchmark.h"
#include <stdbool.h>

struct BUDSState {
    int fd;
    bool is_server;
};

typedef struct BUDSState BUDSState;

/* Setup Unix Domain Socket server */
BUDSState* setup_buds_server(const char* socket_path);

/* Setup Unix Domain Socket client */
BUDSState* setup_buds_client(const char* socket_path);

/* Free Unix Domain Socket resources */
void free_buds(BUDSState* state);

/* Run the Unix Domain Socket server benchmark */
void run_buds_server(BUDSState* state, int duration_secs);

/* Run the Unix Domain Socket client benchmark */
void run_buds_client(BUDSState* state, int duration_secs, BenchmarkStats* stats); 