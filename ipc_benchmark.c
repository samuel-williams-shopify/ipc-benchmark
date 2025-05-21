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
#include <unistd.h>

#include "interrupt.h"
#include "benchmark.h"
#include "shm_nonblocking.h"
#include "shm_blocking.h"
#ifdef __linux__
#include "lfshm_blocking.h"
#include "lfshm_nonblocking.h"
#endif
#include "uds_blocking.h"
#include "uds_nonblocking.h"

/* Function prototypes */
void print_usage(const char* prog_name);
int main(int argc, char* argv[]);

void print_usage(const char* prog_name) {
    fprintf(stderr, "Usage: %s [options]\n", prog_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -s, --server           Run as server\n");
    fprintf(stderr, "  -c, --client           Run as client\n");
    fprintf(stderr, "  -m, --mode MODE        IPC mode (uds-blocking, uds-nonblocking, shm-notification, shm-blocking, lfshm-blocking, lfshm-nonblocking)\n");
    fprintf(stderr, "  -d, --duration SECS    Benchmark duration in seconds\n");
    fprintf(stderr, "  -w, --work SECS        Server work time in seconds (default: 0)\n");
    fprintf(stderr, "  -h, --help             Show this help message\n");
}

int main(int argc, char* argv[]) {
    bool is_server = false;
    bool is_client = false;
    const char* mode = NULL;
    int duration_secs = RUN_DURATION;
    float work_secs = 0.0f;

    signal(SIGPIPE, SIG_IGN);

    static struct option long_options[] = {
        {"server", no_argument, 0, 's'},
        {"client", no_argument, 0, 'c'},
        {"mode", required_argument, 0, 'm'},
        {"duration", required_argument, 0, 'd'},
        {"work", required_argument, 0, 'w'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "scm:d:w:h", long_options, &option_index)) != -1) {
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
            case 'w':
                work_secs = atof(optarg);
                if (work_secs < 0) {
                    fprintf(stderr, "Invalid work time\n");
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

    if (strcmp(mode, "uds-blocking") == 0) {
        if (is_server) {
            UDSBlockingState* state = setup_uds_blocking_server("/tmp/uds_blocking.sock");
            if (!state) {
                fprintf(stderr, "Failed to setup Unix Domain Socket server\n");
                return 1;
            }
            run_uds_blocking_server(state, duration_secs, work_secs);
            free_uds_blocking(state);
        } else {
            UDSBlockingState* state = setup_uds_blocking_client("/tmp/uds_blocking.sock");
            if (!state) {
                fprintf(stderr, "Failed to setup Unix Domain Socket client\n");
                return 1;
            }
            BenchmarkStats stats = {0};
            run_uds_blocking_client(state, duration_secs, &stats);
            print_stats(&stats, "Blocking Unix Domain Socket");
            free_uds_blocking(state);
        }
    } else if (strcmp(mode, "uds-nonblocking") == 0) {
        if (is_server) {
            UDSNonblockingState* state = setup_uds_nonblocking_server("/tmp/uds_nonblocking.sock");
            if (!state) {
                fprintf(stderr, "Failed to setup Unix Domain Socket server\n");
                return 1;
            }
            run_uds_nonblocking_server(state, duration_secs, work_secs);
            free_uds_nonblocking(state);
        } else {
            UDSNonblockingState* state = setup_uds_nonblocking_client("/tmp/uds_nonblocking.sock");
            if (!state) {
                fprintf(stderr, "Failed to setup Unix Domain Socket client\n");
                return 1;
            }
            BenchmarkStats stats = {0};
            run_uds_nonblocking_client(state, duration_secs, &stats);
            print_stats(&stats, "Non-Blocking Unix Domain Socket");
            free_uds_nonblocking(state);
        }
    } else if (strcmp(mode, "shm-notification") == 0) {
        NonBlockingRingBuffer* rb = setup_shm_nonblocking(BUFFER_SIZE, is_server);
        if (rb == NULL) {
            fprintf(stderr, "Failed to setup Notification-based Shared Memory\n");
            return 1;
        }
        if (is_server) {
            run_shm_nonblocking_server(rb, duration_secs, work_secs);
        } else {
            BenchmarkStats stats = {0};
            run_shm_nonblocking_client(rb, duration_secs, &stats);
            print_stats(&stats, "Notification-based Shared Memory");
        }
        free_shm_nonblocking(rb);
    } else if (strcmp(mode, "shm-blocking") == 0) {
        if (is_server) {
            BlockingRingBuffer* rb = setup_shm_blocking(BUFFER_SIZE, true);
            if (!rb) {
                fprintf(stderr, "Failed to setup Blocking Shared Memory server\n");
                return 1;
            }
            run_shm_blocking_server(rb, duration_secs, work_secs);
            free_shm_blocking(rb);
        } else {
            BlockingRingBuffer* rb = setup_shm_blocking(BUFFER_SIZE, false);
            if (!rb) {
                fprintf(stderr, "Failed to setup Blocking Shared Memory client\n");
                return 1;
            }
            BenchmarkStats stats = {0};
            run_shm_blocking_client(rb, duration_secs, &stats);
            print_stats(&stats, "Blocking Shared Memory");
            free_shm_blocking(rb);
        }
    } else if (strcmp(mode, "lfshm-blocking") == 0) {
#ifdef __linux__
        if (is_server) {
            LockFreeBlockingRingBuffer* rb = setup_lfshm_blocking(BUFFER_SIZE, true);
            if (!rb) {
                fprintf(stderr, "Failed to setup Lock-free Blocking Shared Memory server\n");
                return 1;
            }
            run_lfshm_blocking_server(rb, duration_secs, work_secs);
            free_lfshm_blocking(rb);
        } else {
            LockFreeBlockingRingBuffer* rb = setup_lfshm_blocking(BUFFER_SIZE, false);
            if (!rb) {
                fprintf(stderr, "Failed to setup Lock-free Blocking Shared Memory client\n");
                return 1;
            }
            BenchmarkStats stats = {0};
            run_lfshm_blocking_client(rb, duration_secs, &stats);
            print_stats(&stats, "Lock-free Blocking Shared Memory");
            free_lfshm_blocking(rb);
        }
#else
        fprintf(stderr, "Lock-free Blocking Shared Memory is only available on Linux\n");
        return 1;
#endif
    } else if (strcmp(mode, "lfshm-nonblocking") == 0) {
#ifdef __linux__
        if (is_server) {
            LockFreeNonBlockingRingBuffer* rb = setup_lfshm_nonblocking(BUFFER_SIZE, true);
            if (!rb) {
                fprintf(stderr, "Failed to setup Lock-free Non-Blocking Shared Memory server\n");
                return 1;
            }
            run_lfshm_nonblocking_server(rb, duration_secs, work_secs);
            free_lfshm_nonblocking(rb);
        } else {
            LockFreeNonBlockingRingBuffer* rb = setup_lfshm_nonblocking(BUFFER_SIZE, false);
            if (!rb) {
                fprintf(stderr, "Failed to setup Lock-free Non-Blocking Shared Memory client\n");
                return 1;
            }
            BenchmarkStats stats = {0};
            run_lfshm_nonblocking_client(rb, duration_secs, &stats);
            print_stats(&stats, "Lock-free Non-Blocking Shared Memory");
            free_lfshm_nonblocking(rb);
        }
#else
        fprintf(stderr, "Lock-free Non-Blocking Shared Memory is only available on Linux\n");
        return 1;
#endif
    } else if (strcmp(mode, "shm-nonblocking") == 0) {
        if (is_server) {
            NonBlockingRingBuffer* rb = setup_shm_nonblocking(BUFFER_SIZE, true);
            if (!rb) {
                fprintf(stderr, "Failed to setup Non-Blocking Shared Memory server\n");
                return 1;
            }
            run_shm_nonblocking_server(rb, duration_secs, work_secs);
            free_shm_nonblocking(rb);
        } else {
            NonBlockingRingBuffer* rb = setup_shm_nonblocking(BUFFER_SIZE, false);
            if (!rb) {
                fprintf(stderr, "Failed to setup Non-Blocking Shared Memory client\n");
                return 1;
            }
            BenchmarkStats stats = {0};
            run_shm_nonblocking_client(rb, duration_secs, &stats);
            print_stats(&stats, "Non-Blocking Shared Memory");
            free_shm_nonblocking(rb);
        }
    } else {
        fprintf(stderr, "Invalid IPC mode: %s\n", mode);
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}

volatile bool should_exit = false;

void signal_handler(int signum) {
    (void)signum;
    should_exit = true;
}
