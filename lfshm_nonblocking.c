#include "lfshm_nonblocking.h"
#include "benchmark.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <liburing.h>

#define SHM_NAME "/lfshm_nonblocking_ring_buffer"

// Helper functions for futex operations
static inline int futex_wait(volatile uint32_t* uaddr, uint32_t val) {
    return syscall(SYS_futex, uaddr, FUTEX_WAIT, val, NULL, NULL, 0);
}

static inline int futex_wake(volatile uint32_t* uaddr, int count) {
    return syscall(SYS_futex, uaddr, FUTEX_WAKE, count, NULL, NULL, 0);
}

/* Setup lock-free shared memory */
LockFreeNonBlockingRingBuffer* setup_lfshm_nonblocking(size_t size, bool is_server) {
    int fd = -1;
    LockFreeNonBlockingRingBuffer* rb = NULL;
    size_t total_size = sizeof(LockFreeNonBlockingRingBuffer) + size;

    // Ensure size is power of 2 for faster modulo operations
    size = 1ULL << (64 - __builtin_clzll(size - 1));
    total_size = sizeof(LockFreeNonBlockingRingBuffer) + size;

    size_t page_size = getpagesize();
    total_size = (total_size + page_size - 1) & ~(page_size - 1);

    // Create and truncate shared memory
    if (is_server) {
        shm_unlink(SHM_NAME); // Ensure old segment is gone
        fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
        if (fd == -1) {
            perror("shm_open");
            return NULL;
        }

        if (ftruncate(fd, total_size) == -1) {
            perror("ftruncate");
            close(fd);
            shm_unlink(SHM_NAME);
            return NULL;
        }

        rb = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (rb == MAP_FAILED) {
            perror("mmap");
            close(fd);
            shm_unlink(SHM_NAME);
            return NULL;
        }

        // Initialize the ring buffer
        rb->size = size;
        atomic_store_explicit(&rb->read_pos, 0, memory_order_release);
        atomic_store_explicit(&rb->write_pos, 0, memory_order_release);
        atomic_store_explicit(&rb->server_futex, 0, memory_order_release);
        atomic_store_explicit(&rb->client_futex, 0, memory_order_release);
        atomic_store_explicit(&rb->ready, true, memory_order_release);
        atomic_store_explicit(&rb->message_available, false, memory_order_release);
        atomic_store_explicit(&rb->response_available, false, memory_order_release);
    } else {
        // Client just opens the existing segment
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (fd == -1) {
            perror("shm_open");
            return NULL;
        }

        rb = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (rb == MAP_FAILED) {
            perror("mmap");
            close(fd);
            return NULL;
        }

        // Wait for server to initialize
        while (!atomic_load_explicit(&rb->ready, memory_order_acquire)) {
            usleep(1000); // Sleep for 1ms
        }
    }

    close(fd);
    return rb;
}

/* Read a message from the lock-free ring buffer */
static bool ring_buffer_read(LockFreeNonBlockingRingBuffer* rb, void* data, size_t max_len, size_t* bytes_read) {
    *bytes_read = 0;
    
    // Check if the buffer is empty
    uint32_t read_pos = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
    uint32_t write_pos = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
    
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
    
    // Update the read position atomically
    atomic_store_explicit(&rb->read_pos, read_pos, memory_order_release);
    *bytes_read = msg_len;
    
    return true;
}

/* Write a message to the lock-free ring buffer */
static bool ring_buffer_write(LockFreeNonBlockingRingBuffer* rb, const void* data, size_t len) {
    if (len > rb->size / 2) {
        // Prevent a single message from taking more than half the buffer
        return false;
    }
    
    // Calculate available space
    uint32_t read_pos = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
    uint32_t write_pos = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
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
    
    // Update the write position atomically
    atomic_store_explicit(&rb->write_pos, write_pos, memory_order_release);
    
    return true;
}

