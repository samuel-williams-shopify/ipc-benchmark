#include "nbuds.h"
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

// Helper to read exactly n bytes (blocking)
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

// Helper to write exactly n bytes (blocking)
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

NBUDSState* setup_nbuds_server(const char* socket_path) {
    NBUDSState* state = malloc(sizeof(NBUDSState));
    if (!state) {
        perror("malloc");
        return NULL;
    }
    state->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (state->fd == -1) {
        perror("socket");
        free(state);
        return NULL;
    }
    unlink(socket_path);
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
    if (listen(state->fd, 1) == -1) {
        perror("listen");
        close(state->fd);
        free(state);
        return NULL;
    }
    state->is_server = true;
    return state;
}

NBUDSState* setup_nbuds_client(const char* socket_path) {
    NBUDSState* state = malloc(sizeof(NBUDSState));
    if (!state) {
        perror("malloc");
        return NULL;
    }
    state->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (state->fd == -1) {
        perror("socket");
        free(state);
        return NULL;
    }
    // Set non-blocking
    int flags = fcntl(state->fd, F_GETFL, 0);
    fcntl(state->fd, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    int ret;
    do {
        ret = connect(state->fd, (struct sockaddr*)&addr, sizeof(addr));
    } while (ret == -1 && errno == EINTR);
    if (ret == -1 && errno != EINPROGRESS) {
        perror("connect");
        close(state->fd);
        free(state);
        return NULL;
    }
    // Wait for connection to complete
    if (ret == -1 && errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(state->fd, &wfds);
        struct timeval tv = {5, 0};
        if (select(state->fd + 1, NULL, &wfds, NULL, &tv) <= 0) {
            perror("select/connect");
            close(state->fd);
            free(state);
            return NULL;
        }
    }
    state->is_server = false;
    return state;
}

void free_nbuds(NBUDSState* state) {
    if (state) {
        close(state->fd);
        if (state->is_server) {
            unlink(SOCKET_PATH);
        }
        free(state);
    }
}

void run_nbuds_server(NBUDSState* state, int duration_secs) {
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
        ssize_t header_bytes = read_full(client_fd, buffer, sizeof(Message));
        if (header_bytes <= 0) break;
        Message* msg = (Message*)buffer;
        size_t msg_size = msg->size;
        if (msg_size < sizeof(Message) + sizeof(uint32_t) || msg_size > MAX_MSG_SIZE) {
            fprintf(stderr, "Server: Invalid message size %zu\n", msg_size);
            break;
        }
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

// Non-blocking client with select
void run_nbuds_client(NBUDSState* state, int duration_secs, BenchmarkStats* stats) {
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
    size_t to_write = msg->size;
    size_t written = 0;
    while (written < to_write) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(state->fd, &wfds);
        if (select(state->fd + 1, NULL, &wfds, NULL, NULL) < 0) {
            perror("select");
            goto cleanup;
        }
        if (FD_ISSET(state->fd, &wfds)) {
            ssize_t w = send(state->fd, (char*)buffer + written, to_write - written, 0);
            if (w < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                perror("send");
                goto cleanup;
            }
            written += w;
        }
    }
    while (get_timestamp_us() < end_warmup) {
        // Read header
        size_t read_bytes = 0;
        while (read_bytes < sizeof(Message)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(state->fd, &rfds);
            if (select(state->fd + 1, &rfds, NULL, NULL, NULL) < 0) {
                perror("select");
                goto cleanup;
            }
            if (FD_ISSET(state->fd, &rfds)) {
                ssize_t r = recv(state->fd, (char*)buffer + read_bytes, sizeof(Message) - read_bytes, 0);
                if (r < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    perror("recv");
                    goto cleanup;
                }
                if (r == 0) goto cleanup;
                read_bytes += r;
            }
        }
        Message* response = (Message*)buffer;
        size_t msg_size = response->size;
        if (msg_size < sizeof(Message) + sizeof(uint32_t) || msg_size > MAX_MSG_SIZE) {
            fprintf(stderr, "Client: Invalid message size %zu\n", msg_size);
            goto cleanup;
        }
        // Read body
        read_bytes = 0;
        while (read_bytes < msg_size - sizeof(Message)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(state->fd, &rfds);
            if (select(state->fd + 1, &rfds, NULL, NULL, NULL) < 0) {
                perror("select");
                goto cleanup;
            }
            if (FD_ISSET(state->fd, &rfds)) {
                ssize_t r = recv(state->fd, (char*)buffer + sizeof(Message) + read_bytes, msg_size - sizeof(Message) - read_bytes, 0);
                if (r < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    perror("recv");
                    goto cleanup;
                }
                if (r == 0) goto cleanup;
                read_bytes += r;
            }
        }
        if (!validate_message(response, msg_size)) {
            fprintf(stderr, "Client: Message validation failed\n");
            continue;
        }
        random_message(msg, 2048, 4096);
        // Send next message
        to_write = msg->size;
        written = 0;
        while (written < to_write) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(state->fd, &wfds);
            if (select(state->fd + 1, NULL, &wfds, NULL, NULL) < 0) {
                perror("select");
                goto cleanup;
            }
            if (FD_ISSET(state->fd, &wfds)) {
                ssize_t w = send(state->fd, (char*)buffer + written, to_write - written, 0);
                if (w < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    perror("send");
                    goto cleanup;
                }
                written += w;
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
    to_write = msg->size;
    written = 0;
    while (written < to_write) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(state->fd, &wfds);
        if (select(state->fd + 1, NULL, &wfds, NULL, NULL) < 0) {
            perror("select");
            goto cleanup;
        }
        if (FD_ISSET(state->fd, &wfds)) {
            ssize_t w = send(state->fd, (char*)buffer + written, to_write - written, 0);
            if (w < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                perror("send");
                goto cleanup;
            }
            written += w;
        }
    }
    while (get_timestamp_us() < end_time) {
        // Read header
        size_t read_bytes = 0;
        while (read_bytes < sizeof(Message)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(state->fd, &rfds);
            if (select(state->fd + 1, &rfds, NULL, NULL, NULL) < 0) {
                perror("select");
                goto cleanup;
            }
            if (FD_ISSET(state->fd, &rfds)) {
                ssize_t r = recv(state->fd, (char*)buffer + read_bytes, sizeof(Message) - read_bytes, 0);
                if (r < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    perror("recv");
                    goto cleanup;
                }
                if (r == 0) goto cleanup;
                read_bytes += r;
            }
        }
        Message* response = (Message*)buffer;
        size_t msg_size = response->size;
        if (msg_size < sizeof(Message) + sizeof(uint32_t) || msg_size > MAX_MSG_SIZE) {
            fprintf(stderr, "Client: Invalid message size %zu\n", msg_size);
            goto cleanup;
        }
        // Read body
        read_bytes = 0;
        while (read_bytes < msg_size - sizeof(Message)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(state->fd, &rfds);
            if (select(state->fd + 1, &rfds, NULL, NULL, NULL) < 0) {
                perror("select");
                goto cleanup;
            }
            if (FD_ISSET(state->fd, &rfds)) {
                ssize_t r = recv(state->fd, (char*)buffer + sizeof(Message) + read_bytes, msg_size - sizeof(Message) - read_bytes, 0);
                if (r < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    perror("recv");
                    goto cleanup;
                }
                if (r == 0) goto cleanup;
                read_bytes += r;
            }
        }
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
        to_write = msg->size;
        written = 0;
        while (written < to_write) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(state->fd, &wfds);
            if (select(state->fd + 1, NULL, &wfds, NULL, NULL) < 0) {
                perror("select");
                goto cleanup;
            }
            if (FD_ISSET(state->fd, &wfds)) {
                ssize_t w = send(state->fd, (char*)buffer + written, to_write - written, 0);
                if (w < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    perror("send");
                    goto cleanup;
                }
                written += w;
            }
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