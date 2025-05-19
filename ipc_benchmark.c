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

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#define HAVE_FUTEX 1
#define HAVE_EVENTFD 1
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

/* Statistics structure */
typedef struct {
    uint64_t ops;            // Operations completed
    uint64_t bytes;          // Bytes transferred
    double cpu_usage;        // CPU usage percentage
    double avg_latency_us;   // Average latency in microseconds
    double p50_latency_us;   // 50th percentile latency
    double p95_latency_us;   // 95th percentile latency
    double p99_latency_us;   // 99th percentile latency
} BenchmarkStats;

/* Message structure */
typedef struct {
    uint32_t magic_start;  // Magic number to detect corruption
    uint32_t size;        // Message size
    uint32_t seq;         // Sequence number
    uint64_t timestamp;   // For latency calculation
    uint32_t checksum;    // Message checksum
    char data[0];         // Flexible array member for message payload
    // magic_end is written after data
} Message;

/* Shared memory ring buffer structure */
struct RingBuffer {
    pthread_mutex_t mutex;
    pthread_cond_t server_cond;
    pthread_cond_t client_cond;
    size_t read_pos;
    size_t write_pos;
    size_t size;
    bool message_available;
    bool response_available;
    volatile bool ready;
    // Add timing metrics
    uint64_t mutex_lock_time;    // Time spent in mutex locks
    uint64_t cond_wait_time;     // Time spent waiting on condition variables
    uint64_t notify_time;        // Time spent in notification handling
    uint64_t copy_time;          // Time spent copying messages
    uint64_t total_ops;          // Total number of operations
    char buffer[0];  // Flexible array member for actual data
};

/* Thread data structure */
struct ThreadData {
    RingBuffer* rb;
    int notif_fd;  // notification pipe/eventfd
    bool is_server;
    volatile bool should_exit;
};

#ifdef HAVE_FUTEX
/* Lock-free ring buffer structure */
struct LockFreeRingBuffer {
    atomic_size_t read_pos;
    atomic_size_t write_pos;
    size_t size;
    uint32_t server_futex; // For servers to wait on
    uint32_t client_futex; // For clients to wait on
    char buffer[0];
};

/* Futex functions */
static inline long futex(uint32_t* uaddr, int futex_op, uint32_t val) {
    return syscall(SYS_futex, uaddr, futex_op, val, NULL, NULL, 0);
}

void futex_wait(uint32_t* uaddr, uint32_t val) {
    futex(uaddr, FUTEX_WAIT, val);
}

void futex_wake(uint32_t* uaddr, int count) {
    futex(uaddr, FUTEX_WAKE, count);
}
#endif

/* Per-process notification pipe */
static int create_notification_pipe(int pipefd[2]) {
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return -1;
    }
    return 0;
}
#define eventfd(val, flags) -1 // Not used
#define eventfd_write(fd, val) do { uint64_t v = (val); write((fd), &v, sizeof(v)); } while(0)
#define eventfd_close(fd) close(fd)

/* Function prototypes */
static inline uint64_t get_timestamp_us(void);
static int cmp_uint64(const void *a, const void *b);
void calculate_stats(uint64_t* latencies, size_t count, BenchmarkStats* stats);
void print_stats(BenchmarkStats* stats, const char* title);
double get_cpu_usage(void);
static uint32_t calculate_checksum(const void* data, size_t len);
static bool validate_message(const Message* msg, size_t total_size);
void random_message(Message* msg, int min_size, int max_size);
void* alloc_aligned_buffer(size_t size);

// Mode 1: Unix Domain Socket
int setup_uds_server(const char* socket_path);
int setup_uds_client(const char* socket_path);
void run_uds_server(int socket_fd, int duration_secs);
void run_uds_client(int socket_fd, int duration_secs, BenchmarkStats* stats);

// Mode 2: Shared Memory with pthread_mutex
RingBuffer* setup_shared_memory(size_t size, bool is_server);
void* notification_thread(void* arg);
bool ring_buffer_read(RingBuffer* rb, void* data, size_t max_len, size_t* bytes_read);
bool ring_buffer_write_client_to_server(RingBuffer* rb, const void* data, size_t len);
bool ring_buffer_write_server_to_client(RingBuffer* rb, const void* data, size_t len);
void run_shm_server(RingBuffer* rb, int duration_secs);
void run_shm_client(RingBuffer* rb, int duration_secs, BenchmarkStats* stats);

#ifdef HAVE_FUTEX
// Mode 3: Linux-optimized implementation with futex
LockFreeRingBuffer* setup_lockfree_shared_memory(size_t size);
bool lfring_write(LockFreeRingBuffer* rb, const void* data, size_t len);
bool lfring_read(LockFreeRingBuffer* rb, void* data, size_t max_len, size_t* bytes_read);
void run_lfshm_server(LockFreeRingBuffer* rb, int duration_secs);
void run_lfshm_client(LockFreeRingBuffer* rb, int duration_secs, BenchmarkStats* stats);
#endif

/*
 * Main function - Parses command line and starts the appropriate benchmark
 */