/* Run the Lock-free Shared Memory server benchmark */
void run_lfshm_nonblocking_server(LockFreeNonBlockingRingBuffer* rb, int duration_secs, float work_secs) {
    void* buffer = malloc(MAX_MSG_SIZE);
    if (!buffer) {
        perror("malloc");
        return;
    }

    // Start the benchmark
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    
    printf("Server ready to process messages\n");
    
    while (get_timestamp_us() < end_time) {
        // Check if there's a message available
        if (!atomic_load_explicit(&rb->message_available, memory_order_acquire)) {
            // Wait for client to write
            futex_wait((volatile uint32_t*)&rb->server_futex, atomic_load_explicit(&rb->server_futex, memory_order_acquire));
            continue;
        }

        // Read message from ring buffer
        size_t msg_size;
        bool read_success = ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &msg_size);
        
        if (read_success) {
            Message* msg = (Message*)buffer;
            if (!validate_message(msg, msg_size)) {
                fprintf(stderr, "Server: Message validation failed\n");
            }
            
            // Simulate work if requested
            if (work_secs > 0.0f) {
                usleep(work_secs * 1000000);
            }
            
            // Echo the message back
            if (!ring_buffer_write(rb, buffer, msg_size)) {
                fprintf(stderr, "Failed to write response to ring buffer\n");
            } else {
                // Set response as available and message as not available
                atomic_store_explicit(&rb->response_available, true, memory_order_release);
                atomic_store_explicit(&rb->message_available, false, memory_order_release);
                
                // Wake up client if it's waiting
                futex_wake((volatile uint32_t*)&rb->client_futex, 1);
            }
        }
    }
    
    // Cleanup
    free(buffer);
}

/* Run the Lock-free Shared Memory client benchmark */
void run_lfshm_nonblocking_client(LockFreeNonBlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats) {
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

    // Setup io_uring for futex operations
    struct io_uring ring;
    if (io_uring_queue_init(32, &ring, 0) < 0) {
        perror("io_uring_queue_init");
        free(buffer);
        free(latencies);
        return;
    }

    Message* msg = (Message*)buffer;
    msg->seq = 0;

    // Start the benchmark
    stats->ops = 0;
    stats->bytes = 0;
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);

    while (get_timestamp_us() < end_time) {
        // Send message
        random_message(msg, 2048, 4096);
        msg->timestamp = get_timestamp_us();
        if (!ring_buffer_write(rb, buffer, msg->size)) {
            fprintf(stderr, "Failed to write message to ring buffer\n");
            break;
        }
        
        // Set message as available and response as not available
        atomic_store_explicit(&rb->message_available, true, memory_order_release);
        atomic_store_explicit(&rb->response_available, false, memory_order_release);
        
        // Wake up server
        futex_wake((volatile uint32_t*)&rb->server_futex, 1);

        // Wait for response
        size_t msg_size;
        bool read_success = false;
        
        while (!read_success) {
            // Check if response is available
            if (!atomic_load_explicit(&rb->response_available, memory_order_acquire)) {
                // Setup futex wait using io_uring
                struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
                if (!sqe) {
                    fprintf(stderr, "Failed to get SQE\n");
                    break;
                }

                uint32_t current_val = atomic_load_explicit(&rb->client_futex, memory_order_acquire);
                io_uring_prep_futex_wait(sqe, (uint32_t*)&rb->client_futex, current_val, FUTEX_PRIVATE_FLAG, 0, 0);

                // Submit the futex wait request
                if (io_uring_submit(&ring) < 0) {
                    fprintf(stderr, "Failed to submit futex wait\n");
                    break;
                }

                // Wait for completion
                struct io_uring_cqe* cqe;
                if (io_uring_wait_cqe(&ring, &cqe) < 0) {
                    fprintf(stderr, "Failed to wait for futex completion\n");
                    break;
                }

                // Mark the completion as seen
                io_uring_cqe_seen(&ring, cqe);
                continue;
            }
            
            // Try to read the response
            read_success = ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &msg_size);
        }

        Message* response = (Message*)buffer;
        if (!validate_message(response, msg_size)) {
            fprintf(stderr, "Client: Message validation failed\n");
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
    }

    double cpu_end = get_cpu_usage();
    stats->cpu_usage = (cpu_end - cpu_start) / (10000.0 * duration_secs);
    calculate_stats(latencies, latency_count, stats);
    
    // Cleanup io_uring
    io_uring_queue_exit(&ring);
    
    free(buffer);
    free(latencies);
}

void free_lfshm_nonblocking_server(LockFreeNonBlockingRingBuffer* rb) {
    if (rb) {
        munmap(rb, sizeof(LockFreeNonBlockingRingBuffer) + rb->size);
        shm_unlink(SHM_NAME);
    }
}

void free_lfshm_nonblocking_client(LockFreeNonBlockingRingBuffer* rb) {
    if (rb) {
        munmap(rb, sizeof(LockFreeNonBlockingRingBuffer) + rb->size);
    }
} 
