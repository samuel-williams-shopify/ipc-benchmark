#include "uds_nonblocking.h"
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
#include <poll.h>

// Helper to read exactly n bytes (non-blocking with poll)
static ssize_t read_nonblocking(int fd, void* buf, size_t n) {
    size_t total = 0;
    char* ptr = (char*)buf;
    while (total < n) {
        ssize_t r = recv(fd, ptr + total, n - total, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {
                    .fd = fd,
                    .events = POLLIN,
                    .revents = 0
                };
                if (poll(&pfd, 1, -1) < 0) {
                    perror("poll");
                    return -1;
                }
                continue;
            }
            return -1;
        }
        if (r == 0) break;
        total += r;
    }
    return total;
}

// Helper to write exactly n bytes (non-blocking with poll)
static ssize_t write_nonblocking(int fd, const void* buf, size_t n) {
    size_t total = 0;
    const char* ptr = (const char*)buf;
    while (total < n) {
        ssize_t w = send(fd, ptr + total, n - total, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {
                    .fd = fd,
                    .events = POLLOUT,
                    .revents = 0
                };
                if (poll(&pfd, 1, -1) < 0) {
                    perror("poll");
                    return -1;
                }
                continue;
            }
            return -1;
        }
        total += w;
    }
    return total;
}

/* Setup Unix Domain Socket server */
UDSNonblockingState* setup_uds_nonblocking_server(const char* socket_path) {
    UDSNonblockingState* state = malloc(sizeof(UDSNonblockingState));
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
UDSNonblockingState* setup_uds_nonblocking_client(const char* socket_path) {
    UDSNonblockingState* state = malloc(sizeof(UDSNonblockingState));
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

    // Set socket to non-blocking mode
    int flags = fcntl(state->fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        close(state->fd);
        free(state);
        return NULL;
    }
    if (fcntl(state->fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        close(state->fd);
        free(state);
        return NULL;
    }

    // Connect to server
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    int ret = connect(state->fd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret == -1 && errno != EINPROGRESS) {
        perror("connect");
        close(state->fd);
        free(state);
        return NULL;
    }

    // Wait for connection to complete using poll
    struct pollfd pfd = {
        .fd = state->fd,
        .events = POLLOUT,
        .revents = 0
    };

    ret = poll(&pfd, 1, 5000); // 5 second timeout
    if (ret <= 0) {
        perror("poll");
        close(state->fd);
        free(state);
        return NULL;
    }

    if (pfd.revents & POLLERR) {
        fprintf(stderr, "Connection failed\n");
        close(state->fd);
        free(state);
        return NULL;
    }

    state->is_server = false;
    return state;
}

/* Free Unix Domain Socket resources */
void free_uds_nonblocking(UDSNonblockingState* state) {
    if (state) {
        close(state->fd);
        if (state->is_server) {
            unlink(SOCKET_PATH);
        }
        free(state);
    }
}

/* Run the Unix Domain Socket server benchmark */
void run_uds_nonblocking_server(UDSNonblockingState* state, int duration_secs) {
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
        ssize_t header_bytes = recv(client_fd, buffer, sizeof(Message), 0);
        if (header_bytes <= 0) break;
        Message* msg = (Message*)buffer;
        size_t msg_size = msg->size;
        if (msg_size < sizeof(Message) + sizeof(uint32_t) || msg_size > MAX_MSG_SIZE) {
            fprintf(stderr, "Server: Invalid message size %zu\n", msg_size);
            break;
        }
        // Read the rest of the message
        ssize_t body_bytes = recv(client_fd, (char*)buffer + sizeof(Message), msg_size - sizeof(Message), 0);
        if (body_bytes < 0) break;
        if (!validate_message(msg, msg_size)) {
            fprintf(stderr, "Server: Message validation failed\n");
        }
        if (send(client_fd, buffer, msg_size, 0) != (ssize_t)msg_size) {
            perror("send");
            break;
        }
    }

    close(client_fd);
    free(buffer);
}

/* Run the Unix Domain Socket client benchmark */
void run_uds_nonblocking_client(UDSNonblockingState* state, int duration_secs, BenchmarkStats* stats) {
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

    // Benchmark phase
    stats->ops = 0;
    stats->bytes = 0;
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);

    while (get_timestamp_us() < end_time) {
        // Send message
        random_message(msg, 2048, 4096);
        msg->timestamp = get_timestamp_us();
        ssize_t sent = write_nonblocking(state->fd, buffer, msg->size);
        if (sent != msg->size) {
            if (sent < 0 && (errno == EPIPE || errno == ECONNRESET)) {
                fprintf(stderr, "Connection closed by server (benchmark)\n");
                break;
            }
            perror("write_nonblocking");
            break;
        }

        // Read header
        ssize_t header_bytes = read_nonblocking(state->fd, buffer, sizeof(Message));
        if (header_bytes <= 0) {
            if (header_bytes < 0 && (errno == EPIPE || errno == ECONNRESET)) {
                fprintf(stderr, "Connection closed by server (benchmark)\n");
                break;
            }
            fprintf(stderr, "Failed to read message header\n");
            break;
        }

        Message* response = (Message*)buffer;
        size_t msg_size = response->size;
        if (msg_size < sizeof(Message) + sizeof(uint32_t) || msg_size > MAX_MSG_SIZE) {
            fprintf(stderr, "Client: Invalid message size %zu\n", msg_size);
            break;
        }

        // Read body
        ssize_t body_bytes = read_nonblocking(state->fd, (char*)buffer + sizeof(Message), msg_size - sizeof(Message));
        if (body_bytes < 0) {
            if (errno == EPIPE || errno == ECONNRESET) {
                fprintf(stderr, "Connection closed by server (benchmark)\n");
                break;
            }
            fprintf(stderr, "Failed to read message body\n");
            break;
        }

        if (!validate_message(response, msg_size)) {
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
    }

    double cpu_end = get_cpu_usage();
    stats->cpu_usage = (cpu_end - cpu_start) / (10000.0 * duration_secs);
    calculate_stats(latencies, latency_count, stats);
    
    free(buffer);
    free(latencies);
} 