/**
 * ipc_benchmark.c - Performance comparison between different IPC mechanisms using io_uring
 * 
 * This benchmark compares:
 * 1. Unix Domain Socket with io_uring
 * 2. Shared Memory with eventfd notification and io_uring
 * 3. Linux-optimized implementation with futex and lock-free ring buffer
 * 
 * Compile with:
 * gcc -o ipc_benchmark ipc_benchmark.c -luring -lpthread -lrt -Wall -O2
 * 
 * Run as:
 * Server: ./ipc_benchmark --server --mode [uds|shm|lfshm]
 * Client: ./ipc_benchmark --client --mode [uds|shm|lfshm]
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
#include <sys/resource.h>
#include <sys/eventfd.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <liburing.h>
#include <sys/syscall.h>
#include <linux/futex.h>

/* For compatibility with older headers */
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#endif

#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif

/* Configuration constants */
#define SOCKET_PATH "/tmp/ipc_benchmark_socket"
#define SHM_NAME "/ipc_benchmark_shm"
#define MAX_MSG_SIZE (128 * 1024)  // 128KiB max message size
#define BUFFER_SIZE (4 * 1024 * 1024)  // 4MiB ring buffer
#define QUEUE_DEPTH 32  // io_uring queue depth
#define WARMUP_DURATION 1  // Warmup duration in seconds
#define RUN_DURATION 10     // Benchmark duration in seconds
#define MAX_LATENCIES 1000000  // Maximum number of latency measurements

/* Messaging structures */
typedef struct {
    uint32_t size;       // Message size
    uint32_t seq;        // Sequence number
    uint64_t timestamp;  // For latency calculation
    char data[0];        // Flexible array member for message payload
} Message;

/* Statistics structures */
typedef struct {
    uint64_t ops;            // Operations completed
    uint64_t bytes;          // Bytes transferred
    double cpu_usage;        // CPU usage percentage
    double avg_latency_us;   // Average latency in microseconds
    double p50_latency_us;   // 50th percentile latency
    double p95_latency_us;   // 95th percentile latency
    double p99_latency_us;   // 99th percentile latency
} BenchmarkStats;

/* Function prototypes */

// Utility functions
static inline uint64_t get_timestamp_us(void);
static int cmp_uint64(const void *a, const void *b);
void calculate_stats(uint64_t* latencies, size_t count, BenchmarkStats* stats);
void print_stats(BenchmarkStats* stats, const char* title);
double get_cpu_usage(void);
void random_message(Message* msg, int min_size, int max_size);
void* alloc_aligned_buffer(size_t size);
static inline long futex(uint32_t* uaddr, int futex_op, uint32_t val);

// Mode 1: Unix Domain Socket with io_uring
int setup_uds_server(const char* socket_path);
int setup_uds_client(const char* socket_path);
void run_uds_server(int socket_fd, int duration_secs);
void run_uds_client(int socket_fd, int duration_secs, BenchmarkStats* stats);

// Mode 2: Shared Memory with eventfd and pthread_mutex
typedef struct {
    pthread_mutex_t mutex;
    size_t read_pos;
    size_t write_pos;
    size_t size;
    char buffer[0];  // Flexible array member for actual data
} RingBuffer;

int setup_shared_memory(size_t size);
RingBuffer* map_shared_memory(int fd, size_t size);
int setup_eventfd(void);
bool ring_buffer_write(RingBuffer* rb, const void* data, size_t len);
bool ring_buffer_read(RingBuffer* rb, void* data, size_t max_len, size_t* bytes_read);
void run_shm_server(RingBuffer* rb, int eventfd_read, int eventfd_write, int duration_secs);
void run_shm_client(RingBuffer* rb, int eventfd_read, int eventfd_write, int duration_secs, BenchmarkStats* stats);

// Mode 3: Linux-optimized implementation with futex and lock-free ring buffer
typedef struct {
    atomic_size_t read_pos;
    atomic_size_t write_pos;
    size_t size;
    uint32_t server_futex; // For servers to wait on
    uint32_t client_futex; // For clients to wait on
    char buffer[0];
} LockFreeRingBuffer;

