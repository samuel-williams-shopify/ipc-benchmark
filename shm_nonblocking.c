#include "shm_nonblocking.h"
#include "benchmark.h"
#include "interrupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/select.h>

#define SHM_NAME "/shm_nonblocking"

/* Forward declarations */
static bool ring_buffer_read(NonBlockingRingBuffer* rb, void* data, size_t max_len, size_t* bytes_read);
static bool ring_buffer_write(NonBlockingRingBuffer* rb, const void* data, size_t len);

/* Thread data structure */
typedef struct {
    NonBlockingRingBuffer* rb;
    Interrupt* intr;
    volatile bool should_exit;
    void* buffer;
    size_t* msg_size;
    bool* read_success;
} ThreadData;

/* Read a message from the ring buffer */
static bool ring_buffer_read(NonBlockingRingBuffer* rb, void* data, size_t max_len, size_t* bytes_read) {
    *bytes_read = 0;
    
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
    rb->message_available = false;
    *bytes_read = msg_len;
    
    return true;
}

/* Write a message to the ring buffer */
static bool ring_buffer_write(NonBlockingRingBuffer* rb, const void* data, size_t len) {
    if (len > rb->size / 2) {
        // Prevent a single message from taking more than half the buffer
        return false;
    }
    
    pthread_mutex_lock(&rb->mutex);
    
    // Wait for space in the buffer
    while (rb->write_pos == rb->read_pos && rb->message_available) {
        pthread_cond_wait(&rb->server_cond, &rb->mutex);
    }
    
    // Write the message
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
    
    // Update the write position
    rb->write_pos = write_pos;
    rb->message_available = true;
    pthread_cond_signal(&rb->server_cond);
    
    pthread_mutex_unlock(&rb->mutex);
    return true;
}

/* Thread function that waits on mutex and signals eventfd */
static void* notification_thread(void* arg) {
    ThreadData* data = (ThreadData*)arg;
    NonBlockingRingBuffer* rb = data->rb;
    
    printf("Client thread started\n");
    
    while (!data->should_exit) {
        pthread_mutex_lock(&rb->mutex);
        
        // Wait for a message to be available
        while (!rb->message_available && !data->should_exit) {
            pthread_cond_wait(&rb->server_cond, &rb->mutex);
        }
        
        if (rb->message_available && !data->should_exit) {
            // Read the message
            *data->read_success = ring_buffer_read(rb, data->buffer, MAX_MSG_SIZE, data->msg_size);
            
            // Signal the interrupt to wake up the select() loop
            interrupt_signal(data->intr);
        }
        
        pthread_mutex_unlock(&rb->mutex);
    }
    
    printf("Client thread exiting\n");
    return NULL;
}

NonBlockingRingBuffer* setup_shm_nonblocking(size_t size, bool is_server) {
    int fd = -1;
    NonBlockingRingBuffer* rb = NULL;
    size_t total_size = sizeof(NonBlockingRingBuffer) + size;

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
    pthread_mutexattr_t mutex_attr;
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&rb->mutex, &mutex_attr);
    pthread_mutexattr_destroy(&mutex_attr);

    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&rb->server_cond, &cond_attr);
    pthread_cond_init(&rb->client_cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    rb->read_pos = 0;
    rb->write_pos = 0;
    rb->size = size;
    rb->message_available = false;
    rb->response_available = false;
    rb->ready = is_server;
    rb->is_server = is_server;
    rb->buffer = (char*)rb + sizeof(NonBlockingRingBuffer);

    close(fd);
    return rb;
}

void free_shm_nonblocking(NonBlockingRingBuffer* rb) {
    if (rb) {
        munmap(rb, sizeof(NonBlockingRingBuffer) + rb->size);
        shm_unlink(SHM_NAME);
    }
}

