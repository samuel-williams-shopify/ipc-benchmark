#include "shm_blocking.h"
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

#define SHM_NAME "/shm_blocking"

BlockingRingBuffer* setup_shm_blocking(size_t size, bool is_server) {
    BlockingRingBuffer* rb = malloc(sizeof(BlockingRingBuffer));
    if (!rb) {
        perror("malloc");
        return NULL;
    }

    // Create shared memory
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open");
        free(rb);
        return NULL;
    }

    // Set size
    if (ftruncate(fd, size) == -1) {
        perror("ftruncate");
        close(fd);
        free(rb);
        return NULL;
    }

    // Map shared memory
    rb->buffer = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (rb->buffer == MAP_FAILED) {
        perror("mmap");
        close(fd);
        free(rb);
        return NULL;
    }
    close(fd);

    rb->size = size;
    rb->read_pos = 0;
    rb->write_pos = 0;
    rb->is_server = is_server;

    // Initialize mutex and condition variables
    pthread_mutexattr_t mutex_attr;
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&rb->mutex, &mutex_attr);
    pthread_mutexattr_destroy(&mutex_attr);

    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&rb->not_empty, &cond_attr);
    pthread_cond_init(&rb->not_full, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    return rb;
}

void free_shm_blocking(BlockingRingBuffer* rb) {
    if (rb) {
        if (rb->is_server) {
            shm_unlink(SHM_NAME);
        }
        munmap(rb->buffer, rb->size);
        pthread_mutex_destroy(&rb->mutex);
        pthread_cond_destroy(&rb->not_empty);
        pthread_cond_destroy(&rb->not_full);
        free(rb);
    }
}

void run_shm_blocking_server(BlockingRingBuffer* rb, int duration_secs) {
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
        while (rb->write_pos == rb->read_pos) {
            pthread_cond_wait(&rb->not_empty, &rb->mutex);
        }

        // Read message
        size_t read_pos = rb->read_pos;
        size_t write_pos = rb->write_pos;
        size_t available = write_pos - read_pos;
        size_t to_read = available > MAX_MSG_SIZE ? MAX_MSG_SIZE : available;
        memcpy(buffer, rb->buffer + (read_pos % rb->size), to_read);
        rb->read_pos = read_pos + to_read;

        pthread_cond_signal(&rb->not_full);
        pthread_mutex_unlock(&rb->mutex);

        // Process message
        Message* msg = (Message*)buffer;
        if (!validate_message(msg, to_read)) {
            fprintf(stderr, "Server: Message validation failed\n");
            continue;
        }

        // Echo back
        pthread_mutex_lock(&rb->mutex);
        while ((rb->write_pos - rb->read_pos) >= rb->size) {
            pthread_cond_wait(&rb->not_full, &rb->mutex);
        }

        size_t write_pos2 = rb->write_pos;
        memcpy(rb->buffer + (write_pos2 % rb->size), buffer, to_read);
        rb->write_pos = write_pos2 + to_read;

        pthread_cond_signal(&rb->not_empty);
        pthread_mutex_unlock(&rb->mutex);
    }

    if (get_timestamp_us() >= end_time - 100000) {
        printf("Server shutting down...\n");
    }
    free(buffer);
}

