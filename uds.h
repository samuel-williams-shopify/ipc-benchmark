#pragma once

#include "benchmark.h"
#include <stddef.h>
#include <stdbool.h>

/* Setup a server-side Unix domain socket */
int setup_uds_server(const char* socket_path);

/* Setup a client-side Unix domain socket */
int setup_uds_client(const char* socket_path);

/* Run the Unix Domain Socket server benchmark */
void run_uds_server(int socket_fd, int duration_secs);

/* Run the Unix Domain Socket client benchmark */
void run_uds_client(int socket_fd, int duration_secs, BenchmarkStats* stats); 