LockFreeRingBuffer* setup_lockfree_shared_memory(size_t size);
bool lfring_write(LockFreeRingBuffer* rb, const void* data, size_t len);
bool lfring_read(LockFreeRingBuffer* rb, void* data, size_t max_len, size_t* bytes_read);
void futex_wait(uint32_t* uaddr, uint32_t val);
void futex_wake(uint32_t* uaddr, int count);
void run_lfshm_server(LockFreeRingBuffer* rb, int duration_secs);
void run_lfshm_client(LockFreeRingBuffer* rb, int duration_secs, BenchmarkStats* stats);

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
                mode = MODE_LFSHM;
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
        printf("Mode: Unix Domain Socket with io_uring\n");
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
        printf("Mode: Shared Memory with eventfd and io_uring\n");
        
        // Setup shared memory and eventfds
        int shm_fd = setup_shared_memory(BUFFER_SIZE);
        RingBuffer* rb = map_shared_memory(shm_fd, BUFFER_SIZE);
        
        int client_to_server_eventfd = setup_eventfd();
        int server_to_client_eventfd = setup_eventfd();
        
        if (is_server) {
            printf("Server running...\n");
            run_shm_server(rb, client_to_server_eventfd, server_to_client_eventfd, RUN_DURATION);
        } else {
            printf("Client running...\n");
            run_shm_client(rb, server_to_client_eventfd, client_to_server_eventfd, RUN_DURATION, &stats);
            print_stats(&stats, "Shared Memory with eventfd Results");
        }
        
        // Cleanup
        munmap(rb, BUFFER_SIZE + sizeof(RingBuffer));
        close(shm_fd);
        close(client_to_server_eventfd);
        close(server_to_client_eventfd);
        
    } else if (mode == MODE_LFSHM) {
        printf("Mode: Lock-free Shared Memory with futex\n");
        
        // Setup lockfree shared memory
        LockFreeRingBuffer* rb = setup_lockfree_shared_memory(BUFFER_SIZE);
        
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
        shm_unlink(SHM_NAME);
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

/* Generate a random message with a size between min_size and max_size */
void random_message(Message* msg, int min_size, int max_size) {
    int range = max_size - min_size;
    int payload_size = min_size;
    
    if (range > 0) {
        payload_size += rand() % range;
    }
    
    msg->size = sizeof(Message) + payload_size;
    msg->seq++;
    msg->timestamp = get_timestamp_us();
    
    // Fill with random data
    for (int i = 0; i < payload_size; i++) {
        msg->data[i] = rand() % 256;
    }
}

/* Allocate buffer with proper alignment */
void* alloc_aligned_buffer(size_t size) {
    void* buf;
    if (posix_memalign(&buf, 4096, size) != 0) {
        perror("posix_memalign");
        return NULL;
    }
    return buf;
}

/* Direct futex syscall wrapper */
static inline long futex(uint32_t* uaddr, int futex_op, uint32_t val) {
    return syscall(SYS_futex, uaddr, futex_op, val, NULL, NULL, 0);
}

/*
 * Mode 1: Unix Domain Socket with io_uring
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
    struct io_uring ring;
    void* buffer = alloc_aligned_buffer(MAX_MSG_SIZE);
    
    // Initialize io_uring
    if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
        perror("io_uring_queue_init");
        free(buffer);
        return;
    }
    
    // Start the benchmark
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    
    // Submit the first read
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_recv(sqe, socket_fd, buffer, MAX_MSG_SIZE, 0);
    sqe->user_data = 1;  // Mark as read operation
    io_uring_submit(&ring);
    
    while (get_timestamp_us() < end_time) {
        struct io_uring_cqe* cqe;
        
        // Wait for completion
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            perror("io_uring_wait_cqe");
            break;
        }
        
        // Process the completion
        if (cqe->user_data == 1) {  // Read operation
            if (cqe->res <= 0) {
                // Error or connection closed
                io_uring_cqe_seen(&ring, cqe);
                break;
            }
            
            // Prepare to send the response back
            Message* msg = (Message*)buffer;
            
            // Submit the write
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_send(sqe, socket_fd, buffer, msg->size, 0);
            sqe->user_data = 2;  // Mark as write operation
            io_uring_submit(&ring);
            
        } else if (cqe->user_data == 2) {  // Write operation
            if (cqe->res <= 0) {
                // Error or connection closed
                io_uring_cqe_seen(&ring, cqe);
                break;
            }
            
            // Submit the next read
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_recv(sqe, socket_fd, buffer, MAX_MSG_SIZE, 0);
            sqe->user_data = 1;  // Mark as read operation
            io_uring_submit(&ring);
        }
        
        io_uring_cqe_seen(&ring, cqe);
    }
    
    // Cleanup
    io_uring_queue_exit(&ring);
    free(buffer);
}

/* Run the Unix Domain Socket client benchmark */
void run_uds_client(int socket_fd, int duration_secs, BenchmarkStats* stats) {
    struct io_uring ring;
    void* buffer = alloc_aligned_buffer(MAX_MSG_SIZE);
    memset(buffer, 0, MAX_MSG_SIZE);
    
    uint64_t* latencies = malloc(sizeof(uint64_t) * MAX_LATENCIES);
    size_t latency_count = 0;
    
    // Initialize io_uring
    if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
        perror("io_uring_queue_init");
        free(buffer);
        free(latencies);
        return;
    }
    
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
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_send(sqe, socket_fd, buffer, msg->size, 0);
    sqe->user_data = 2;  // Mark as write operation
    io_uring_submit(&ring);
    
    // Warmup phase
    while (get_timestamp_us() < end_warmup) {
        struct io_uring_cqe* cqe;
        
        // Wait for completion
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            perror("io_uring_wait_cqe");
            break;
        }
        
        // Process the completion
        if (cqe->user_data == 2) {  // Write operation completed
            if (cqe->res <= 0) {
                // Error or connection closed
                io_uring_cqe_seen(&ring, cqe);
                break;
            }
            
            // Submit a read to get the response
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_recv(sqe, socket_fd, buffer, MAX_MSG_SIZE, 0);
            sqe->user_data = 1;  // Mark as read operation
            io_uring_submit(&ring);
            
        } else if (cqe->user_data == 1) {  // Read operation completed
            if (cqe->res <= 0) {
                // Error or connection closed
                io_uring_cqe_seen(&ring, cqe);
                break;
            }
            
            // Send the next message
            random_message(msg, 2048, 4096);
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_send(sqe, socket_fd, buffer, msg->size, 0);
            sqe->user_data = 2;  // Mark as write operation
            io_uring_submit(&ring);
        }
        
        io_uring_cqe_seen(&ring, cqe);
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
    msg->timestamp = get_timestamp_us();  // Set the timestamp for latency calculation
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_send(sqe, socket_fd, buffer, msg->size, 0);
    sqe->user_data = 2;  // Mark as write operation
    io_uring_submit(&ring);
    
    while (get_timestamp_us() < end_time) {
        struct io_uring_cqe* cqe;
        
        // Wait for completion
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            perror("io_uring_wait_cqe");
            break;
        }
        
        // Process the completion
        if (cqe->user_data == 2) {  // Write operation completed
            if (cqe->res <= 0) {
                // Error or connection closed
                io_uring_cqe_seen(&ring, cqe);
                break;
            }
            
            // Submit a read to get the response
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_recv(sqe, socket_fd, buffer, MAX_MSG_SIZE, 0);
            sqe->user_data = 1;  // Mark as read operation
            io_uring_submit(&ring);
            
        } else if (cqe->user_data == 1) {  // Read operation completed
            if (cqe->res <= 0) {
                // Error or connection closed
                io_uring_cqe_seen(&ring, cqe);
                break;
            }
            
            // Calculate latency
            uint64_t now = get_timestamp_us();
            uint64_t latency = now - msg->timestamp;
            
            if (latency_count < MAX_LATENCIES) {
                latencies[latency_count++] = latency;
            }
            
            // Update statistics
            ops++;
            bytes += msg->size;
            
            // Send the next message
            random_message(msg, 2048, 4096);
            msg->timestamp = get_timestamp_us();  // Update timestamp for next latency calculation
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_send(sqe, socket_fd, buffer, msg->size, 0);
            sqe->user_data = 2;  // Mark as write operation
            io_uring_submit(&ring);
        }
        
        io_uring_cqe_seen(&ring, cqe);
    }
    
    // Record CPU usage
    double cpu_end = get_cpu_usage();
    double cpu_usage = (cpu_end - cpu_start) / (10000.0 * duration_secs);  // Convert to percentage
    
    // Update stats
    stats->ops = ops;
    stats->bytes = bytes;
    stats->cpu_usage = cpu_usage;
    calculate_stats(latencies, latency_count, stats);
    
    // Cleanup
    io_uring_queue_exit(&ring);
    free(buffer);
    free(latencies);
}

