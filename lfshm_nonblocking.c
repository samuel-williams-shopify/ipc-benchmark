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
#include <sys/syscall.h>
#include <linux/futex.h>

/* Setup shared memory with io_uring futex operations */
LockFreeNonBlockingRingBuffer* setup_lfnbshm(size_t size, bool is_server) {
    int fd = -1;
    LockFreeNonBlockingRingBuffer* rb = NULL;
    size_t total_size = sizeof(LockFreeNonBlockingRingBuffer) + size;

    // Create and truncate shared memory
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
    atomic_store(&rb->read_pos, 0);
    atomic_store(&rb->write_pos, 0);
    atomic_store(&rb->message_available, false);
    atomic_store(&rb->response_available, false);
    rb->size = size;
    atomic_store(&rb->ready, is_server);
    atomic_store(&rb->server_futex, 0);
    atomic_store(&rb->client_futex, 0);
    rb->is_server = is_server;

    close(fd);
    return rb;
}

/* Read a message from the ring buffer */
static bool ring_buffer_read(LockFreeNonBlockingRingBuffer* rb, void* data, size_t max_len, size_t* bytes_read) {
    *bytes_read = 0;
    
    // Check if the buffer is empty
    if (atomic_load(&rb->read_pos) == atomic_load(&rb->write_pos) && !atomic_load(&rb->message_available)) {
        return false;
    }
    
    // Read the message length first
    uint32_t msg_len;
    size_t read_pos = atomic_load(&rb->read_pos);
    
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
    atomic_store(&rb->read_pos, read_pos);
    atomic_store(&rb->message_available, false);
    *bytes_read = msg_len;
    
    return true;
}

/* Write a message to the ring buffer */
static bool ring_buffer_write(LockFreeNonBlockingRingBuffer* rb, const void* data, size_t len) {
    if (len > rb->size / 2) {
        // Prevent a single message from taking more than half the buffer
        return false;
    }
    
    // Wait for space in the buffer
    while (atomic_load(&rb->write_pos) == atomic_load(&rb->read_pos) && atomic_load(&rb->message_available)) {
        // Spin wait
        __builtin_ia32_pause();
    }
    
    // Write the message
    size_t write_pos = atomic_load(&rb->write_pos);
    
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
    
    // Update the write position
    atomic_store(&rb->write_pos, write_pos);
    atomic_store(&rb->message_available, true);
    
    return true;
}

/* Run the Lock-free io_uring Shared Memory server benchmark */
void run_lfnbshm_server(LockFreeNonBlockingRingBuffer* rb, int duration_secs) {
    void* buffer = malloc(MAX_MSG_SIZE);
    if (!buffer) {
        perror("malloc");
        return;
    }

    struct io_uring ring;
    if (io_uring_queue_init(32, &ring, 0) < 0) {
        perror("io_uring_queue_init");
        free(buffer);
        return;
    }

    // Start the benchmark
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    
    printf("Server ready to process messages\n");
    
    while (get_timestamp_us() < end_time) {
        // Wait for message using io_uring futex
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        uint32_t message_available = atomic_load(&rb->message_available);
        io_uring_prep_futex_wait(sqe, (uint32_t*)&rb->message_available, message_available, 0, 0, FUTEX_PRIVATE_FLAG);
        
        struct io_uring_cqe* cqe;
        if (io_uring_submit_and_wait(&ring, 1) < 0) {
            perror("io_uring_submit_and_wait");
            continue;
        }
        
        if (io_uring_wait_cqe(&ring, &cqe) < 0) {
            perror("io_uring_wait_cqe");
            continue;
        }
        
        io_uring_cqe_seen(&ring, cqe);
        
        // Read message from ring buffer
        size_t msg_size;
        bool read_success = ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &msg_size);
        
        if (read_success) {
            Message* msg = (Message*)buffer;
            if (!validate_message(msg, msg_size)) {
                fprintf(stderr, "Server: Message validation failed\n");
            }
            
            // Echo the message back
            if (!ring_buffer_write(rb, buffer, msg_size)) {
                fprintf(stderr, "Failed to write response to ring buffer\n");
            }
            
            // Wake client using io_uring futex
            sqe = io_uring_get_sqe(&ring);
            uint32_t response_available = atomic_load(&rb->response_available);
            io_uring_prep_futex_wake(sqe, (uint32_t*)&rb->response_available, response_available, 1, 0, FUTEX_PRIVATE_FLAG);
            io_uring_submit(&ring);
        }
    }
    
    io_uring_queue_exit(&ring);
    free(buffer);
    munmap(rb, sizeof(LockFreeNonBlockingRingBuffer) + BUFFER_SIZE);
    shm_unlink(SHM_NAME);
}

/* Run the Lock-free io_uring Shared Memory client benchmark */
void run_lfnbshm_client(LockFreeNonBlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats) {
    void* buffer = malloc(MAX_MSG_SIZE);
    if (!buffer) {
        perror("malloc");
        munmap(rb, sizeof(LockFreeNonBlockingRingBuffer) + BUFFER_SIZE);
        return;
    }
    uint64_t* latencies = malloc(sizeof(uint64_t) * MAX_LATENCIES);
    if (!latencies) {
        perror("malloc");
        free(buffer);
        munmap(rb, sizeof(LockFreeNonBlockingRingBuffer) + BUFFER_SIZE);
        return;
    }
    size_t latency_count = 0;
    double cpu_start = get_cpu_usage();

    struct io_uring ring;
    if (io_uring_queue_init(32, &ring, 0) < 0) {
        perror("io_uring_queue_init");
        free(buffer);
        free(latencies);
        munmap(rb, sizeof(LockFreeNonBlockingRingBuffer) + BUFFER_SIZE);
        return;
    }

    Message* msg = (Message*)buffer;

    // Benchmark phase
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
        
        // Wake up server
        futex_wake((volatile uint32_t*)&rb->server_futex, 1);

        // Wait for response
        size_t msg_size;
        bool read_success = ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &msg_size);
        
        if (read_success) {
            if (!validate_message(msg, msg_size)) {
                fprintf(stderr, "Client: Message validation failed\n");
            }

            // Calculate latency
            uint64_t now = get_timestamp_us();
            uint64_t latency = now - msg->timestamp;
            
            if (latency_count < MAX_LATENCIES) {
                latencies[latency_count++] = latency;
            }
            
            // Update statistics
            stats->ops++;
            stats->bytes += msg_size;
        } else {
            // Wait for server to write
            futex_wait((volatile uint32_t*)&rb->client_futex, atomic_load(&rb->client_futex));
        }
    }

    double cpu_end = get_cpu_usage();
    stats->cpu_usage = (cpu_end - cpu_start) / (10000.0 * duration_secs);
    calculate_stats(latencies, latency_count, stats);
    
    io_uring_queue_exit(&ring);
    free(buffer);
    free(latencies);
    munmap(rb, sizeof(LockFreeNonBlockingRingBuffer) + BUFFER_SIZE);
}

void free_lfnbshm(LockFreeNonBlockingRingBuffer* rb) {
    munmap(rb, sizeof(LockFreeNonBlockingRingBuffer) + rb->size);
    if (rb->is_server) {
        shm_unlink(SHM_NAME);
    }
} 