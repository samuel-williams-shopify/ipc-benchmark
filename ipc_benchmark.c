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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/resource.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "interrupt.h"
#include <getopt.h>
#include "benchmark.h"
#include "uds.h"
#include "shm.h"
#include "lfshm.h"

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#define HAVE_FUTEX 1
#endif

/* For compatibility with older headers */
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

/* Configuration constants */
#define SOCKET_PATH "/tmp/ipc_benchmark_socket"
#define SHM_NAME "/ipc_benchmark_shm"
#define MAX_MSG_SIZE (128 * 1024)  // 128KiB max message size
#define BUFFER_SIZE (4 * 1024 * 1024)  // 4MiB ring buffer
#define WARMUP_DURATION 1  // Warmup duration in seconds
#define RUN_DURATION 10     // Benchmark duration in seconds
#define MAX_LATENCIES 1000000  // Maximum number of latency measurements

#define MAGIC_START 0xDEADBEEF
#define MAGIC_END   0xCAFEBABE

/* Forward declarations of structures */
typedef struct RingBuffer RingBuffer;
typedef struct ThreadData ThreadData;
#ifdef HAVE_FUTEX
typedef struct LockFreeRingBuffer LockFreeRingBuffer;
#endif

/* Thread data structure for notification thread */
struct ThreadData {
    RingBuffer* rb;
    Interrupt* intr;
    bool is_server;
    volatile bool should_exit;
};

/* Function prototypes */
void print_usage(const char* prog_name);
int main(int argc, char* argv[]);

// Mode 1: Unix Domain Socket
void run_uds_server(int socket_fd, int duration_secs);
void run_uds_client(int socket_fd, int duration_secs, BenchmarkStats* stats);

// Mode 2: Shared Memory with pthread_mutex
void run_shm_server(RingBuffer* rb, int duration_secs);
void run_shm_client(RingBuffer* rb, int duration_secs, BenchmarkStats* stats);

#ifdef HAVE_FUTEX
// Mode 3: Linux-optimized implementation with futex
void run_lfshm_server(LockFreeRingBuffer* rb, int duration_secs);
void run_lfshm_client(LockFreeRingBuffer* rb, int duration_secs, BenchmarkStats* stats);
#endif