/*
 * Mode 2: Shared Memory with eventfd and pthread_mutex
 */

/* Create a shared memory region */
int setup_shared_memory(size_t size) {
    // Create or open the shared memory object
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open");
        return -1;
    }
    
    // Set the size of the shared memory object
    if (ftruncate(fd, sizeof(RingBuffer) + size) == -1) {
        perror("ftruncate");
        close(fd);
        shm_unlink(SHM_NAME);
        return -1;
    }
    
    return fd;
}

/* Map the shared memory region and initialize the ring buffer */
RingBuffer* map_shared_memory(int fd, size_t size) {
    // Map the shared memory object
    RingBuffer* rb = mmap(NULL, sizeof(RingBuffer) + size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (rb == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    
    // Initialize the ring buffer
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    
    pthread_mutex_init(&rb->mutex, &attr);
    rb->read_pos = 0;
    rb->write_pos = 0;
    rb->size = size;
    
    pthread_mutexattr_destroy(&attr);
    
    return rb;
}

/* Create an eventfd for notification */
int setup_eventfd(void) {
    int efd = eventfd(0, EFD_CLOEXEC);
    if (efd == -1) {
        perror("eventfd");
        return -1;
    }
    return efd;
}

/* Write a message to the ring buffer */
bool ring_buffer_write(RingBuffer* rb, const void* data, size_t len) {
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
    
    pthread_mutex_unlock(&rb->mutex);
    return true;
}

/* Read a message from the ring buffer */
bool ring_buffer_read(RingBuffer* rb, void* data, size_t max_len, size_t* bytes_read) {
    *bytes_read = 0;
    
    pthread_mutex_lock(&rb->mutex);
    
    // Check if the buffer is empty
    if (rb->read_pos == rb->write_pos) {
        pthread_mutex_unlock(&rb->mutex);
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
        pthread_mutex_unlock(&rb->mutex);
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
    
    pthread_mutex_unlock(&rb->mutex);
    return true;
}

/* Run the Shared Memory server benchmark */
void run_shm_server(RingBuffer* rb, int eventfd_read, int eventfd_write, int duration_secs) {
    struct io_uring ring;
    void* buffer = alloc_aligned_buffer(MAX_MSG_SIZE);
    uint64_t eventfd_buffer = 0;
    
    // Initialize io_uring
    if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
        perror("io_uring_queue_init");
        free(buffer);
        return;
    }
    
    // Start the benchmark
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    
    // Submit the first eventfd read
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe, eventfd_read, &eventfd_buffer, sizeof(eventfd_buffer), 0);
    sqe->user_data = 1;  // Mark as eventfd read operation
    io_uring_submit(&ring);
    
    while (get_timestamp_us() < end_time) {
        struct io_uring_cqe* cqe;
        
        // Wait for completion
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            perror("io_uring_wait_cqe");
            break;
        }
        
        // Process the completion
        if (cqe->user_data == 1) {  // eventfd read completed
            if (cqe->res <= 0) {
                // Error
                io_uring_cqe_seen(&ring, cqe);
                break;
            }
            
            // Read the message from the ring buffer
            size_t bytes_read;
            if (ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &bytes_read)) {
                // Process the message (just echo it back in this example)
                Message* msg = (Message*)buffer;
                
                // Write the response to the ring buffer
                if (ring_buffer_write(rb, buffer, msg->size)) {
                    // Notify the client
                    uint64_t val = 1;
                    sqe = io_uring_get_sqe(&ring);
                    io_uring_prep_write(sqe, eventfd_write, &val, sizeof(val), 0);
                    sqe->user_data = 2;  // Mark as eventfd write operation
                    io_uring_submit(&ring);
                }
            }
            
            // Submit another eventfd read
            eventfd_buffer = 0;  // Reset buffer
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_read(sqe, eventfd_read, &eventfd_buffer, sizeof(eventfd_buffer), 0);
            sqe->user_data = 1;  // Mark as eventfd read operation
            io_uring_submit(&ring);
            
        } else if (cqe->user_data == 2) {  // eventfd write completed
            // No special handling needed, we already submitted a new read
        }
        
        io_uring_cqe_seen(&ring, cqe);
    }
    
    // Cleanup
    io_uring_queue_exit(&ring);
    free(buffer);
}