int main(int argc, char** argv) {
    bool is_server = false;
    bool is_client = false;
    enum { MODE_UDS, MODE_SHM, MODE_LFSHM } mode = MODE_UDS;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--server") == 0) {
            is_server = true;
        } else if (strcmp(argv[i], "--client") == 0) {
            is_client = true;
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            if (strcmp(argv[i+1], "uds") == 0) {
                mode = MODE_UDS;
            } else if (strcmp(argv[i+1], "shm") == 0) {
                mode = MODE_SHM;
            } else if (strcmp(argv[i+1], "lfshm") == 0) {
                #ifdef HAVE_FUTEX
                mode = MODE_LFSHM;
                #else
                fprintf(stderr, "lfshm mode requires Linux with futex support\n");
                return 1;
                #endif
            } else {
                fprintf(stderr, "Unknown mode: %s\n", argv[i+1]);
                return 1;
            }
            i++;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    // Validate arguments
    if (!is_server && !is_client) {
        fprintf(stderr, "Please specify --server or --client\n");
        return 1;
    }
    
    if (is_server && is_client) {
        fprintf(stderr, "Cannot be both server and client\n");
        return 1;
    }

    BenchmarkStats stats = {0};

    // Run the appropriate benchmark
    if (mode == MODE_UDS) {
        printf("Mode: Unix Domain Socket\n");
        if (is_server) {
            // Remove socket if it exists
            unlink(SOCKET_PATH);
            
            // Setup server
            int socket_fd = setup_uds_server(SOCKET_PATH);
            if (socket_fd < 0) {
                return 1;
            }
            
            printf("Server running...\n");
            run_uds_server(socket_fd, RUN_DURATION);
            
            close(socket_fd);
            unlink(SOCKET_PATH);
        } else {
            // Setup client
            int socket_fd = setup_uds_client(SOCKET_PATH);
            if (socket_fd < 0) {
                return 1;
            }
            
            printf("Client running...\n");
            run_uds_client(socket_fd, RUN_DURATION, &stats);
            
            close(socket_fd);
            print_stats(&stats, "Unix Domain Socket Results");
        }
    } else if (mode == MODE_SHM) {
        printf("Mode: Shared Memory with pthread_mutex\n");
        RingBuffer* rb = setup_shared_memory(BUFFER_SIZE, is_server);
        if (!rb) {
            fprintf(stderr, "Failed to setup shared memory\n");
            return 1;
        }
        if (is_server) {
            printf("Server running...\n");
            run_shm_server(rb, RUN_DURATION);
        } else {
            printf("Client running...\n");
            run_shm_client(rb, RUN_DURATION, &stats);
            print_stats(&stats, "Shared Memory with pthread_mutex Results");
        }
        munmap(rb, BUFFER_SIZE + sizeof(RingBuffer));
        if (is_server) {
            shm_unlink(SHM_NAME);
        }
    } else if (mode == MODE_LFSHM) {
        #ifdef HAVE_FUTEX
        printf("Mode: Lock-free Shared Memory with futex\n");
        
        // Setup lockfree shared memory
        LockFreeRingBuffer* rb = setup_lockfree_shared_memory(BUFFER_SIZE);
        if (!rb) {
            fprintf(stderr, "Failed to setup lock-free shared memory\n");
            return 1;
        }
        
        if (is_server) {
            printf("Server running...\n");
            run_lfshm_server(rb, RUN_DURATION);
        } else {
            printf("Client running...\n");
            run_lfshm_client(rb, RUN_DURATION, &stats);
            print_stats(&stats, "Lock-free Shared Memory Results");
        }
        
        // Cleanup
        munmap(rb, BUFFER_SIZE + sizeof(LockFreeRingBuffer));
        if (is_server) {
            shm_unlink(SHM_NAME);
        }
        #endif
    }

    return 0;
}

/* 
 * Utility functions
 */

/* Get current timestamp in microseconds */
static inline uint64_t get_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Comparison function for qsort */
static int cmp_uint64(const void *a, const void *b) {
    return (*(uint64_t*)a > *(uint64_t*)b) - (*(uint64_t*)a < *(uint64_t*)b);
}

/* Calculate statistics from collected latency measurements */
void calculate_stats(uint64_t* latencies, size_t count, BenchmarkStats* stats) {
    if (count == 0) return;
    
    // Sort latencies for percentile calculation
    qsort(latencies, count, sizeof(uint64_t), cmp_uint64);
    
    // Calculate average
    uint64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += latencies[i];
    }
    stats->avg_latency_us = (double)sum / count;
    
    // Calculate percentiles
    stats->p50_latency_us = latencies[count * 50 / 100];
    stats->p95_latency_us = latencies[count * 95 / 100];
    stats->p99_latency_us = latencies[count * 99 / 100];
}

/* Print benchmark statistics */
void print_stats(BenchmarkStats* stats, const char* title) {
    printf("\n=== %s ===\n", title);
    printf("Operations per second: %.2f\n", (double)stats->ops / RUN_DURATION);
    printf("Throughput: %.2f MiB/s\n", (double)stats->bytes / (1024 * 1024 * RUN_DURATION));
    printf("CPU usage: %.2f%%\n", stats->cpu_usage);
    printf("Average latency: %.2f µs\n", stats->avg_latency_us);
    printf("P50 latency: %.2f µs\n", stats->p50_latency_us);
    printf("P95 latency: %.2f µs\n", stats->p95_latency_us);
    printf("P99 latency: %.2f µs\n", stats->p99_latency_us);
    printf("\n");
}

