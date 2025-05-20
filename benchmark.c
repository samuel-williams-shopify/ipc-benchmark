#include "benchmark.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>

/* Get current timestamp in microseconds */
uint64_t get_timestamp_us(void) {
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
uint32_t calculate_checksum(const void* data, size_t len) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (sum << 1) | (sum >> 31);  // Rotate left by 1
        sum += bytes[i];
    }
    return sum;
}

/* Validate a message */
bool validate_message(const Message* msg, size_t total_size) {
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

/* Allocate an aligned buffer for message data */
void* alloc_aligned_buffer(size_t size) {
    void* buffer;
    if (posix_memalign(&buffer, 64, size) != 0) {
        perror("posix_memalign");
        return NULL;
    }
    return buffer;
} 