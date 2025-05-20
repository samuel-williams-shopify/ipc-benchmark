#include "buds.h"
#include "benchmark.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>

/* Setup Unix Domain Socket server */
BUDSState* setup_buds_server(const char* socket_path) {
    BUDSState* state = malloc(sizeof(BUDSState));
    if (!state) {
        perror("malloc");
        return NULL;
    }

    // Create socket
    state->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (state->fd == -1) {
        perror("socket");
        free(state);
        return NULL;
    }

    // Remove existing socket file if it exists
    unlink(socket_path);

    // Bind socket
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(state->fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(state->fd);
        free(state);
        return NULL;
    }

    // Listen for connections
    if (listen(state->fd, 1) == -1) {
        perror("listen");
        close(state->fd);
        free(state);
        return NULL;
    }

    state->is_server = true;
    return state;
}

/* Setup Unix Domain Socket client */
BUDSState* setup_buds_client(const char* socket_path) {
    BUDSState* state = malloc(sizeof(BUDSState));
    if (!state) {
        perror("malloc");
        return NULL;
    }

    // Create socket
    state->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (state->fd == -1) {
        perror("socket");
        free(state);
        return NULL;
    }

    // Connect to server
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(state->fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("connect");
        close(state->fd);
        free(state);
        return NULL;
    }

    state->is_server = false;
    return state;
}

/* Free Unix Domain Socket resources */
void free_buds(BUDSState* state) {
    if (state) {
        close(state->fd);
        if (state->is_server) {
            unlink(SOCKET_PATH);
        }
        free(state);
    }
}

// Helper to read exactly n bytes
static ssize_t read_full(int fd, void* buf, size_t n) {
    size_t total = 0;
    char* ptr = (char*)buf;
    while (total < n) {
        ssize_t r = recv(fd, ptr + total, n - total, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        total += r;
    }
    return total;
}

// Helper to write exactly n bytes
static ssize_t write_full(int fd, const void* buf, size_t n) {
    size_t total = 0;
    const char* ptr = (const char*)buf;
    while (total < n) {
        ssize_t w = send(fd, ptr + total, n - total, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += w;
    }
    return total;
}

/* Run the Unix Domain Socket server benchmark */
void run_buds_server(BUDSState* state, int duration_secs) {
    void* buffer = malloc(MAX_MSG_SIZE);
    if (!buffer) {
        perror("malloc");
        return;
    }

    int client_fd = accept(state->fd, NULL, NULL);
    if (client_fd == -1) {
        perror("accept");
        free(buffer);
        return;
    }

    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    printf("Server ready to process messages\n");
    while (get_timestamp_us() < end_time) {
        // First, read the header to get the size
        ssize_t header_bytes = read_full(client_fd, buffer, sizeof(Message));
        if (header_bytes <= 0) break;
        Message* msg = (Message*)buffer;
        size_t msg_size = msg->size;
        if (msg_size < sizeof(Message) + sizeof(uint32_t) || msg_size > MAX_MSG_SIZE) {
            fprintf(stderr, "Server: Invalid message size %zu\n", msg_size);
            break;
        }
        // Read the rest of the message
        ssize_t body_bytes = read_full(client_fd, (char*)buffer + sizeof(Message), msg_size - sizeof(Message));
        if (body_bytes < 0) break;
        if (!validate_message(msg, msg_size)) {
            fprintf(stderr, "Server: Message validation failed\n");
            continue;
        }
        if ((size_t)write_full(client_fd, buffer, msg_size) != msg_size) {
            perror("send");
            break;
        }
    }
    if (get_timestamp_us() >= end_time - 100000) {
        printf("Server shutting down...\n");
    }
    close(client_fd);
    free(buffer);
}

/* Run the Unix Domain Socket client benchmark */
void run_buds_client(BUDSState* state, int duration_secs, BenchmarkStats* stats) {
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
    if (write_full(state->fd, buffer, msg->size) != msg->size) {
        perror("send");
        goto cleanup;
    }
    while (get_timestamp_us() < end_warmup) {
        // Read header
        ssize_t header_bytes = read_full(state->fd, buffer, sizeof(Message));
        if (header_bytes <= 0) break;
        Message* response = (Message*)buffer;
        size_t msg_size = response->size;
        if (msg_size < sizeof(Message) + sizeof(uint32_t) || msg_size > MAX_MSG_SIZE) {
            fprintf(stderr, "Client: Invalid message size %zu\n", msg_size);
            break;
        }
        ssize_t body_bytes = read_full(state->fd, (char*)buffer + sizeof(Message), msg_size - sizeof(Message));
        if (body_bytes < 0) break;
        if (!validate_message(response, msg_size)) {
            fprintf(stderr, "Client: Message validation failed\n");
            continue;
        }
        random_message(msg, 2048, 4096);
        if (write_full(state->fd, buffer, msg->size) != msg->size) {
            perror("send");
            break;
        }
    }
    printf("Warmup completed, starting benchmark...\n");
    stats->ops = 0;
    stats->bytes = 0;
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    random_message(msg, 2048, 4096);
    msg->timestamp = get_timestamp_us();
    if (write_full(state->fd, buffer, msg->size) != msg->size) {
        perror("send");
        goto cleanup;
    }
    while (get_timestamp_us() < end_time) {
        ssize_t header_bytes = read_full(state->fd, buffer, sizeof(Message));
        if (header_bytes <= 0) break;
        Message* response = (Message*)buffer;
        size_t msg_size = response->size;
        if (msg_size < sizeof(Message) + sizeof(uint32_t) || msg_size > MAX_MSG_SIZE) {
            fprintf(stderr, "Client: Invalid message size %zu\n", msg_size);
            break;
        }
        ssize_t body_bytes = read_full(state->fd, (char*)buffer + sizeof(Message), msg_size - sizeof(Message));
        if (body_bytes < 0) break;
        if (!validate_message(response, msg_size)) {
            fprintf(stderr, "Client: Message validation failed\n");
            continue;
        }
        uint64_t now = get_timestamp_us();
        uint64_t latency = now - response->timestamp;
        if (latency_count < MAX_LATENCIES) {
            latencies[latency_count++] = latency;
        }
        stats->ops++;
        stats->bytes += msg_size;
        random_message(msg, 2048, 4096);
        msg->timestamp = get_timestamp_us();
        if (write_full(state->fd, buffer, msg->size) != msg->size) {
            perror("send");
            break;
        }
    }
    if (get_timestamp_us() >= end_time - 100000) {
        printf("\nBenchmark completed successfully.\n");
    }
    double cpu_end;
cleanup:
    cpu_end = get_cpu_usage();
    stats->cpu_usage = (cpu_end - cpu_start) / (10000.0 * duration_secs);
    calculate_stats(latencies, latency_count, stats);
    free(buffer);
    free(latencies);
} 