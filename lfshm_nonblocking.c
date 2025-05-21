#include "lfshm_nonblocking.h"
#include "benchmark.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
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
static inline int futex_wait(volatile atomic_uint* uaddr, uint32_t val) {
    return syscall(SYS_futex, uaddr, FUTEX_WAIT, val, NULL, NULL, 0);
}

static inline int futex_wake(volatile atomic_uint* uaddr, int count) {
    return syscall(SYS_futex, uaddr, FUTEX_WAKE, count, NULL, NULL, 0);
}

// Wait until the given atomic_bool `*condition` becomes true. Uses `*futex_word` as a version counter to block on.
static void futex_wait_for_atomic_bool(atomic_uint *futex_word, atomic_bool *condition) {
    while (!atomic_load_explicit(condition, memory_order_acquire)) {
        uint32_t val = atomic_load_explicit(futex_word, memory_order_acquire);

        // Recheck after reading futex word
        if (!atomic_load_explicit(condition, memory_order_acquire)) {
            futex_wait(futex_word, val);
        }
    }
}

// Signal threads waiting on `*condition` by setting it to true, incrementing the futex word, and waking waiters.
//
// @parameter futex_word Atomic uint32_t futex version counter.
// @parameter condition Atomic boolean condition to set.
// @parameter count Number of threads to wake (1 for signal, INT_MAX for broadcast).
static void futex_signal_atomic_bool(atomic_uint *futex_word, atomic_bool *condition, int count) {
    atomic_store_explicit(condition, true, memory_order_release);
    atomic_fetch_add_explicit(futex_word, 1, memory_order_acq_rel);
    futex_wake(futex_word, count);  // No need for FUTEX_PRIVATE_FLAG since we're doing IPC
}

// Required for newer futex flags if not defined in your headers
#ifndef FUTEX2_32
#define FUTEX2_32 0x02
#endif

#ifndef FUTEX_BITSET_MATCH_ANY
#define FUTEX_BITSET_MATCH_ANY 0xffffffff
#endif

// Wait until the given atomic_bool `*condition` becomes true using io_uring for non-blocking operation.
// Returns true if the condition was met, false if there was an error.
static bool futex_wait_for_atomic_bool_uring(struct io_uring* ring, atomic_uint* futex_word, atomic_bool* condition) {
    while (!atomic_load_explicit(condition, memory_order_acquire)) {
        uint32_t val = atomic_load_explicit(futex_word, memory_order_acquire);

        // Recheck after reading futex word
        if (!atomic_load_explicit(condition, memory_order_acquire)) {
            // Setup futex wait using io_uring
            struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
            if (!sqe) {
                fprintf(stderr, "Failed to get SQE\n");
                return false;
            }

            // Use correctly parameterized FUTEX_WAIT operation
            io_uring_prep_futex_wait(sqe, 
                (uint32_t*)futex_word,  // Futex address
                val,                    // Expected value
                FUTEX_BITSET_MATCH_ANY, // Bitset flag - REQUIRED
                FUTEX2_32,              // Size flag - REQUIRED
                0);                     // No additional flags
            
            // Submit the futex wait request
            if (io_uring_submit(ring) < 0) {
                fprintf(stderr, "Failed to submit futex wait\n");
                return false;
            }

            // Wait for completion
            struct io_uring_cqe* cqe;
            if (io_uring_wait_cqe(ring, &cqe) < 0) {
                fprintf(stderr, "Failed to wait for futex completion\n");
                return false;
            }

            // Check if the wait was interrupted
            if (cqe->res < 0) {
                if (cqe->res == -EAGAIN) {
                    // This is expected when the value changed before we could wait
                    io_uring_cqe_seen(ring, cqe);
                    continue;
                }
                fprintf(stderr, "Futex wait failed with error: %d\n", cqe->res);
                io_uring_cqe_seen(ring, cqe);
                return false;
            }

            // Mark the completion as seen
            io_uring_cqe_seen(ring, cqe);
        }
    }
    return true;
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
        // Wait for message to become available
        futex_wait_for_atomic_bool(&rb->server_futex, &rb->message_available);

        // Read message from ring buffer
        size_t msg_size;
        bool read_success = ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &msg_size);
        
        if (read_success) {
            atomic_store_explicit(&rb->message_available, false, memory_order_release);

            Message* msg = (Message*)buffer;
            if (!validate_message(msg, msg_size)) {
                fprintf(stderr, "Server: Message validation failed\n");
            }
            
            // Simulate work
            if (work_secs > 0) {
                usleep(work_secs * 1000000);
            }
            
            // Echo the message back
            if (!ring_buffer_write(rb, buffer, msg_size)) {
                fprintf(stderr, "Server: Failed to write response to ring buffer\n");
            } else {
                // Signal client that response is available
                futex_signal_atomic_bool(&rb->client_futex, &rb->response_available, 1);
            }
        } else {
            fprintf(stderr, "Server: Failed to read message from ring buffer\n");
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
            fprintf(stderr, "Client: Failed to write message to ring buffer\n");
            break;
        }
        
        // Signal server that message is available
        futex_signal_atomic_bool(&rb->server_futex, &rb->message_available, 1);

        // Check ready
        if (!atomic_load_explicit(&rb->ready, memory_order_acquire)) {
            fprintf(stderr, "Client: Server is no longer ready, exiting\n");
            break;
        }

        // Wait for response using io_uring
        futex_wait_for_atomic_bool_uring(&ring, &rb->client_futex, &rb->response_available);
            
        // Try to read the response
        size_t msg_size;
        bool read_success = ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &msg_size);
        if (!read_success) {
            fprintf(stderr, "Client: Failed to read response from ring buffer\n");
        }
        
        atomic_store_explicit(&rb->response_available, false, memory_order_release);

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
        // Signal client that server is shutting down
        atomic_store_explicit(&rb->ready, false, memory_order_release);

        // Wake client in case it's waiting
        futex_signal_atomic_bool(&rb->client_futex, &rb->response_available, INT_MAX);
        futex_signal_atomic_bool(&rb->server_futex, &rb->message_available, INT_MAX);
        
        munmap(rb, sizeof(LockFreeNonBlockingRingBuffer) + rb->size);
        shm_unlink(SHM_NAME);
    }
}

void free_lfshm_nonblocking_client(LockFreeNonBlockingRingBuffer* rb) {
    if (rb) {
        munmap(rb, sizeof(LockFreeNonBlockingRingBuffer) + rb->size);
    }
} 
