# IPC Benchmark

A performance comparison of different Inter-Process Communication (IPC) mechanisms with io_uring integration.

## Overview

This benchmark compares three different IPC approaches:

1. **Unix Domain Socket with io_uring**
   - Direct integration with io_uring's IORING_OP_SEND/RECV
   - Kernel-managed buffering and synchronization

2. **Shared Memory with pthread_mutex and io_uring**
   - Shared memory region with a ring buffer
   - pthread_mutex and condition variables for synchronization 
   - Dedicated thread waiting on mutex, signaling eventfd
   - io_uring integration via eventfd

3. **Linux-optimized Lock-free Shared Memory with futex**
   - Lock-free ring buffer implementation
   - Direct futex wait/wake for efficient thread synchronization
   - Atomic operations for coordination

## Building

```bash
# Build with make
make

# Clean build files
make clean
```

## Running Benchmarks

### Unix Domain Socket

```bash
# In terminal 1
./ipc_benchmark --server --mode uds

# In terminal 2
./ipc_benchmark --client --mode uds
```

Or use the Makefile target:
```bash
make run-uds
```

### Shared Memory with pthread_mutex

```bash
# In terminal 1
./ipc_benchmark --server --mode shm

# In terminal 2
./ipc_benchmark --client --mode shm
```

Or use the Makefile target:
```bash
make run-shm
```

### Lock-free Shared Memory

```bash
# In terminal 1
./ipc_benchmark --server --mode lfshm

# In terminal 2
./ipc_benchmark --client --mode lfshm
```

Or use the Makefile target:
```bash
make run-lfshm
```

### Run All Benchmarks

```bash
make run-all
```

## Benchmark Details

Each benchmark measures:

- **Operations per second**: Number of request/response cycles completed
- **Throughput**: Data transferred in MiB/s
- **CPU usage**: Percentage of CPU time used
- **Latency**: Average, P50, P95, and P99 latency in microseconds

Message sizes vary from 2-4KiB typical, up to a maximum of 128KiB.

## Implementation Design

### 1. Unix Domain Socket with io_uring

```
Client Process                     Server Process
+----------------+                +----------------+
| io_uring Event |                | io_uring Event |
| Loop           |                | Loop           |
|                |                |                |
| - IORING_OP_   |<--UDS Pipe---->| - IORING_OP_   |
|   SEND/RECV    |                |   SEND/RECV    |
+----------------+                +----------------+
```

### 2. Shared Memory with pthread_mutex and Thread

```
Client Process                           Server Process
+----------------+                      +----------------+
| io_uring Event |                      | io_uring Event |
| Loop           |                      | Loop           |
|                |                      |                |
| - IORING_OP_   |<---eventfd events--->| - IORING_OP_   |
|   READ(eventfd)|                      |   READ(eventfd)|
+----------------+                      +----------------+
        ^                                      ^
        |                                      |
+----------------+                      +----------------+
| Thread waiting |                      | Thread waiting |
| on cond var    |                      | on cond var    |
+----------------+                      +----------------+
        ^                                      ^
        |                                      |
+----------------+                      +----------------+
| memfd shared   |<----Ring Buffer----->| memfd shared   |
| memory region  |   with pthread_mutex | memory region  |
+----------------+                      +----------------+
```

### 3. Linux-optimized Shared Memory with futex

```
Client Process                           Server Process
+----------------+                      +----------------+
| Direct futex   |<----Atomic counters->| Direct futex   |
| integration    |                      | integration    |
+----------------+                      +----------------+
        ^                                      ^
        |                                      |
+----------------+                      +----------------+
| memfd shared   |<----Lock-free-------->| memfd shared   |
| memory region  |     Ring Buffer      | memory region  |
+----------------+                      +----------------+
```

## System Requirements

- Linux kernel 5.1+ (for io_uring support)
- liburing development package
- pthread and rt libraries

## Purpose

This benchmark demonstrates the performance trade-offs when using different IPC mechanisms, particularly focusing on the overhead introduced by integrating them with an io_uring based event loop.

The shared memory implementation specifically shows how a traditional mutex-based approach with a dedicated thread affects performance compared to direct socket I/O with io_uring, helping developers make informed decisions about their IPC strategy.

## License

MIT License