void run_shm_nonblocking_client(NonBlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats) {
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

    Interrupt* intr = interrupt_create();
    if (!intr) {
        free(buffer);
        free(latencies);
        return;
    }

    size_t msg_size = 0;
    bool read_success = false;
    ThreadData thread_data = {
        .rb = rb,
        .intr = intr,
        .should_exit = false,
        .buffer = buffer,
        .msg_size = &msg_size,
        .read_success = &read_success
    };

    pthread_t notif_thread;
    if (pthread_create(&notif_thread, NULL, notification_thread, &thread_data) != 0) {
        perror("pthread_create");
        interrupt_destroy(intr);
        free(buffer);
        free(latencies);
        return;
    }

    Message* msg = (Message*)buffer;
    msg->seq = 0;
    uint64_t start_warmup = get_timestamp_us();
    uint64_t end_warmup = start_warmup + (WARMUP_DURATION * 1000000ULL);
    random_message(msg, 2048, 4096);

    // Send first message (warmup)
    if (!ring_buffer_write(rb, buffer, msg->size)) {
        fprintf(stderr, "Failed to write message to ring buffer\n");
        goto cleanup;
    }

    // Set up select() parameters
    fd_set read_fds;
    struct timeval timeout;

    // Warmup phase
    while (get_timestamp_us() < end_warmup) {
        FD_ZERO(&read_fds);
        FD_SET(interrupt_get_fd(intr), &read_fds);
        
        timeout.tv_sec = 0;
        timeout.tv_usec = 10000;  // 10ms timeout
        
        int ret = select(interrupt_get_fd(intr) + 1, &read_fds, NULL, NULL, &timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        if (FD_ISSET(interrupt_get_fd(intr), &read_fds)) {
            if (interrupt_clear(intr) == -1) {
                break;
            }
            
            if (read_success) {
                Message* response = (Message*)buffer;
                if (!validate_message(response, msg_size)) {
                    fprintf(stderr, "Client: Message validation failed\n");
                    continue;
                }
                
                // Send the next warmup message
                random_message(msg, 2048, 4096);
                if (!ring_buffer_write(rb, buffer, msg->size)) {
                    fprintf(stderr, "Failed to write message to ring buffer\n");
                }
            }
        }
    }

    printf("Warmup completed, starting benchmark...\n");
    stats->ops = 0;
    stats->bytes = 0;
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    random_message(msg, 2048, 4096);
    msg->timestamp = get_timestamp_us();

    // Send first message
    if (!ring_buffer_write(rb, buffer, msg->size)) {
        fprintf(stderr, "Failed to write message to ring buffer\n");
        goto cleanup;
    }

    while (get_timestamp_us() < end_time) {
        FD_ZERO(&read_fds);
        FD_SET(interrupt_get_fd(intr), &read_fds);
        
        timeout.tv_sec = 0;
        timeout.tv_usec = 10000;  // 10ms timeout
        
        int ret = select(interrupt_get_fd(intr) + 1, &read_fds, NULL, NULL, &timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        if (FD_ISSET(interrupt_get_fd(intr), &read_fds)) {
            if (interrupt_clear(intr) == -1) {
                break;
            }
            
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
        }
    }

    if (get_timestamp_us() >= end_time - 100000) {
        printf("\nBenchmark completed successfully.\n");
    }

cleanup:
    double cpu_end = get_cpu_usage();
    stats->cpu_usage = (cpu_end - cpu_start) / (10000.0 * duration_secs);
    calculate_stats(latencies, latency_count, stats);
    
    thread_data.should_exit = true;
    pthread_mutex_lock(&rb->mutex);
    pthread_cond_broadcast(&rb->server_cond);
    pthread_mutex_unlock(&rb->mutex);
    pthread_join(notif_thread, NULL);
    
    interrupt_destroy(intr);
    free(buffer);
    free(latencies);
}

void run_shm_nonblocking_server(NonBlockingRingBuffer* rb, int duration_secs) {
    void* buffer = malloc(MAX_MSG_SIZE);
    if (!buffer) {
        perror("malloc");
        return;
    }

    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    printf("Server ready to process messages\n");

    while (get_timestamp_us() < end_time) {
        pthread_mutex_lock(&rb->mutex);
        
        // Wait for a message to be available
        while (!rb->message_available) {
            pthread_cond_wait(&rb->server_cond, &rb->mutex);
        }
        
        size_t msg_size;
        bool read_success = ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &msg_size);
        
        if (read_success) {
            Message* msg = (Message*)buffer;
            if (!validate_message(msg, msg_size)) {
                fprintf(stderr, "Server: Message validation failed\n");
                pthread_mutex_unlock(&rb->mutex);
                continue;
            }
            
            // Echo the message back
            if (!ring_buffer_write(rb, buffer, msg_size)) {
                fprintf(stderr, "Failed to write response to ring buffer\n");
            }
        }
        
        pthread_mutex_unlock(&rb->mutex);
    }

    if (get_timestamp_us() >= end_time - 100000) {
        printf("Server shutting down...\n");
    }
    free(buffer);
} 