void run_shm_blocking_client(BlockingRingBuffer* rb, int duration_secs, BenchmarkStats* stats) {
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
    Message* msg = (Message*)buffer;
    msg->seq = 0;
    uint64_t start_warmup = get_timestamp_us();
    uint64_t end_warmup = start_warmup + (WARMUP_DURATION * 1000000ULL);
    random_message(msg, 2048, 4096);

    // Send first message (warmup)
    pthread_mutex_lock(&rb->mutex);
    while ((rb->write_pos - rb->read_pos) >= rb->size) {
        pthread_cond_wait(&rb->not_full, &rb->mutex);
    }

    size_t write_pos = rb->write_pos;
    memcpy(rb->buffer + (write_pos % rb->size), buffer, msg->size);
    rb->write_pos = write_pos + msg->size;

    pthread_cond_signal(&rb->not_empty);
    pthread_mutex_unlock(&rb->mutex);

    while (get_timestamp_us() < end_warmup) {
        pthread_mutex_lock(&rb->mutex);
        while (rb->write_pos == rb->read_pos) {
            pthread_cond_wait(&rb->not_empty, &rb->mutex);
        }

        // Read response
        size_t read_pos = rb->read_pos;
        size_t write_pos2 = rb->write_pos;
        size_t available = write_pos2 - read_pos;
        size_t to_read = available > MAX_MSG_SIZE ? MAX_MSG_SIZE : available;
        memcpy(buffer, rb->buffer + (read_pos % rb->size), to_read);
        rb->read_pos = read_pos + to_read;

        pthread_cond_signal(&rb->not_full);
        pthread_mutex_unlock(&rb->mutex);

        if (!validate_message(msg, to_read)) {
            fprintf(stderr, "Client: Message validation failed\n");
            continue;
        }

        random_message(msg, 2048, 4096);

        // Send next message
        pthread_mutex_lock(&rb->mutex);
        while ((rb->write_pos - rb->read_pos) >= rb->size) {
            pthread_cond_wait(&rb->not_full, &rb->mutex);
        }

        size_t write_pos3 = rb->write_pos;
        memcpy(rb->buffer + (write_pos3 % rb->size), buffer, msg->size);
        rb->write_pos = write_pos3 + msg->size;

        pthread_cond_signal(&rb->not_empty);
        pthread_mutex_unlock(&rb->mutex);
    }

    printf("Warmup completed, starting benchmark...\n");
    stats->ops = 0;
    stats->bytes = 0;
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    random_message(msg, 2048, 4096);
    msg->timestamp = get_timestamp_us();

    // Send first message
    pthread_mutex_lock(&rb->mutex);
    while ((rb->write_pos - rb->read_pos) >= rb->size) {
        pthread_cond_wait(&rb->not_full, &rb->mutex);
    }

    size_t write_pos4 = rb->write_pos;
    memcpy(rb->buffer + (write_pos4 % rb->size), buffer, msg->size);
    rb->write_pos = write_pos4 + msg->size;

    pthread_cond_signal(&rb->not_empty);
    pthread_mutex_unlock(&rb->mutex);

    while (get_timestamp_us() < end_time) {
        pthread_mutex_lock(&rb->mutex);
        while (rb->write_pos == rb->read_pos) {
            pthread_cond_wait(&rb->not_empty, &rb->mutex);
        }

        // Read response
        size_t read_pos = rb->read_pos;
        size_t write_pos2 = rb->write_pos;
        size_t available = write_pos2 - read_pos;
        size_t to_read = available > MAX_MSG_SIZE ? MAX_MSG_SIZE : available;
        memcpy(buffer, rb->buffer + (read_pos % rb->size), to_read);
        rb->read_pos = read_pos + to_read;

        pthread_cond_signal(&rb->not_full);
        pthread_mutex_unlock(&rb->mutex);

        if (!validate_message(msg, to_read)) {
            fprintf(stderr, "Client: Message validation failed\n");
            continue;
        }

        uint64_t now = get_timestamp_us();
        uint64_t latency = now - msg->timestamp;
        if (latency_count < MAX_LATENCIES) {
            latencies[latency_count++] = latency;
        }
        stats->ops++;
        stats->bytes += to_read;

        random_message(msg, 2048, 4096);
        msg->timestamp = get_timestamp_us();

        // Send next message
        pthread_mutex_lock(&rb->mutex);
        while ((rb->write_pos - rb->read_pos) >= rb->size) {
            pthread_cond_wait(&rb->not_full, &rb->mutex);
        }

        size_t write_pos3 = rb->write_pos;
        memcpy(rb->buffer + (write_pos3 % rb->size), buffer, msg->size);
        rb->write_pos = write_pos3 + msg->size;

        pthread_cond_signal(&rb->not_empty);
        pthread_mutex_unlock(&rb->mutex);
    }

    if (get_timestamp_us() >= end_time - 100000) {
        printf("\nBenchmark completed successfully.\n");
    }
    double cpu_end;
    cpu_end = get_cpu_usage();
    stats->cpu_usage = (cpu_end - cpu_start) / (10000.0 * duration_secs);
    calculate_stats(latencies, latency_count, stats);
    free(buffer);
    free(latencies);
} 