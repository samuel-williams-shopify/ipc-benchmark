/**
 * ipc_benchmark.c - Performance comparison between different IPC mechanisms
 * 
 * This benchmark compares:
 * 1. Unix Domain Socket with select()
 * 2. Shared Memory with pthread_mutex and dedicated thread
 * 3. Linux-optimized implementation with futex and lock-free ring buffer
 * 
 * Compile with:
 * gcc -o ipc_benchmark ipc_benchmark.c -lpthread -lrt -Wall -O2
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "interrupt.h"
#include "benchmark.h"
#include "uds.h"
#include "nbshm.h"
#include "bshm.h"
#ifdef __linux__
#include "lfbshm.h"
#include "lfnbshm.h"
#endif

/* Function prototypes */
void print_usage(const char* prog_name);
int main(int argc, char* argv[]);

void print_usage(const char* prog_name) {
    fprintf(stderr, "Usage: %s [options]\n", prog_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -s, --server           Run as server\n");
    fprintf(stderr, "  -c, --client           Run as client\n");
    fprintf(stderr, "  -m, --mode MODE        IPC mode (uds, nbshm, bshm, lfbshm, lfnbshm)\n");
    fprintf(stderr, "  -d, --duration SECS    Benchmark duration in seconds\n");
    fprintf(stderr, "  -h, --help             Show this help message\n");
}

int main(int argc, char* argv[]) {
    bool is_server = false;
    bool is_client = false;
    const char* mode = NULL;
    int duration_secs = RUN_DURATION;

    signal(SIGPIPE, SIG_IGN);

    static struct option long_options[] = {
        {"server", no_argument, 0, 's'},
        {"client", no_argument, 0, 'c'},
        {"mode", required_argument, 0, 'm'},
        {"duration", required_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "scm:d:h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 's':
                is_server = true;
                break;
            case 'c':
                is_client = true;
                break;
            case 'm':
                mode = optarg;
                break;
            case 'd':
                duration_secs = atoi(optarg);
                if (duration_secs <= 0) {
                    fprintf(stderr, "Invalid duration\n");
                    return 1;
                }
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (!is_server && !is_client) {
        fprintf(stderr, "Must specify either --server or --client\n");
        print_usage(argv[0]);
        return 1;
    }

    if (is_server && is_client) {
        fprintf(stderr, "Cannot specify both --server and --client\n");
        print_usage(argv[0]);
        return 1;
    }

    if (!mode) {
        fprintf(stderr, "Must specify --mode\n");
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(mode, "uds") == 0) {
        if (is_server) {
            int sock_fd = setup_uds_server(SOCKET_PATH);
            if (sock_fd == -1) {
                return 1;
            }
            run_uds_server(sock_fd, duration_secs);
            close(sock_fd);
        } else {
            int sock_fd = setup_uds_client(SOCKET_PATH);
            if (sock_fd == -1) {
                return 1;
            }
            BenchmarkStats stats = {0};
            run_uds_client(sock_fd, duration_secs, &stats);
            print_stats(&stats, "UDS Client");
            close(sock_fd);
        }
    } else if (strcmp(mode, "nbshm") == 0) {
        NonBlockingRingBuffer* rb = setup_non_blocking_shared_memory(BUFFER_SIZE, is_server);
        if (rb == NULL) {
            fprintf(stderr, "Failed to setup shared memory\n");
            return 1;
        }
        if (is_server) {
            run_nbshm_server(rb, duration_secs);
        } else {
            BenchmarkStats stats = {0};
            run_nbshm_client(rb, duration_secs, &stats);
            print_stats(&stats, "NBSHM Client");
        }
        // Cleanup is handled by the server
    } else if (strcmp(mode, "bshm") == 0) {
        if (is_server) {
            BlockingRingBuffer* rb = setup_blocking_shared_memory(BUFFER_SIZE, true);
            if (!rb) {
                return 1;
            }
            run_bshm_server(rb, duration_secs);
        } else {
            BlockingRingBuffer* rb = setup_blocking_shared_memory(BUFFER_SIZE, false);
            if (!rb) {
                return 1;
            }
            BenchmarkStats stats = {0};
            run_bshm_client(rb, duration_secs, &stats);
            print_stats(&stats, "BSHM Client");
        }
    } else if (strcmp(mode, "lfbshm") == 0) {
#ifdef HAVE_FUTEX
        if (is_server) {
            LockFreeBlockingRingBuffer* rb = setup_lock_free_blocking_shared_memory(BUFFER_SIZE);
            if (!rb) {
                return 1;
            }
            run_lfbshm_server(rb, duration_secs);
        } else {
            LockFreeBlockingRingBuffer* rb = setup_lock_free_blocking_shared_memory(BUFFER_SIZE);
            if (!rb) {
                return 1;
            }
            BenchmarkStats stats = {0};
            run_lfbshm_client(rb, duration_secs, &stats);
            print_stats(&stats, "LFBSHM Client");
        }
#else
        fprintf(stderr, "Lock-free shared memory mode is only available on Linux\n");
        return 1;
#endif
    } else if (strcmp(mode, "lfnbshm") == 0) {
#ifdef __linux__
        if (is_server) {
            LockFreeBlockingRingBuffer* rb = setup_lock_free_blocking_shared_memory(BUFFER_SIZE);
            if (!rb) {
                return 1;
            }
            run_lfnbshm_server(rb, duration_secs);
        } else {
            LockFreeBlockingRingBuffer* rb = setup_lock_free_blocking_shared_memory(BUFFER_SIZE);
            if (!rb) {
                return 1;
            }
            BenchmarkStats stats = {0};
            run_lfnbshm_client(rb, duration_secs, &stats);
            print_stats(&stats, "LFNBSHM Client");
        }
#else
        fprintf(stderr, "Lock-free io_uring shared memory mode is only available on Linux\n");
        return 1;
#endif
    } else {
        fprintf(stderr, "Invalid mode: %s\n", mode);
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}