/* Get CPU usage for the current process */
double get_cpu_usage(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000.0 + 
           (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec);
}

/* Calculate checksum of message data */
static uint32_t calculate_checksum(const void* data, size_t len) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (sum << 1) | (sum >> 31);  // Rotate left by 1
        sum += bytes[i];
    }
    return sum;
}

/* Validate a message */
static bool validate_message(const Message* msg, size_t total_size) {
    if (msg->magic_start != MAGIC_START) {
        fprintf(stderr, "Message validation failed: invalid magic_start (got 0x%x, expected 0x%x)\n",
                msg->magic_start, MAGIC_START);
        return false;
    }
    
    if (msg->size != total_size) {
        fprintf(stderr, "Message validation failed: size mismatch (got %u, expected %zu)\n",
                msg->size, total_size);
        return false;
    }
    
    // Get magic_end which is stored after the data
    uint32_t magic_end;
    memcpy(&magic_end, msg->data + (total_size - sizeof(Message) - sizeof(uint32_t)), sizeof(uint32_t));
    if (magic_end != MAGIC_END) {
        fprintf(stderr, "Message validation failed: invalid magic_end (got 0x%x, expected 0x%x)\n",
                magic_end, MAGIC_END);
        return false;
    }
    
    // Calculate and verify checksum
    uint32_t calc_checksum = calculate_checksum(msg->data, total_size - sizeof(Message) - sizeof(uint32_t));
    if (calc_checksum != msg->checksum) {
        fprintf(stderr, "Message validation failed: invalid checksum (got 0x%x, expected 0x%x)\n",
                calc_checksum, msg->checksum);
        return false;
    }
    
    return true;
}

/* Fill a message with random data and validation fields */
void random_message(Message* msg, int min_size, int max_size) {
    int range = max_size - min_size;
    int payload_size = min_size;
    
    if (range > 0) {
        payload_size += rand() % range;
    }
    
    msg->magic_start = MAGIC_START;
    msg->size = sizeof(Message) + payload_size + sizeof(uint32_t); // Include magic_end
    msg->seq++;
    msg->timestamp = get_timestamp_us();
    
    // Fill with random but deterministic data based on sequence number
    uint32_t rand_state = msg->seq;
    for (int i = 0; i < payload_size; i++) {
        rand_state = rand_state * 1103515245 + 12345;
        msg->data[i] = (rand_state >> 16) & 0xFF;
    }
    
    // Calculate checksum of the data
    msg->checksum = calculate_checksum(msg->data, payload_size);
    
    // Write magic_end after the data
    uint32_t magic_end = MAGIC_END;
    memcpy(msg->data + payload_size, &magic_end, sizeof(magic_end));
}

/*
 * Mode 1: Unix Domain Socket implementation
 */

/* Setup a server-side Unix domain socket */
int setup_uds_server(const char* socket_path) {
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(server_fd);
        return -1;
    }
    
    if (listen(server_fd, 5) == -1) {
        perror("listen");
        close(server_fd);
        return -1;
    }
    
    // Accept a connection
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd == -1) {
        perror("accept");
        close(server_fd);
        return -1;
    }
    
    // We don't need the server socket anymore
    close(server_fd);
    
    return client_fd;
}

/* Setup a client-side Unix domain socket */
int setup_uds_client(const char* socket_path) {
    int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd == -1) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    
    // Try to connect, with retry
    int retries = 10;
    while (retries--) {
        if (connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            return client_fd;
        }
        
        if (errno != ENOENT && errno != ECONNREFUSED) {
            perror("connect");
            close(client_fd);
            return -1;
        }
        
        fprintf(stderr, "Waiting for server to start... (%d)\n", retries);
        sleep(1);
    }
    
    perror("connect");
    close(client_fd);
    return -1;
}

/* Run the Unix Domain Socket server benchmark */
void run_uds_server(int socket_fd, int duration_secs) {
    void* buffer = malloc(MAX_MSG_SIZE);
    if (!buffer) {
        perror("malloc");
        return;
    }

    // Start the benchmark
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    
    // Set socket to non-blocking mode
    int flags = fcntl(socket_fd, F_GETFL, 0);
    fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);

    fd_set read_fds, write_fds;
    bool waiting_to_write = false;
    size_t msg_size = 0;

    while (get_timestamp_us() < end_time) {
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 10000}; // 10ms timeout
        
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        
        if (!waiting_to_write) {
            FD_SET(socket_fd, &read_fds);
        }
        if (waiting_to_write) {
            FD_SET(socket_fd, &write_fds);
        }
        
        int ret = select(socket_fd + 1, &read_fds, &write_fds, NULL, &timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        if (FD_ISSET(socket_fd, &read_fds)) {
            ssize_t bytes_read = read(socket_fd, buffer, MAX_MSG_SIZE);
            if (bytes_read == 0) {
                // Clean shutdown - remote end closed connection
                printf("Connection closed by remote end\n");
                break;
            } else if (bytes_read < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("read");
                    break;
                }
                continue;
            }
            
            Message* msg = (Message*)buffer;
            if (!validate_message(msg, bytes_read)) {
                fprintf(stderr, "Server: Message validation failed\n");
                continue;
            }
            
            msg_size = msg->size;
            waiting_to_write = true;
        }
        
        if (FD_ISSET(socket_fd, &write_fds)) {
            ssize_t bytes_written = write(socket_fd, buffer, msg_size);
            if (bytes_written <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("write");
                    break;
                }
                continue;
            }
            waiting_to_write = false;
        }
    }
    
    free(buffer);
}

