#include "lfshm.h"
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

/* Setup lock-free shared memory */
LockFreeRingBuffer* setup_lockfree_shared_memory(size_t size) {
    int fd;
    LockFreeRingBuffer* rb = NULL;
    size_t total_size = sizeof(LockFreeRingBuffer) + size;

    size_t page_size = getpagesize();
    total_size = (total_size + page_size - 1) & ~(page_size - 1);

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
    rb->read_pos = 0;
    rb->write_pos = 0;
    rb->size = size;
    rb->ready = true;

    close(fd);
    return rb;
}

/* Read a message from the lock-free ring buffer */
static bool ring_buffer_read(LockFreeRingBuffer* rb, void* data, size_t max_len, size_t* bytes_read) {
    *bytes_read = 0;
    
    // Check if the buffer is empty
    if (rb->read_pos == rb->write_pos) {
        return false;
    }
    
    // Read the message length first
    uint32_t msg_len;
    uint32_t read_pos = rb->read_pos;
    
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
    rb->read_pos = read_pos;
    *bytes_read = msg_len;
    
    return true;
}

/* Write a message to the lock-free ring buffer */
static bool ring_buffer_write(LockFreeRingBuffer* rb, const void* data, size_t len) {
    if (len > rb->size / 2) {
        // Prevent a single message from taking more than half the buffer
        return false;
    }
    
    // Calculate available space
    uint32_t read_pos = rb->read_pos;
    uint32_t write_pos = rb->write_pos;
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
    rb->write_pos = write_pos;
    
    return true;
}

/* Run the Lock-free Shared Memory server benchmark */
void run_lfshm_server(LockFreeRingBuffer* rb, int duration_secs) {
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
        // Read message from ring buffer
        size_t msg_size;
        bool read_success = ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &msg_size);
        
        if (read_success) {
            Message* msg = (Message*)buffer;
            if (!validate_message(msg, msg_size)) {
                fprintf(stderr, "Server: Message validation failed\n");
                continue;
            }
            
            // Echo the message back
            if (!ring_buffer_write(rb, buffer, msg_size)) {
                fprintf(stderr, "Failed to write response to ring buffer\n");
            }
        }
        
        // Small delay to prevent busy waiting
        usleep(1);
    }
    
    if (get_timestamp_us() >= end_time - 100000) { // Within 100ms of end
        printf("Server shutting down...\n");
    }
    
    // Cleanup
    free(buffer);
    munmap(rb, sizeof(LockFreeRingBuffer) + BUFFER_SIZE);
    shm_unlink(SHM_NAME);
}

/* Run the Lock-free Shared Memory client benchmark */
void run_lfshm_client(LockFreeRingBuffer* rb, int duration_secs, BenchmarkStats* stats) {
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

    // Initialize the message
    Message* msg = (Message*)buffer;
    msg->seq = 0;
    
    // Start warmup
    uint64_t start_warmup = get_timestamp_us();
    uint64_t end_warmup = start_warmup + (WARMUP_DURATION * 1000000ULL);

    // Send the first message (warmup)
    random_message(msg, 2048, 4096);
    if (!ring_buffer_write(rb, buffer, msg->size)) {
        fprintf(stderr, "Failed to write message to ring buffer\n");
        goto cleanup;
    }

    // Warmup phase
    while (get_timestamp_us() < end_warmup) {
        // Read response from ring buffer
        size_t msg_size;
        bool read_success = ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &msg_size);
        
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
            if (!ring_buffer_write(rb, buffer, msg->size)) {
                fprintf(stderr, "Failed to write message to ring buffer\n");
            }
        }
        
        // Small delay to prevent busy waiting
        usleep(1);
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
    if (!ring_buffer_write(rb, buffer, msg->size)) {
        fprintf(stderr, "Failed to write message to ring buffer\n");
        goto cleanup;
    }

    while (get_timestamp_us() < end_time) {
        // Read response from ring buffer
        size_t msg_size;
        bool read_success = ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &msg_size);
        
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
            if (!ring_buffer_write(rb, buffer, msg->size)) {
                fprintf(stderr, "Failed to write message to ring buffer\n");
            }
        }
        
        // Small delay to prevent busy waiting
        usleep(1);
    }
    
    if (get_timestamp_us() >= end_time - 100000) { // Within 100ms of end
        printf("\nBenchmark completed successfully.\n");
    }

    double cpu_end;
cleanup:    
    // Record CPU usage
    cpu_end = get_cpu_usage();
    stats->cpu_usage = (cpu_end - cpu_start) / (10000.0 * duration_secs);

    // Calculate final statistics
    calculate_stats(latencies, latency_count, stats);
    
    // Cleanup resources
    free(buffer);
    free(latencies);
    munmap(rb, sizeof(LockFreeRingBuffer) + BUFFER_SIZE);
} 