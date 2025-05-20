#pragma once

#include "benchmark.h"
#include <stdbool.h>

struct UDSState {
    int fd;
    bool is_server;
};

typedef struct UDSState UDSState;

/* Setup Unix Domain Socket server */
UDSState* setup_uds_server(const char* socket_path);

/* Setup Unix Domain Socket client */
UDSState* setup_uds_client(const char* socket_path);

/* Free Unix Domain Socket resources */
void free_uds(UDSState* state);

/* Run the Unix Domain Socket server benchmark */
void run_uds_server(UDSState* state, int duration_secs);

/* Run the Unix Domain Socket client benchmark */
void run_uds_client(UDSState* state, int duration_secs, BenchmarkStats* stats); 