/* Run the Unix Domain Socket client benchmark */
void run_uds_client(int socket_fd, int duration_secs, BenchmarkStats* stats) {
    void* buffer = malloc(MAX_MSG_SIZE);
    if (!buffer) {
        perror("malloc");
        return;
    }

    uint64_t* latencies = malloc(sizeof(uint64_t) * MAX_LATENCIES);
    if (!latencies) {
        perror("malloc");
        free(buffer);
        return;
    }
    size_t latency_count = 0;
    
    // Initialize the message
    Message* msg = (Message*)buffer;
    msg->seq = 0;
    
    // Record CPU usage at start
    double cpu_start = get_cpu_usage();
    
    // Set socket to non-blocking mode
    int flags = fcntl(socket_fd, F_GETFL, 0);
    fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
    
    // Start warmup
    uint64_t start_warmup = get_timestamp_us();
    uint64_t end_warmup = start_warmup + (WARMUP_DURATION * 1000000ULL);
    
    // Send the first message (warmup)
    random_message(msg, 2048, 4096);
    bool waiting_for_read = false;
    bool waiting_to_write = true;
    
    fd_set read_fds, write_fds;
    
    // Warmup phase
    while (get_timestamp_us() < end_warmup) {
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 10000}; // 10ms timeout
        
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        
        if (waiting_for_read) {
            FD_SET(socket_fd, &read_fds);
        }
        if (waiting_to_write) {
            FD_SET(socket_fd, &write_fds);
        }
        
        int ret = select(socket_fd + 1, &read_fds, &write_fds, NULL, &timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        if (FD_ISSET(socket_fd, &write_fds)) {
            ssize_t bytes_written = write(socket_fd, buffer, msg->size);
            if (bytes_written <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("write");
                    break;
                }
                continue;
            }
            waiting_to_write = false;
            waiting_for_read = true;
        }
        
        if (FD_ISSET(socket_fd, &read_fds)) {
            ssize_t bytes_read = read(socket_fd, buffer, MAX_MSG_SIZE);
            if (bytes_read <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("read");
                    break;
                }
                continue;
            }
            
            // Send the next warmup message
            random_message(msg, 2048, 4096);
            waiting_to_write = true;
            waiting_for_read = false;
        }
    }
    
    printf("Warmup completed, starting benchmark...\n");
    
    // Reset statistics
    uint64_t ops = 0;
    uint64_t bytes = 0;
    
    // Start the benchmark
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    
    // Send the first message (benchmark)
    random_message(msg, 2048, 4096);
    msg->timestamp = get_timestamp_us();
    waiting_to_write = true;
    waiting_for_read = false;
    
    while (get_timestamp_us() < end_time) {
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 10000}; // 10ms timeout
        
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        
        if (waiting_for_read) {
            FD_SET(socket_fd, &read_fds);
        }
        if (waiting_to_write) {
            FD_SET(socket_fd, &write_fds);
        }
        
        int ret = select(socket_fd + 1, &read_fds, &write_fds, NULL, &timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        if (FD_ISSET(socket_fd, &write_fds)) {
            ssize_t bytes_written = write(socket_fd, buffer, msg->size);
            if (bytes_written <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("write");
                    break;
                }
                continue;
            }
            waiting_to_write = false;
            waiting_for_read = true;
        }
        
        if (FD_ISSET(socket_fd, &read_fds)) {
            ssize_t bytes_read = read(socket_fd, buffer, MAX_MSG_SIZE);
            if (bytes_read == 0) {
                // Clean shutdown - remote end closed connection
                printf("\nBenchmark completed successfully\n");
                break;
            } else if (bytes_read < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("read");
                    break;
                }
                continue;
            }
            
            Message* response = (Message*)buffer;
            if (!validate_message(response, bytes_read)) {
                fprintf(stderr, "Client: Message validation failed\n");
                continue;
            }
            
            // Calculate latency
            uint64_t now = get_timestamp_us();
            uint64_t latency = now - response->timestamp;
            
            if (latency_count < MAX_LATENCIES) {
                latencies[latency_count++] = latency;
            }
            
            // Update statistics
            ops++;
            bytes += bytes_read;
            
            // Send the next message
            random_message(msg, 2048, 4096);
            msg->timestamp = get_timestamp_us();
            waiting_to_write = true;
            waiting_for_read = false;
        }
    }
    
    // Record CPU usage
    double cpu_end = get_cpu_usage();
    double cpu_usage = (cpu_end - cpu_start) / (10000.0 * duration_secs);
    
    // Update stats
    stats->ops = ops;
    stats->bytes = bytes;
    stats->cpu_usage = cpu_usage;
    calculate_stats(latencies, latency_count, stats);
    
    // Cleanup
    free(buffer);
    free(latencies);
}

