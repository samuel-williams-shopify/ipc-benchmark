#pragma once

#include "benchmark.h"
#include <stdbool.h>

struct NBUDSState {
    int fd;
    bool is_server;
};

typedef struct NBUDSState NBUDSState;

NBUDSState* setup_nbuds_server(const char* socket_path);
NBUDSState* setup_nbuds_client(const char* socket_path);
void free_nbuds(NBUDSState* state);
void run_nbuds_server(NBUDSState* state, int duration_secs);
void run_nbuds_client(NBUDSState* state, int duration_secs, BenchmarkStats* stats); 