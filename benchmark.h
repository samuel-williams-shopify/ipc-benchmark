#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Configuration constants */
#define SOCKET_PATH "/tmp/ipc_benchmark_socket"
#define SHM_NAME "/ipc_benchmark_shm"
#define MAX_MSG_SIZE (128 * 1024)  // 128KiB max message size
#define BUFFER_SIZE (4 * 1024 * 1024)  // 4MiB ring buffer
#define WARMUP_DURATION 1  // Warmup duration in seconds
#define RUN_DURATION 10     // Benchmark duration in seconds
#define MAX_LATENCIES 1000000  // Maximum number of latency measurements

#define MAGIC_START 0xDEADBEEF
#define MAGIC_END   0xCAFEBABE

/* Statistics structure */
typedef struct {
    uint64_t ops;            // Operations completed
    uint64_t bytes;          // Bytes transferred
    double cpu_usage;        // CPU usage percentage
    double avg_latency_us;   // Average latency in microseconds
    double p50_latency_us;   // 50th percentile latency
    double p95_latency_us;   // 95th percentile latency
    double p99_latency_us;   // 99th percentile latency
} BenchmarkStats;

/* Message structure */
typedef struct {
    uint32_t magic_start;  // Magic number to detect corruption
    uint32_t size;        // Message size
    uint32_t seq;         // Sequence number
    uint64_t timestamp;   // For latency calculation
    uint32_t checksum;    // Message checksum
    char data[0];         // Flexible array member for message payload
    // magic_end is written after data
} Message;

/* Utility functions */
uint64_t get_timestamp_us(void);
void calculate_stats(uint64_t* latencies, size_t count, BenchmarkStats* stats);
void print_stats(BenchmarkStats* stats, const char* title);
double get_cpu_usage(void);
uint32_t calculate_checksum(const void* data, size_t len);
bool validate_message(const Message* msg, size_t total_size);
void random_message(Message* msg, int min_size, int max_size);
void* alloc_aligned_buffer(size_t size); 