/*
 * Mode 2: Shared Memory with pthread_mutex, thread and eventfd
 */

/* Setup shared memory with pthread mutex and condition variables */
RingBuffer* setup_shared_memory(size_t size, bool is_server) {
    int fd;
    RingBuffer* rb = NULL;
    size_t total_size;
    total_size = sizeof(RingBuffer) + size;

#ifdef __APPLE__
    size_t page_size = getpagesize();
    total_size = (total_size + page_size - 1) & ~(page_size - 1);
    if (is_server) {
        // Server: create and truncate
        shm_unlink(SHM_NAME); // Ensure old segment is gone
        fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
        if (fd == -1) {
            perror("shm_open (server)");
            return NULL;
        }
        if (ftruncate(fd, total_size) == -1) {
            perror("ftruncate");
            close(fd);
            shm_unlink(SHM_NAME);
            return NULL;
        }
    } else {
        // Client: retry open until available
        int retries = 10;
        while (retries--) {
            fd = shm_open(SHM_NAME, O_RDWR, 0666);
            if (fd != -1) break;
            sleep(1);
        }
        if (fd == -1) {
            perror("shm_open (client)");
            return NULL;
        }
    }
#else
    if (is_server) {
        shm_unlink(SHM_NAME);
        fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
        if (fd == -1) {
            perror("shm_open (server)");
            return NULL;
        }
        if (ftruncate(fd, total_size) == -1) {
            perror("ftruncate");
            close(fd);
            shm_unlink(SHM_NAME);
            return NULL;
        }
    } else {
        int retries = 10;
        while (retries--) {
            fd = shm_open(SHM_NAME, O_RDWR, 0666);
            if (fd != -1) break;
            sleep(1);
        }
        if (fd == -1) {
            perror("shm_open (client)");
            return NULL;
        }
    }
#endif
    rb = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (rb == MAP_FAILED) {
        perror("mmap");
        close(fd);
        if (is_server) shm_unlink(SHM_NAME);
        return NULL;
    }
    if (is_server) {
        pthread_mutexattr_t mutex_attr;
        pthread_mutexattr_init(&mutex_attr);
        pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
        if (pthread_mutex_init(&rb->mutex, &mutex_attr) != 0) {
            perror("pthread_mutex_init");
            munmap(rb, total_size);
            close(fd);
            shm_unlink(SHM_NAME);
            return NULL;
        }
        pthread_mutexattr_destroy(&mutex_attr);
        pthread_condattr_t cond_attr;
        pthread_condattr_init(&cond_attr);
        pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
        if (pthread_cond_init(&rb->server_cond, &cond_attr) != 0 ||
            pthread_cond_init(&rb->client_cond, &cond_attr) != 0) {
            perror("pthread_cond_init");
            pthread_mutex_destroy(&rb->mutex);
            munmap(rb, total_size);
            close(fd);
            shm_unlink(SHM_NAME);
            return NULL;
        }
        pthread_condattr_destroy(&cond_attr);
        rb->read_pos = 0;
        rb->write_pos = 0;
        rb->size = size;
        rb->message_available = false;
        rb->response_available = false;
        rb->ready = true;
    }
    if (!is_server) {
        // Wait for server to set ready flag
        int wait_count = 0;
        while (!rb->ready) {
            usleep(1000); // 1ms
            if (++wait_count > 5000) { // 5 seconds max
                fprintf(stderr, "Timeout waiting for server to initialize shared memory\n");
                munmap(rb, total_size);
                close(fd);
                return NULL;
            }
        }
    }
    close(fd);
    return rb;
}

/* Thread function that waits on mutex and signals eventfd */
void* notification_thread(void* arg) {
    ThreadData* data = (ThreadData*)arg;
    RingBuffer* rb = data->rb;
    
    printf("%s thread started\n", data->is_server ? "Server" : "Client");
    
    while (!data->should_exit) {
        pthread_mutex_lock(&rb->mutex);
        
        // Server thread waits for message_available
        // Client thread waits for response_available
        if (data->is_server) {
            while (!rb->message_available && !data->should_exit) {
                pthread_cond_wait(&rb->server_cond, &rb->mutex);
            }
            
            if (rb->message_available && !data->should_exit) {
                rb->message_available = false;
                // Signal the eventfd to wake up the select() loop
                uint64_t val = 1;
                if (write(data->notif_fd, &val, sizeof(val)) != sizeof(val)) {
                    perror("write eventfd");
                }
            }
        } else {
            while (!rb->response_available && !data->should_exit) {
                pthread_cond_wait(&rb->client_cond, &rb->mutex);
            }
            
            if (rb->response_available && !data->should_exit) {
                rb->response_available = false;
                // Signal the eventfd to wake up the select() loop
                uint64_t val = 1;
                if (write(data->notif_fd, &val, sizeof(val)) != sizeof(val)) {
                    perror("write eventfd");
                }
            }
        }
        
        pthread_mutex_unlock(&rb->mutex);
    }
    
    printf("%s thread exiting\n", data->is_server ? "Server" : "Client");
    return NULL;
}

