# IPC Benchmark

A performance comparison of different Inter-Process Communication (IPC) mechanisms.

## Overview

This benchmark compares three different IPC approaches:

1. **Unix Domain Socket**
   - Non-blocking I/O with select()
   - Kernel-managed buffering and synchronization
   - Efficient local communication

2. **Shared Memory with pthread_mutex**
   - Shared memory region with a ring buffer
   - pthread_mutex and condition variables for synchronization 
   - Dedicated thread waiting on mutex, signaling via pipe/eventfd

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

### 1. Unix Domain Socket

```
Client Process                     Server Process
+----------------+                +----------------+
| select() Event |                | select() Event |
| Loop           |                | Loop           |
|                |                |                |
| - Non-blocking |<--UDS Pipe---->| - Non-blocking |
|   send/recv    |                |   send/recv    |
+----------------+                +----------------+
```

### 2. Shared Memory with pthread_mutex and Thread

```
Client Process                           Server Process
+----------------+                      +----------------+
| select() Event |                      | select() Event |
| Loop           |                      | Loop           |
|                |                      |                |
| - READ from    |<---pipe/eventfd----->| - READ from    |
|   notif pipe   |       events         |   notif pipe   |
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
| memfd shared   |<----Lock-free------->| memfd shared   |
| memory region  |     Ring Buffer      | memory region  |
+----------------+                      +----------------+
```

## System Requirements

- Unix-based OS (Linux, macOS, etc.)
- pthread library
- rt library (Linux only)

## Purpose

This benchmark demonstrates the performance trade-offs when using different IPC mechanisms, comparing traditional Unix Domain Sockets with shared memory approaches.

The shared memory implementation specifically shows how a traditional mutex-based approach with a dedicated thread affects performance compared to direct socket I/O, helping developers make informed decisions about their IPC strategy.

## License

MIT License