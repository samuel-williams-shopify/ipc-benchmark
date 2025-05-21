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
#include <time.h>

#define SHM_NAME "/shm_nonblocking"

/* Thread data structure */
typedef struct {
    NonBlockingRingBuffer* rb;
    Interrupt* read_intr;    // Signal when message is ready to process
    Interrupt* write_intr;   // Signal when ready to send next message
    volatile bool should_exit;
    Message* msg;            // Message buffer for both sending and receiving
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
    *bytes_read = msg_len;
    
    return true;
}

/* Write a message to the ring buffer */
static bool ring_buffer_write(NonBlockingRingBuffer* rb, const void* data, size_t len) {
    if (len > rb->size / 2) {
        // Prevent a single message from taking more than half the buffer
        return false;
    }
    
    // Check if there's enough space
    size_t available;
    if (rb->write_pos >= rb->read_pos) {
        available = rb->size - (rb->write_pos - rb->read_pos);
    } else {
        available = rb->read_pos - rb->write_pos;
    }
    
    // Check if there's enough space (reserving 1 byte to distinguish empty from full)
    if (available <= len + sizeof(uint32_t) + 1) {
        return false;
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
    
    return true;
}

NonBlockingRingBuffer* setup_shm_nonblocking(size_t size, bool is_server) {
    int fd = -1;
    NonBlockingRingBuffer* rb = NULL;
    size_t total_size = sizeof(NonBlockingRingBuffer) + size;

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
        rb->ready = true;
    } else {
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

        while (!rb->ready) {
            usleep(1000);
        }
    }

    close(fd);
    return rb;
}

void run_shm_nonblocking_server(NonBlockingRingBuffer* rb, int duration_secs, float work_secs) {
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
        while (!rb->message_available) {
            pthread_cond_wait(&rb->server_cond, &rb->mutex);
        }

        size_t msg_size;
        bool read_success = ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &msg_size);
        
        if (read_success) {
            rb->message_available = false;
            pthread_mutex_unlock(&rb->mutex);

            Message* msg = (Message*)buffer;
            if (!validate_message(msg, msg_size)) {
                continue;
            }
            
            if (work_secs > 0) {
                usleep(work_secs * 1000000);
            }
            
            pthread_mutex_lock(&rb->mutex);
            if (ring_buffer_write(rb, buffer, msg_size)) {
                rb->response_available = true;
                pthread_cond_signal(&rb->client_cond);
            }
            pthread_mutex_unlock(&rb->mutex);
        } else {
            pthread_mutex_unlock(&rb->mutex);
        }
    }

    fprintf(stderr, "Server exiting\n");
    
    free(buffer);
}

// Thread function that handles all shared memory operations
static void* run_client_thread(void* arg) {
    ThreadData* data = (ThreadData*)arg;
    NonBlockingRingBuffer* rb = data->rb;

    Message* msg = data->msg;
    void* buffer = msg;

    while (!data->should_exit) {
        interrupt_wait(data->write_intr);

        pthread_mutex_lock(&rb->mutex);

        if (!ring_buffer_write(rb, buffer, msg->size)) {
            pthread_mutex_unlock(&rb->mutex);
            break;
        }

        rb->message_available = true;
        pthread_cond_signal(&rb->server_cond);

        if (!rb->ready) {
            *data->read_success = false;
            *data->msg_size = 0;

            interrupt_signal(data->read_intr);
            pthread_mutex_unlock(&rb->mutex);
            
            break;
        }

        while (!rb->response_available) {
            pthread_cond_wait(&rb->client_cond, &rb->mutex);
        }

        size_t msg_size;
        bool read_success = ring_buffer_read(rb, buffer, MAX_MSG_SIZE, &msg_size);
        *data->read_success = read_success;
        *data->msg_size = msg_size;

        rb->response_available = false;
        pthread_mutex_unlock(&rb->mutex);
        interrupt_signal(data->read_intr);
    }

    return NULL;
}

void run_shm_nonblocking_client(NonBlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats) {
    Message* msg = malloc(MAX_MSG_SIZE);
    if (!msg) {
        perror("malloc");
        return;
    }
    uint64_t* latencies = malloc(sizeof(uint64_t) * MAX_LATENCIES);
    if (!latencies) {
        perror("malloc");
        free(msg);
        return;
    }

    Interrupt* read_intr = interrupt_create();
    Interrupt* write_intr = interrupt_create();
    if (!read_intr || !write_intr) {
        if (read_intr) interrupt_destroy(read_intr);
        if (write_intr) interrupt_destroy(write_intr);
        free(msg);
        free(latencies);
        return;
    }

    size_t msg_size = 0;
    bool read_success = false;
    ThreadData thread_data = {
        .rb = rb,
        .read_intr = read_intr,
        .write_intr = write_intr,
        .should_exit = false,
        .msg = msg,
        .msg_size = &msg_size,
        .read_success = &read_success
    };

    pthread_t client_thread;
    if (pthread_create(&client_thread, NULL, run_client_thread, &thread_data) != 0) {
        perror("pthread_create");
        interrupt_destroy(read_intr);
        interrupt_destroy(write_intr);
        free(msg);
        free(latencies);
        return;
    }

    size_t latency_count = 0;
    double cpu_start = get_cpu_usage();
    stats->ops = 0;
    stats->bytes = 0;
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);

    while (get_timestamp_us() < end_time) {
        random_message(msg, 2048, 4096);
        msg->timestamp = get_timestamp_us();

        interrupt_signal(write_intr);

        interrupt_wait(read_intr);

        if (!read_success) {
            break;
        }

        if (!validate_message(msg, msg_size)) {
            fprintf(stderr, "Client main: Message validation failed\n");
        }
        
        uint64_t now = get_timestamp_us();
        uint64_t latency = now - msg->timestamp;
        
        if (latency_count < MAX_LATENCIES) {
            latencies[latency_count++] = latency;
        }
        
        stats->ops++;
        stats->bytes += msg_size;
    }

    thread_data.should_exit = true;
    interrupt_signal(read_intr);
    interrupt_signal(write_intr);
    pthread_join(client_thread, NULL);

    double cpu_end = get_cpu_usage();
    stats->cpu_usage = (cpu_end - cpu_start) / (10000.0 * duration_secs);
    calculate_stats(latencies, latency_count, stats);
    
    interrupt_destroy(read_intr);
    interrupt_destroy(write_intr);
    free(msg);
    free(latencies);
}

void free_shm_nonblocking_server(NonBlockingRingBuffer* rb) {
    if (rb) {
        pthread_mutex_lock(&rb->mutex);
        rb->ready = false;
        pthread_mutex_unlock(&rb->mutex);
        
        munmap(rb, sizeof(NonBlockingRingBuffer) + rb->size);
        shm_unlink(SHM_NAME);
    }
}

void free_shm_nonblocking_client(NonBlockingRingBuffer* rb) {
    if (rb) {
        munmap(rb, sizeof(NonBlockingRingBuffer) + rb->size);
    }
}
