#include "shm.h"
#include "benchmark.h"
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

/* Thread data structure */
typedef struct {
    RingBuffer* rb;
    Interrupt* intr;  // notification interrupt
    bool is_server;
    volatile bool should_exit;
} ThreadData;

/* Thread function that waits on mutex and signals eventfd */
static void* notification_thread(void* arg) {
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
                // Signal the interrupt to wake up the select() loop
                interrupt_signal(data->intr);
            }
        } else {
            while (!rb->response_available && !data->should_exit) {
                pthread_cond_wait(&rb->client_cond, &rb->mutex);
            }
            
            if (rb->response_available && !data->should_exit) {
                rb->response_available = false;
                // Signal the interrupt to wake up the select() loop
                interrupt_signal(data->intr);
            }
        }
        
        pthread_mutex_unlock(&rb->mutex);
    }
    
    printf("%s thread exiting\n", data->is_server ? "Server" : "Client");
    return NULL;
}

/* Read a message from the ring buffer */
static bool ring_buffer_read(RingBuffer* rb, void* data, size_t max_len, size_t* bytes_read) {
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
static bool ring_buffer_write_client_to_server(RingBuffer* rb, const void* data, size_t len) {
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
    
    // Signal that a message is available
    rb->message_available = true;
    pthread_cond_signal(&rb->server_cond);
    
    pthread_mutex_unlock(&rb->mutex);
    
    return true;
}

/* Write a message from server to client */
static bool ring_buffer_write_server_to_client(RingBuffer* rb, const void* data, size_t len) {
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

/* Setup shared memory with pthread mutex and condition variables */
RingBuffer* setup_shared_memory(size_t size, bool is_server) {
    int fd = -1;
    RingBuffer* rb = NULL;
    size_t total_size;
    total_size = sizeof(RingBuffer) + size;

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
            pthread_mutexattr_destroy(&mutex_attr);
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
            pthread_condattr_destroy(&cond_attr);
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

/* Run the Shared Memory server benchmark */
void run_shm_server(RingBuffer* rb, int duration_secs) {
    void* buffer = malloc(MAX_MSG_SIZE);
    if (!buffer) {
        perror("malloc");
        return;
    }

    Interrupt* intr = interrupt_create();
    if (!intr) {
        free(buffer);
        return;
    }

    ThreadData thread_data = {
        .rb = rb,
        .intr = intr,
        .is_server = true,
        .should_exit = false
    };

    pthread_t notif_thread;
    if (pthread_create(&notif_thread, NULL, notification_thread, &thread_data) != 0) {
        perror("pthread_create");
        interrupt_destroy(intr);
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
            // Read the notification
            if (interrupt_clear(intr) == -1) {
                break;
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
    
    // Cleanup
    interrupt_destroy(intr);
    free(buffer);
    munmap(rb, sizeof(RingBuffer) + BUFFER_SIZE);
    shm_unlink(SHM_NAME);
}

/* Run the Shared Memory client benchmark */
void run_shm_client(RingBuffer* rb, int duration_secs, BenchmarkStats* stats) {
    void* buffer = malloc(MAX_MSG_SIZE);
    if (!buffer) {
        perror("malloc");
        munmap(rb, sizeof(RingBuffer) + BUFFER_SIZE);
        return;
    }
    uint64_t* latencies = malloc(sizeof(uint64_t) * MAX_LATENCIES);
    if (!latencies) {
        perror("malloc");
        free(buffer);
        munmap(rb, sizeof(RingBuffer) + BUFFER_SIZE);
        return;
    }
    size_t latency_count = 0;
    double cpu_start = get_cpu_usage();

    Interrupt* intr = interrupt_create();
    if (!intr) {
        free(buffer);
        free(latencies);
        munmap(rb, sizeof(RingBuffer) + BUFFER_SIZE);
        return;
    }

    ThreadData thread_data = {
        .rb = rb,
        .intr = intr,
        .is_server = false,
        .should_exit = false
    };

    pthread_t notif_thread;
    if (pthread_create(&notif_thread, NULL, notification_thread, &thread_data) != 0) {
        perror("pthread_create");
        interrupt_destroy(intr);
        free(buffer);
        free(latencies);
        munmap(rb, sizeof(RingBuffer) + BUFFER_SIZE);
        return;
    }

    // Initialize the message
    Message* msg = (Message*)buffer;
    msg->seq = 0;
    
    // Start warmup
    uint64_t start_warmup = get_timestamp_us();
    uint64_t end_warmup = start_warmup + (WARMUP_DURATION * 1000000ULL);

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
            // Read the notification
            if (interrupt_clear(intr) == -1) {
                break;
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
            // Read the notification
            if (interrupt_clear(intr) == -1) {
                break;
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

    double cpu_end;
cleanup:    
    // Record CPU usage
    cpu_end = get_cpu_usage();
    stats->cpu_usage = (cpu_end - cpu_start) / (10000.0 * duration_secs);

    // Calculate final statistics
    calculate_stats(latencies, latency_count, stats);
    
    // Cleanup resources
    interrupt_destroy(intr);
    free(buffer);
    free(latencies);
    munmap(rb, sizeof(RingBuffer) + BUFFER_SIZE);
} 