/* Read a message from the ring buffer */
bool ring_buffer_read(RingBuffer* rb, void* data, size_t max_len, size_t* bytes_read) {
    *bytes_read = 0;
    
    // We assume the mutex is already locked by the caller
    
    // Check if the buffer is empty
    if (rb->read_pos == rb->write_pos) {
        return false;
    }
    
    // Read the message length first
    uint32_t msg_len;
    size_t read_pos = rb->read_pos;
    
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
    
    // Update the read position
    rb->read_pos = read_pos;
    *bytes_read = msg_len;
    
    return true;
}

/* Write a message from client to server */
bool ring_buffer_write_client_to_server(RingBuffer* rb, const void* data, size_t len) {
    uint64_t start_time = get_timestamp_us();
    
    pthread_mutex_lock(&rb->mutex);
    uint64_t mutex_end = get_timestamp_us();
    rb->mutex_lock_time += (mutex_end - start_time);
    
    // Wait for space in the buffer
    while (rb->write_pos == rb->read_pos && rb->message_available) {
        uint64_t wait_start = get_timestamp_us();
        pthread_cond_wait(&rb->server_cond, &rb->mutex);
        uint64_t wait_end = get_timestamp_us();
        rb->cond_wait_time += (wait_end - wait_start);
    }
    
    // Write the message
    uint64_t copy_start = get_timestamp_us();
    size_t write_pos = rb->write_pos;
    
    // Write the length first
    if (write_pos + sizeof(uint32_t) > rb->size) {
        // Wrap around for the length
        size_t first_chunk = rb->size - write_pos;
        memcpy(rb->buffer + write_pos, &len, first_chunk);
        memcpy(rb->buffer, ((char*)&len) + first_chunk, sizeof(uint32_t) - first_chunk);
        write_pos = sizeof(uint32_t) - first_chunk;
    } else {
        // Write the length field
        memcpy(rb->buffer + write_pos, &len, sizeof(uint32_t));
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
    
    uint64_t copy_end = get_timestamp_us();
    rb->copy_time += (copy_end - copy_start);
    
    // Update the write position
    rb->write_pos = write_pos;
    
    // Signal that a message is available
    rb->message_available = true;
    uint64_t notify_start = get_timestamp_us();
    pthread_cond_signal(&rb->server_cond);
    uint64_t notify_end = get_timestamp_us();
    rb->notify_time += (notify_end - notify_start);
    
    pthread_mutex_unlock(&rb->mutex);
    rb->total_ops++;
    
    return true;
}

/* Write a message from server to client */
bool ring_buffer_write_server_to_client(RingBuffer* rb, const void* data, size_t len) {
    if (len > rb->size / 2) {
        // Prevent a single message from taking more than half the buffer
        return false;
    }
    
    pthread_mutex_lock(&rb->mutex);
    
    // Calculate available space
    size_t available;
    if (rb->write_pos >= rb->read_pos) {
        available = rb->size - (rb->write_pos - rb->read_pos);
    } else {
        available = rb->read_pos - rb->write_pos;
    }
    
    // Check if there's enough space (reserving 1 byte to distinguish empty from full)
    if (available <= len + sizeof(uint32_t) + 1) {
        pthread_mutex_unlock(&rb->mutex);
        return false;
    }
    
    // Write the message length first
    uint32_t msg_len = len;
    size_t write_pos = rb->write_pos;
    
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
    
    // Update the write position
    rb->write_pos = write_pos;
    
    // Signal that a response is available
    rb->response_available = true;
    pthread_cond_signal(&rb->client_cond);
    
    pthread_mutex_unlock(&rb->mutex);
    return true;
}

/* Run the Shared Memory server benchmark */
void run_shm_server(RingBuffer* rb, int duration_secs) {
    void* buffer = malloc(MAX_MSG_SIZE);
    if (!buffer) {
        perror("malloc");
        return;
    }
#ifndef HAVE_EVENTFD
    int pipefd[2];
    if (create_notification_pipe(pipefd) == -1) {
        free(buffer);
        return;
    }
    int efd = pipefd[0]; // read end for select
    int notif_write_fd = pipefd[1]; // write end for thread
#else
    int efd = eventfd(0, EFD_CLOEXEC);
    int notif_write_fd = efd;
#endif
    if (efd == -1) {
        perror("eventfd/pipe");
        free(buffer);
        return;
    }
    ThreadData thread_data = {
        .rb = rb,
        .notif_fd = notif_write_fd,
        .is_server = true,
        .should_exit = false
    };
    pthread_t notif_thread;
    if (pthread_create(&notif_thread, NULL, notification_thread, &thread_data) != 0) {
        perror("pthread_create");
        close(efd);
#ifndef HAVE_EVENTFD
        close(notif_write_fd);
#endif
        free(buffer);
        return;
    }
    // Start the benchmark
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    
    // Set up select() parameters
    fd_set read_fds;
    struct timeval timeout;
    
    printf("Server ready to process messages\n");
    
    // Initialize timing metrics
    rb->mutex_lock_time = 0;
    rb->cond_wait_time = 0;
    rb->notify_time = 0;
    rb->copy_time = 0;
    rb->total_ops = 0;
    
    while (get_timestamp_us() < end_time) {
        FD_ZERO(&read_fds);
        FD_SET(efd, &read_fds);
        
        timeout.tv_sec = 0;
        timeout.tv_usec = 10000;  // 10ms timeout
        
        int ret = select(efd + 1, &read_fds, NULL, NULL, &timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        if (FD_ISSET(efd, &read_fds)) {
            // Read the notification
            uint64_t notification = 0;
            ssize_t bytes_read = read(efd, &notification, sizeof(notification));
            if (bytes_read <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("read eventfd");
                    break;
                }
                continue;
            }
            
            // Read message from ring buffer
            size_t msg_size;
            pthread_mutex_lock(&rb->mutex);
            bool read_success = ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &msg_size);
            pthread_mutex_unlock(&rb->mutex);
            
            if (read_success) {
                Message* msg = (Message*)buffer;
                if (!validate_message(msg, msg_size)) {
                    fprintf(stderr, "Server: Message validation failed\n");
                    continue;
                }
                
                // Echo the message back
                if (!ring_buffer_write_server_to_client(rb, buffer, msg_size)) {
                    fprintf(stderr, "Failed to write response to ring buffer\n");
                }
            }
        }
    }
    
    if (get_timestamp_us() >= end_time - 100000) { // Within 100ms of end
        printf("Server shutting down...\n");
    }

    // Signal thread to exit and cleanup
    thread_data.should_exit = true;
    pthread_mutex_lock(&rb->mutex);
    pthread_cond_signal(&rb->server_cond);
    pthread_mutex_unlock(&rb->mutex);
    
    pthread_join(notif_thread, NULL);
    printf("Server notification thread joined\n");
    
    // Cleanup
#ifdef HAVE_EVENTFD
    close(efd);
#else
    close(efd);
    close(notif_write_fd);
#endif
    free(buffer);

    // Print timing metrics at the end
    printf("\nDetailed Timing Metrics:\n");
    printf("Total Operations: %lu\n", rb->total_ops);
    printf("Average Mutex Lock Time: %.2f µs\n", (double)rb->mutex_lock_time / rb->total_ops);
    printf("Average Condition Wait Time: %.2f µs\n", (double)rb->cond_wait_time / rb->total_ops);
    printf("Average Notification Time: %.2f µs\n", (double)rb->notify_time / rb->total_ops);
    printf("Average Copy Time: %.2f µs\n", (double)rb->copy_time / rb->total_ops);
    printf("Total Synchronization Overhead: %.2f µs\n", 
           (double)(rb->mutex_lock_time + rb->cond_wait_time + rb->notify_time) / rb->total_ops);
}

