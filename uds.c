#include "uds.h"
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
UDSState* setup_uds_server(const char* socket_path) {
    UDSState* state = malloc(sizeof(UDSState));
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
UDSState* setup_uds_client(const char* socket_path) {
    UDSState* state = malloc(sizeof(UDSState));
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
void free_uds(UDSState* state) {
    if (state) {
        close(state->fd);
        if (state->is_server) {
            unlink(SOCKET_PATH);
        }
        free(state);
    }
}

/* Run the Unix Domain Socket server benchmark */
void run_uds_server(UDSState* state, int duration_secs) {
    void* buffer = malloc(MAX_MSG_SIZE);
    if (!buffer) {
        perror("malloc");
        return;
    }

    // Accept client connection
    int client_fd = accept(state->fd, NULL, NULL);
    if (client_fd == -1) {
        perror("accept");
        free(buffer);
        return;
    }

    // Start the benchmark
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    
    printf("Server ready to process messages\n");
    
    while (get_timestamp_us() < end_time) {
        // Read message from client
        ssize_t bytes_read = recv(client_fd, buffer, MAX_MSG_SIZE, 0);
        if (bytes_read <= 0) {
            if (bytes_read == -1 && errno == EINTR) continue;
            break;
        }
        
        Message* msg = (Message*)buffer;
        if (!validate_message(msg, bytes_read)) {
            fprintf(stderr, "Server: Message validation failed\n");
            continue;
        }
        
        // Echo the message back
        if (send(client_fd, buffer, bytes_read, 0) == -1) {
            perror("send");
            break;
        }
    }
    
    if (get_timestamp_us() >= end_time - 100000) { // Within 100ms of end
        printf("Server shutting down...\n");
    }
    
    // Cleanup
    close(client_fd);
    free(buffer);
}

/* Run the Unix Domain Socket client benchmark */
void run_uds_client(UDSState* state, int duration_secs, BenchmarkStats* stats) {
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
    if (send(state->fd, buffer, msg->size, 0) == -1) {
        perror("send");
        goto cleanup;
    }

    // Warmup phase
    while (get_timestamp_us() < end_warmup) {
        // Read response from server
        ssize_t bytes_read = recv(state->fd, buffer, MAX_MSG_SIZE, 0);
        if (bytes_read <= 0) {
            if (bytes_read == -1 && errno == EINTR) continue;
            break;
        }
        
        Message* response = (Message*)buffer;
        if (!validate_message(response, bytes_read)) {
            fprintf(stderr, "Client: Message validation failed\n");
            continue;
        }
        
        // Send the next warmup message
        random_message(msg, 2048, 4096);
        if (send(state->fd, buffer, msg->size, 0) == -1) {
            perror("send");
            break;
        }
    }
    
    printf("Warmup completed, starting benchmark...\n");
    
    // Reset statistics
    stats->ops = 0;
    stats->bytes = 0;
    
    // Start the benchmark
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    
    // Send the first message (benchmark)
    random_message(msg, 2048, 4096);
    msg->timestamp = get_timestamp_us();
    if (send(state->fd, buffer, msg->size, 0) == -1) {
        perror("send");
        goto cleanup;
    }

    while (get_timestamp_us() < end_time) {
        // Read response from server
        ssize_t bytes_read = recv(state->fd, buffer, MAX_MSG_SIZE, 0);
        if (bytes_read <= 0) {
            if (bytes_read == -1 && errno == EINTR) continue;
            break;
        }
        
        Message* response = (Message*)buffer;
        if (!validate_message(response, bytes_read)) {
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
        stats->bytes += bytes_read;
        
        // Send the next message
        random_message(msg, 2048, 4096);
        msg->timestamp = get_timestamp_us();
        if (send(state->fd, buffer, msg->size, 0) == -1) {
            perror("send");
            break;
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
    free(buffer);
    free(latencies);
} 