/* Run the Shared Memory client benchmark */
void run_shm_client(RingBuffer* rb, int eventfd_read, int eventfd_write, int duration_secs, BenchmarkStats* stats) {
    struct io_uring ring;
    void* buffer = alloc_aligned_buffer(MAX_MSG_SIZE);
    uint64_t eventfd_buffer = 0;
    
    uint64_t* latencies = malloc(sizeof(uint64_t) * MAX_LATENCIES);
    size_t latency_count = 0;
    
    // Initialize io_uring
    if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
        perror("io_uring_queue_init");
        free(buffer);
        free(latencies);
        return;
    }
    
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
    if (!ring_buffer_write(rb, buffer, msg->size)) {
        fprintf(stderr, "Failed to write message to ring buffer\n");
        free(buffer);
        free(latencies);
        io_uring_queue_exit(&ring);
        return;
    }
    
    // Notify the server
    uint64_t val = 1;
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_write(sqe, eventfd_write, &val, sizeof(val), 0);
    sqe->user_data = 2;  // Mark as eventfd write operation
    io_uring_submit(&ring);
    
    // Submit the first eventfd read (for server's response)
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe, eventfd_read, &eventfd_buffer, sizeof(eventfd_buffer), 0);
    sqe->user_data = 1;  // Mark as eventfd read operation
    io_uring_submit(&ring);
    
    // Warmup phase
    while (get_timestamp_us() < end_warmup) {
        struct io_uring_cqe* cqe;
        
        // Wait for completion
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            perror("io_uring_wait_cqe");
            break;
        }
        
        // Process the completion
        if (cqe->user_data == 1) {  // eventfd read (server notified us)
            if (cqe->res <= 0) {
                // Error
                io_uring_cqe_seen(&ring, cqe);
                break;
            }
            
            // Read the response from the ring buffer
            size_t bytes_read;
            if (ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &bytes_read)) {
                // Send the next message
                random_message(msg, 2048, 4096);
                if (ring_buffer_write(rb, buffer, msg->size)) {
                    // Notify the server
                    uint64_t val = 1;
                    sqe = io_uring_get_sqe(&ring);
                    io_uring_prep_write(sqe, eventfd_write, &val, sizeof(val), 0);
                    sqe->user_data = 2;  // Mark as eventfd write operation
                    io_uring_submit(&ring);
                }
            }
            
            // Submit another eventfd read
            eventfd_buffer = 0;  // Reset buffer
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_read(sqe, eventfd_read, &eventfd_buffer, sizeof(eventfd_buffer), 0);
            sqe->user_data = 1;  // Mark as eventfd read operation
            io_uring_submit(&ring);
        }
        
        io_uring_cqe_seen(&ring, cqe);
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
    msg->timestamp = get_timestamp_us();  // Set the timestamp for latency calculation
    if (!ring_buffer_write(rb, buffer, msg->size)) {
        fprintf(stderr, "Failed to write message to ring buffer\n");
        free(buffer);
        free(latencies);
        io_uring_queue_exit(&ring);
        return;
    }
    
    // Notify the server
    val = 1;
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_write(sqe, eventfd_write, &val, sizeof(val), 0);
    sqe->user_data = 2;  // Mark as eventfd write operation
    io_uring_submit(&ring);
    
    while (get_timestamp_us() < end_time) {
        struct io_uring_cqe* cqe;
        
        // Wait for completion
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            perror("io_uring_wait_cqe");
            break;
        }
        
        // Process the completion
        if (cqe->user_data == 1) {  // eventfd read (server notified us)
            if (cqe->res <= 0) {
                // Error
                io_uring_cqe_seen(&ring, cqe);
                break;
            }
            
            // Read the response from the ring buffer
            size_t bytes_read;
            if (ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &bytes_read)) {
                // Calculate latency
                uint64_t now = get_timestamp_us();
                uint64_t latency = now - msg->timestamp;
                
                if (latency_count < MAX_LATENCIES) {
                    latencies[latency_count++] = latency;
                }
                
                // Update statistics
                ops++;
                bytes += bytes_read;
                
                // Send the next message
                random_message(msg, 2048, 4096);
                msg->timestamp = get_timestamp_us();  // Update timestamp for next latency calculation
                if (ring_buffer_write(rb, buffer, msg->size)) {
                    // Notify the server
                    uint64_t val = 1;
                    sqe = io_uring_get_sqe(&ring);
                    io_uring_prep_write(sqe, eventfd_write, &val, sizeof(val), 0);
                    sqe->user_data = 2;  // Mark as eventfd write operation
                    io_uring_submit(&ring);
                }
            }
            
            // Submit another eventfd read
            eventfd_buffer = 0;  // Reset buffer
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_read(sqe, eventfd_read, &eventfd_buffer, sizeof(eventfd_buffer), 0);
            sqe->user_data = 1;  // Mark as eventfd read operation
            io_uring_submit(&ring);
        }
        
        io_uring_cqe_seen(&ring, cqe);
    }
    
    // Record CPU usage
    double cpu_end = get_cpu_usage();
    double cpu_usage = (cpu_end - cpu_start) / (10000.0 * duration_secs);  // Convert to percentage
    
    // Update stats
    stats->ops = ops;
    stats->bytes = bytes;
    stats->cpu_usage = cpu_usage;
    calculate_stats(latencies, latency_count, stats);
    
    // Cleanup
    io_uring_queue_exit(&ring);
    free(buffer);
    free(latencies);
}