void print_usage(const char* prog_name) {
    fprintf(stderr, "Usage: %s [options]\n", prog_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -s, --server           Run as server\n");
    fprintf(stderr, "  -c, --client           Run as client\n");
    fprintf(stderr, "  -m, --mode MODE        IPC mode (uds, shm, lfshm)\n");
    fprintf(stderr, "  -d, --duration SECS    Benchmark duration in seconds\n");
    fprintf(stderr, "  -h, --help             Show this help message\n");
}

int main(int argc, char* argv[]) {
    bool is_server = false;
    bool is_client = false;
    const char* mode = NULL;
    int duration_secs = RUN_DURATION;

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
    } else if (strcmp(mode, "shm") == 0) {
        if (is_server) {
            RingBuffer* rb = setup_shared_memory(BUFFER_SIZE, true);
            if (!rb) {
                return 1;
            }
            run_shm_server(rb, duration_secs);
            munmap(rb, sizeof(RingBuffer) + BUFFER_SIZE);
            shm_unlink(SHM_NAME);
        } else {
            RingBuffer* rb = setup_shared_memory(BUFFER_SIZE, false);
            if (!rb) {
                return 1;
            }
            BenchmarkStats stats = {0};
            run_shm_client(rb, duration_secs, &stats);
            print_stats(&stats, "SHM Client");
            munmap(rb, sizeof(RingBuffer) + BUFFER_SIZE);
        }
    } else if (strcmp(mode, "lfshm") == 0) {
#ifdef HAVE_FUTEX
        if (is_server) {
            LockFreeRingBuffer* rb = setup_lockfree_shared_memory(BUFFER_SIZE);
            if (!rb) {
                return 1;
            }
            run_lfshm_server(rb, duration_secs);
            munmap(rb, sizeof(LockFreeRingBuffer) + BUFFER_SIZE);
            shm_unlink(SHM_NAME);
        } else {
            LockFreeRingBuffer* rb = setup_lockfree_shared_memory(BUFFER_SIZE);
            if (!rb) {
                return 1;
            }
            BenchmarkStats stats = {0};
            run_lfshm_client(rb, duration_secs, &stats);
            print_stats(&stats, "LFSHM Client");
            munmap(rb, sizeof(LockFreeRingBuffer) + BUFFER_SIZE);
        }
#else
        fprintf(stderr, "Lock-free shared memory mode is only available on Linux\n");
        return 1;
#endif
    } else {
        fprintf(stderr, "Invalid mode: %s\n", mode);
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}

#ifdef HAVE_FUTEX
// Mode 3: Linux-optimized implementation with futex
LockFreeRingBuffer* setup_lockfree_shared_memory(size_t size) {
    // Create or open the shared memory object
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open");
        return NULL;
    }
    
    // Set the size of the shared memory object
    if (ftruncate(fd, sizeof(LockFreeRingBuffer) + size) == -1) {
        perror("ftruncate");
        close(fd);
        shm_unlink(SHM_NAME);
        return NULL;
    }
    
    // Map the shared memory object
    LockFreeRingBuffer* rb = mmap(NULL, sizeof(LockFreeRingBuffer) + size, 
                                PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (rb == MAP_FAILED) {
        perror("mmap");
        close(fd);
        shm_unlink(SHM_NAME);
        return NULL;
    }
    
    // Initialize the ring buffer
    atomic_init(&rb->read_pos, 0);
    atomic_init(&rb->write_pos, 0);
    rb->size = size;
    rb->server_futex = 0;
    rb->client_futex = 0;
    
    // Close the file descriptor (memory remains mapped)
    close(fd);
    
    return rb;
}

bool lfring_write(LockFreeRingBuffer* rb, const void* data, size_t len) {
    if (len > rb->size / 2) {
        // Prevent a single message from taking more than half the buffer
        return false;
    }
    
    // Calculate positions and available space
    size_t write_pos = atomic_load(&rb->write_pos);
    size_t read_pos = atomic_load(&rb->read_pos);
    size_t available;
    
    if (write_pos >= read_pos) {
        available = rb->size - (write_pos - read_pos);
    } else {
        available = read_pos - write_pos;
    }
    
    // Check if there's enough space (reserving 1 byte to distinguish empty from full)
    if (available <= len + sizeof(uint32_t) + 1) {
        return false;
    }
    
    // Write the message length first
    uint32_t msg_len = len;
    
    if (write_pos + sizeof(uint32_t) > rb->size) {
        // Wrap around for the length
        size_t first_chunk = rb->size - write_pos;
        memcpy(rb->buffer + write_pos, &msg_len, first_chunk);
        memcpy(rb->buffer, ((char*)&msg_len) + first_chunk, sizeof(uint32_t) - first_chunk);
        write_pos = sizeof(uint32_t) - first_chunk;
    } else {
        // Write the length field
        memcpy(rb->buffer + write_pos, &msg_len, sizeof(uint32_t));
        write_pos += sizeof(uint32_t);
        if (write_pos == rb->size) {
            write_pos = 0;
        }
    }
    
    // Write the message data
    if (write_pos + len > rb->size) {
        // Wrap around for the data
        size_t first_chunk = rb->size - write_pos;
        memcpy(rb->buffer + write_pos, data, first_chunk);
        memcpy(rb->buffer, (char*)data + first_chunk, len - first_chunk);
        write_pos = len - first_chunk;
    } else {
        // Write the data
        memcpy(rb->buffer + write_pos, data, len);
        write_pos += len;
        if (write_pos == rb->size) {
            write_pos = 0;
        }
    }
    
    // Update the write position (memory barrier provided by atomic_store)
    atomic_store(&rb->write_pos, write_pos);
    
    return true;
}

bool lfring_read(LockFreeRingBuffer* rb, void* data, size_t max_len, size_t* bytes_read) {
    *bytes_read = 0;
    
    // Get current positions
    size_t read_pos = atomic_load(&rb->read_pos);
    size_t write_pos = atomic_load(&rb->write_pos);
    
    // Check if the buffer is empty
    if (read_pos == write_pos) {
        return false;
    }
    
    // Read the message length first
    uint32_t msg_len;
    
    if (read_pos + sizeof(uint32_t) > rb->size) {
        // Wrap around for the length
        size_t first_chunk = rb->size - read_pos;
        memcpy(&msg_len, rb->buffer + read_pos, first_chunk);
        memcpy(((char*)&msg_len) + first_chunk, rb->buffer, sizeof(uint32_t) - first_chunk);
        read_pos = sizeof(uint32_t) - first_chunk;
    } else {
        // Read the length field
        memcpy(&msg_len, rb->buffer + read_pos, sizeof(uint32_t));
        read_pos += sizeof(uint32_t);
        if (read_pos == rb->size) {
            read_pos = 0;
        }
    }
    
    // Check if message fits in the provided buffer
    if (msg_len > max_len) {
        return false;
    }
    
    // Read the message data
    if (read_pos + msg_len > rb->size) {
        // Wrap around for the data
        size_t first_chunk = rb->size - read_pos;
        memcpy(data, rb->buffer + read_pos, first_chunk);
        memcpy((char*)data + first_chunk, rb->buffer, msg_len - first_chunk);
        read_pos = msg_len - first_chunk;
    } else {
        // Read the data
        memcpy(data, rb->buffer + read_pos, msg_len);
        read_pos += msg_len;
        if (read_pos == rb->size) {
            read_pos = 0;
        }
    }
    
    // Update the read position (memory barrier provided by atomic_store)
    atomic_store(&rb->read_pos, read_pos);
    *bytes_read = msg_len;
    
    return true;
}

void run_lfshm_server(LockFreeRingBuffer* rb, int duration_secs) {
    void* buffer = alloc_aligned_buffer(MAX_MSG_SIZE);
    
    // Start the benchmark
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    
    while (get_timestamp_us() < end_time) {
        // Wait for a message
        while (get_timestamp_us() < end_time) {
            size_t read_pos = atomic_load(&rb->read_pos);
            size_t write_pos = atomic_load(&rb->write_pos);
            
            if (read_pos != write_pos) {
                // Message available
                break;
            }
            
            // No message, wait on futex
            uint32_t current_value = rb->server_futex;
            futex_wait(&rb->server_futex, current_value);
        }
        
        if (get_timestamp_us() >= end_time) break;
        
        // Read the message
        size_t bytes_read;
        if (lfring_read(rb, buffer, MAX_MSG_SIZE, &bytes_read)) {
            // Process the message (just echo it back in this example)
            Message* msg = (Message*)buffer;
            
            // Write the response
            if (lfring_write(rb, buffer, msg->size)) {
                // Wake up the client
                rb->client_futex++;
                futex_wake(&rb->client_futex, 1);
            }
        }
    }
    
    // Cleanup
    free(buffer);
}

void run_lfshm_client(LockFreeRingBuffer* rb, int duration_secs, BenchmarkStats* stats) {
    void* buffer = alloc_aligned_buffer(MAX_MSG_SIZE);
    
    uint64_t* latencies = malloc(sizeof(uint64_t) * MAX_LATENCIES);
    size_t latency_count = 0;
    
    // Initialize the message
    Message* msg = (Message*)buffer;
    msg->seq = 0;
    
    // Record CPU usage at start
    double cpu_start = get_cpu_usage();
    
    // Start warmup
    uint64_t start_warmup = get_timestamp_us();
    uint64_t end_warmup = start_warmup + (WARMUP_DURATION * 1000000ULL);
    
    // Send the first message (warmup)
    random_message(msg, 2048, 4096);
    if (lfring_write(rb, buffer, msg->size)) {
        // Wake up the server
        rb->server_futex++;
        futex_wake(&rb->server_futex, 1);
    }
    
    // Warmup phase
    while (get_timestamp_us() < end_warmup) {
        // Wait for a response
        while (get_timestamp_us() < end_warmup) {
            size_t read_pos = atomic_load(&rb->read_pos);
            size_t write_pos = atomic_load(&rb->write_pos);
            
            if (read_pos != write_pos) {
                // Response available
                break;
            }
            
            // No response, wait on futex
            uint32_t current_value = rb->client_futex;
            futex_wait(&rb->client_futex, current_value);
        }
        
        if (get_timestamp_us() >= end_warmup) break;
        
        // Read the response
        size_t bytes_read;
        if (lfring_read(rb, buffer, MAX_MSG_SIZE, &bytes_read)) {
            // Calculate latency
            uint64_t now = get_timestamp_us();
            uint64_t latency = now - msg->timestamp;
            
            if (latency_count < MAX_LATENCIES) {
                latencies[latency_count++] = latency;
            }
            
            // Update statistics
            stats->ops++;
            stats->bytes += bytes_read;
            
            // Send the next message
            random_message(msg, 2048, 4096);
            msg->timestamp = get_timestamp_us();
            if (lfring_write(rb, buffer, msg->size)) {
                // Wake up the server
                rb->server_futex++;
                futex_wake(&rb->server_futex, 1);
            }
        }
    }
    
    // Record CPU usage
    double cpu_end = get_cpu_usage();
    double cpu_usage = (cpu_end - cpu_start) / (10000.0 * duration_secs);
    
    // Update stats
    stats->ops++;
    stats->bytes += msg->size;
    stats->cpu_usage = cpu_usage;
    calculate_stats(latencies, latency_count, stats);
    
    // Cleanup
    free(buffer);
    free(latencies);
}
#endif