#pragma once

#include "benchmark.h"
#include <stdbool.h>

typedef struct {
    int fd;
    bool is_server;
} UDSNonblockingState;

UDSNonblockingState* setup_uds_nonblocking_server(const char* socket_path);
UDSNonblockingState* setup_uds_nonblocking_client(const char* socket_path);
void free_uds_nonblocking(UDSNonblockingState* state);
void run_uds_nonblocking_server(UDSNonblockingState* state, int duration_secs, float work_secs);
void run_uds_nonblocking_client(UDSNonblockingState* state, int duration_secs, BenchmarkStats* stats); 