/*
 * Mode 3: Linux-optimized implementation with futex and lock-free ring buffer
 */

/* Setup a lock-free shared memory region */
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

/* Wait on a futex */
void futex_wait(uint32_t* uaddr, uint32_t val) {
    futex(uaddr, FUTEX_WAIT, val, NULL, NULL, 0);
}

/* Wake threads waiting on a futex */
void futex_wake(uint32_t* uaddr, int count) {
    futex(uaddr, FUTEX_WAKE, count, NULL, NULL, 0);
}

/* Write a message to the lock-free ring buffer */
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

/* Read a message from the lock-free ring buffer */
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

/* Run the Linux-optimized server benchmark */
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

/* Run the Linux-optimized client benchmark */
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
            // Send the next message
            random_message(msg, 2048, 4096);
            if (lfring_write(rb, buffer, msg->size)) {
                // Wake up the server
                rb->server_futex++;
                futex_wake(&rb->server_futex, 1);
            }
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
    msg->timestamp = get_timestamp_us();  // Set the timestamp for latency calculation
    if (lfring_write(rb, buffer, msg->size)) {
        // Wake up the server
        rb->server_futex++;
        futex_wake(&rb->server_futex, 1);
    }
    
    while (get_timestamp_us() < end_time) {
        // Wait for a response
        while (get_timestamp_us() < end_time) {
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
        
        if (get_timestamp_us() >= end_time) break;
        
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
            ops++;
            bytes += bytes_read;
            
            // Send the next message
            random_message(msg, 2048, 4096);
            msg->timestamp = get_timestamp_us();  // Update timestamp for next latency calculation
            if (lfring_write(rb, buffer, msg->size)) {
                // Wake up the server
                rb->server_futex++;
                futex_wake(&rb->server_futex, 1);
            }
        }
    }
    
    // Record CPU usage
    double cpu_end = get_cpu_usage();
    double cpu_usage = (cpu_end - cpu_start) / (10000.0 * duration_secs);  // Convert to percentage
    
    // Update stats
    stats->ops = ops;
    stats->bytes = bytes;
    stats->cpu_usage = cpu_usage;
    calculate_stats(latencies, latency_count, stats);
    
    // Cleanup
    free(buffer);
    free(latencies);
}