/* Run the Shared Memory client benchmark */
void run_shm_client(RingBuffer* rb, int duration_secs, BenchmarkStats* stats) {
    void* buffer = malloc(MAX_MSG_SIZE);
    if (!buffer) {
        perror("malloc");
        return;
    }
    uint64_t* latencies = malloc(sizeof(uint64_t) * MAX_LATENCIES);
    if (!latencies) {
        perror("malloc");
        free(buffer);
        return;
    }
    size_t latency_count = 0;
    double cpu_start = get_cpu_usage();
#ifndef HAVE_EVENTFD
    int pipefd[2];
    if (create_notification_pipe(pipefd) == -1) {
        free(buffer);
        free(latencies);
        return;
    }
    int efd = pipefd[0]; // read end for select
    int notif_write_fd = pipefd[1]; // write end for thread
#else
    int efd = eventfd(0, EFD_CLOEXEC);
    int notif_write_fd = efd;
#endif
    if (efd == -1) {
        perror("eventfd/pipe");
        free(buffer);
        free(latencies);
        return;
    }
    ThreadData thread_data = {
        .rb = rb,
        .notif_fd = notif_write_fd,
        .is_server = false,
        .should_exit = false
    };
    pthread_t notif_thread;
    if (pthread_create(&notif_thread, NULL, notification_thread, &thread_data) != 0) {
        perror("pthread_create");
        close(efd);
#ifndef HAVE_EVENTFD
        close(notif_write_fd);
#endif
        free(buffer);
        free(latencies);
        return;
    }
    // Initialize the message
    Message* msg = (Message*)buffer;
    msg->seq = 0;
    
    // Start warmup
    uint64_t start_warmup = get_timestamp_us();
    uint64_t end_warmup = start_warmup + (WARMUP_DURATION * 1000000ULL);
    uint64_t notification = 0;
    
    // Send the first message (warmup)
    random_message(msg, 2048, 4096);
    if (!ring_buffer_write_client_to_server(rb, buffer, msg->size)) {
        fprintf(stderr, "Failed to write message to ring buffer\n");
        goto cleanup;
    }
    
    // Set up select() parameters
    fd_set read_fds;
    struct timeval timeout;
    
    // Warmup phase
    while (get_timestamp_us() < end_warmup) {
        FD_ZERO(&read_fds);
        FD_SET(efd, &read_fds);
        
        timeout.tv_sec = 0;
        timeout.tv_usec = 10000;  // 10ms timeout
        
        int ret = select(efd + 1, &read_fds, NULL, NULL, &timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        if (FD_ISSET(efd, &read_fds)) {
            // Read the notification
            ssize_t bytes_read = read(efd, &notification, sizeof(notification));
            if (bytes_read <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("read eventfd");
                    break;
                }
                continue;
            }
            
            // Read response from ring buffer
            size_t msg_size;
            pthread_mutex_lock(&rb->mutex);
            bool read_success = ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &msg_size);
            pthread_mutex_unlock(&rb->mutex);
            
            if (read_success) {
                Message* response = (Message*)buffer;
                if (!validate_message(response, msg_size)) {
                    fprintf(stderr, "Client: Message validation failed\n");
                    continue;
                }
                
                // Calculate latency
                uint64_t now = get_timestamp_us();
                uint64_t latency = now - response->timestamp;
                
                if (latency_count < MAX_LATENCIES) {
                    latencies[latency_count++] = latency;
                }
                
                // Update statistics
                stats->ops++;
                stats->bytes += msg_size;
                
                // Send the next message
                random_message(msg, 2048, 4096);
                msg->timestamp = get_timestamp_us();
                if (!ring_buffer_write_client_to_server(rb, buffer, msg->size)) {
                    fprintf(stderr, "Failed to write message to ring buffer\n");
                }
            }
        }
    }
    
    printf("Warmup completed, starting benchmark...\n");
    
    // Reset statistics for benchmark phase
    stats->ops = 0;
    stats->bytes = 0;
    
    // Start the benchmark
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    
    // Send the first message (benchmark)
    random_message(msg, 2048, 4096);
    msg->timestamp = get_timestamp_us();
    if (!ring_buffer_write_client_to_server(rb, buffer, msg->size)) {
        fprintf(stderr, "Failed to write message to ring buffer\n");
        goto cleanup;
    }

    // Initialize timing metrics
    rb->mutex_lock_time = 0;
    rb->cond_wait_time = 0;
    rb->notify_time = 0;
    rb->copy_time = 0;
    rb->total_ops = 0;

    while (get_timestamp_us() < end_time) {
        FD_ZERO(&read_fds);
        FD_SET(efd, &read_fds);
        
        timeout.tv_sec = 0;
        timeout.tv_usec = 10000;  // 10ms timeout
        
        int ret = select(efd + 1, &read_fds, NULL, NULL, &timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        if (FD_ISSET(efd, &read_fds)) {
            // Read the notification
            ssize_t bytes_read = read(efd, &notification, sizeof(notification));
            if (bytes_read <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("read eventfd");
                    break;
                }
                continue;
            }
            
            // Read response from ring buffer
            size_t msg_size;
            pthread_mutex_lock(&rb->mutex);
            bool read_success = ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &msg_size);
            pthread_mutex_unlock(&rb->mutex);
            
            if (read_success) {
                Message* response = (Message*)buffer;
                if (!validate_message(response, msg_size)) {
                    fprintf(stderr, "Client: Message validation failed\n");
                    continue;
                }
                
                // Calculate latency
                uint64_t now = get_timestamp_us();
                uint64_t latency = now - response->timestamp;
                
                if (latency_count < MAX_LATENCIES) {
                    latencies[latency_count++] = latency;
                }
                
                // Update statistics
                stats->ops++;
                stats->bytes += msg_size;
                
                // Send the next message
                random_message(msg, 2048, 4096);
                msg->timestamp = get_timestamp_us();
                if (!ring_buffer_write_client_to_server(rb, buffer, msg->size)) {
                    fprintf(stderr, "Failed to write message to ring buffer\n");
                }
            }
        }
    }
    
    if (get_timestamp_us() >= end_time - 100000) { // Within 100ms of end
        printf("\nBenchmark completed successfully.\n");
    }

    // Print timing metrics at the end
    printf("\nDetailed Timing Metrics:\n");
    printf("Total Operations: %lu\n", rb->total_ops);
    printf("Average Mutex Lock Time: %.2f µs\n", (double)rb->mutex_lock_time / rb->total_ops);
    printf("Average Condition Wait Time: %.2f µs\n", (double)rb->cond_wait_time / rb->total_ops);
    printf("Average Notification Time: %.2f µs\n", (double)rb->notify_time / rb->total_ops);
    printf("Average Copy Time: %.2f µs\n", (double)rb->copy_time / rb->total_ops);
    printf("Total Synchronization Overhead: %.2f µs\n", 
           (double)(rb->mutex_lock_time + rb->cond_wait_time + rb->notify_time) / rb->total_ops);

cleanup:    
    // Record CPU usage
    double cpu_end = get_cpu_usage();
    stats->cpu_usage = (cpu_end - cpu_start) / (10000.0 * duration_secs);

    // Calculate final statistics
    calculate_stats(latencies, latency_count, stats);
    
    // Cleanup resources
#ifdef HAVE_EVENTFD
    close(efd);
#else
    close(efd);
    close(notif_write_fd);
#endif
    free(buffer);
    free(latencies);
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