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

/* Setup a server-side Unix domain socket */
int setup_uds_server(const char* socket_path) {
    // Remove any existing socket file
    unlink(socket_path);
    
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(server_fd);
        unlink(socket_path);
        return -1;
    }
    
    if (listen(server_fd, 5) == -1) {
        perror("listen");
        close(server_fd);
        unlink(socket_path);
        return -1;
    }
    
    // Accept a connection
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd == -1) {
        perror("accept");
        close(server_fd);
        unlink(socket_path);
        return -1;
    }
    
    // We don't need the server socket anymore
    close(server_fd);
    
    return client_fd;
}

/* Setup a client-side Unix domain socket */
int setup_uds_client(const char* socket_path) {
    int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd == -1) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    
    // Try to connect, with retry
    int retries = 10;
    while (retries--) {
        if (connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            return client_fd;
        }
        
        if (errno != ENOENT && errno != ECONNREFUSED) {
            perror("connect");
            close(client_fd);
            return -1;
        }
        
        fprintf(stderr, "Waiting for server to start... (%d)\n", retries);
        sleep(1);
    }
    
    perror("connect");
    close(client_fd);
    return -1;
}

/* Run the Unix Domain Socket server benchmark */
void run_uds_server(int socket_fd, int duration_secs) {
    void* buffer = malloc(MAX_MSG_SIZE);
    if (!buffer) {
        perror("malloc");
        return;
    }

    // Start the benchmark
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    
    // Set socket to non-blocking mode
    int flags = fcntl(socket_fd, F_GETFL, 0);
    fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);

    fd_set read_fds, write_fds;
    bool waiting_to_write = false;
    size_t msg_size = 0;

    while (get_timestamp_us() < end_time) {
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 10000}; // 10ms timeout
        
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        
        if (!waiting_to_write) {
            FD_SET(socket_fd, &read_fds);
        }
        if (waiting_to_write) {
            FD_SET(socket_fd, &write_fds);
        }
        
        int ret = select(socket_fd + 1, &read_fds, &write_fds, NULL, &timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        if (FD_ISSET(socket_fd, &read_fds)) {
            ssize_t bytes_read = read(socket_fd, buffer, MAX_MSG_SIZE);
            if (bytes_read == 0) {
                // Clean shutdown - remote end closed connection
                printf("Connection closed by remote end\n");
                break;
            } else if (bytes_read < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("read");
                    break;
                }
                continue;
            }
            
            Message* msg = (Message*)buffer;
            if (!validate_message(msg, bytes_read)) {
                fprintf(stderr, "Server: Message validation failed\n");
                continue;
            }
            
            msg_size = msg->size;
            waiting_to_write = true;
        }
        
        if (FD_ISSET(socket_fd, &write_fds)) {
            ssize_t bytes_written = write(socket_fd, buffer, msg_size);
            if (bytes_written <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("write");
                    break;
                }
                continue;
            }
            waiting_to_write = false;
        }
    }
    
    free(buffer);
    close(socket_fd);
    unlink(SOCKET_PATH);
}

/* Run the Unix Domain Socket client benchmark */
void run_uds_client(int socket_fd, int duration_secs, BenchmarkStats* stats) {
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
    
    // Initialize the message
    Message* msg = (Message*)buffer;
    msg->seq = 0;
    
    // Record CPU usage at start
    double cpu_start = get_cpu_usage();
    
    // Set socket to non-blocking mode
    int flags = fcntl(socket_fd, F_GETFL, 0);
    fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
    
    // Start warmup
    uint64_t start_warmup = get_timestamp_us();
    uint64_t end_warmup = start_warmup + (WARMUP_DURATION * 1000000ULL);
    
    // Send the first message (warmup)
    random_message(msg, 2048, 4096);
    bool waiting_for_read = false;
    bool waiting_to_write = true;
    
    fd_set read_fds, write_fds;
    
    // Warmup phase
    while (get_timestamp_us() < end_warmup) {
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 10000}; // 10ms timeout
        
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        
        if (waiting_for_read) {
            FD_SET(socket_fd, &read_fds);
        }
        if (waiting_to_write) {
            FD_SET(socket_fd, &write_fds);
        }
        
        int ret = select(socket_fd + 1, &read_fds, &write_fds, NULL, &timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        if (FD_ISSET(socket_fd, &write_fds)) {
            ssize_t bytes_written = write(socket_fd, buffer, msg->size);
            if (bytes_written <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("write");
                    break;
                }
                continue;
            }
            waiting_to_write = false;
            waiting_for_read = true;
        }
        
        if (FD_ISSET(socket_fd, &read_fds)) {
            ssize_t bytes_read = read(socket_fd, buffer, MAX_MSG_SIZE);
            if (bytes_read <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("read");
                    break;
                }
                continue;
            }
            
            // Send the next warmup message
            random_message(msg, 2048, 4096);
            waiting_to_write = true;
            waiting_for_read = false;
        }
    }
    
    printf("Warmup completed, starting benchmark...\n");
    
    // Reset statistics
    uint64_t ops = 0;
    uint64_t bytes = 0;
    
    // Start the benchmark
    uint64_t start_time = get_timestamp_us();
    uint64_t end_time = start_time + (duration_secs * 1000000ULL);
    
    // Send the first message (benchmark)
    random_message(msg, 2048, 4096);
    msg->timestamp = get_timestamp_us();
    waiting_to_write = true;
    waiting_for_read = false;
    
    while (get_timestamp_us() < end_time) {
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 10000}; // 10ms timeout
        
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        
        if (waiting_for_read) {
            FD_SET(socket_fd, &read_fds);
        }
        if (waiting_to_write) {
            FD_SET(socket_fd, &write_fds);
        }
        
        int ret = select(socket_fd + 1, &read_fds, &write_fds, NULL, &timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        if (FD_ISSET(socket_fd, &write_fds)) {
            ssize_t bytes_written = write(socket_fd, buffer, msg->size);
            if (bytes_written <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("write");
                    break;
                }
                continue;
            }
            waiting_to_write = false;
            waiting_for_read = true;
        }
        
        if (FD_ISSET(socket_fd, &read_fds)) {
            ssize_t bytes_read = read(socket_fd, buffer, MAX_MSG_SIZE);
            if (bytes_read == 0) {
                // Clean shutdown - remote end closed connection
                printf("\nBenchmark completed successfully\n");
                break;
            } else if (bytes_read < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("read");
                    break;
                }
                continue;
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
            ops++;
            bytes += bytes_read;
            
            // Send the next message
            random_message(msg, 2048, 4096);
            msg->timestamp = get_timestamp_us();
            waiting_to_write = true;
            waiting_for_read = false;
        }
    }
    
    // Record CPU usage
    double cpu_end = get_cpu_usage();
    double cpu_usage = (cpu_end - cpu_start) / (10000.0 * duration_secs);
    
    // Update stats
    stats->ops = ops;
    stats->bytes = bytes;
    stats->cpu_usage = cpu_usage;
    calculate_stats(latencies, latency_count, stats);
    
    // Cleanup
    free(buffer);
    free(latencies);
    close(socket_fd);
} 