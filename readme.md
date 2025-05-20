# IPC Benchmark

This project benchmarks different inter-process communication (IPC) mechanisms on Linux, comparing their performance characteristics for message passing between processes.

## IPC Mechanisms

The benchmark compares three different IPC mechanisms:

1. **Unix Domain Sockets (UDS)**
   - Uses stream-oriented Unix domain sockets
   - Server uses blocking I/O for simplicity
   - Client uses non-blocking I/O with select() for robust message handling
   - Provides reliable message delivery with kernel buffering
   - Good for general-purpose IPC with moderate performance requirements

2. **Shared Memory with Mutex**
   - Uses POSIX shared memory with pthread mutexes
   - Implements a ring buffer for message passing
   - Provides high performance with kernel synchronization
   - Good for high-throughput scenarios where processes are on the same machine
   - Requires careful synchronization to prevent data races

3. **Lock-free Shared Memory**
   - Uses POSIX shared memory with atomic operations
   - Implements a lock-free ring buffer
   - Provides highest performance with minimal kernel involvement
   - Best for scenarios requiring maximum throughput
   - Requires careful implementation to ensure correctness

## Benchmark Results

### Unix Domain Sockets (UDS)
- Operations per second: 64,716.50
- Throughput: 191.71 MiB/s
- CPU usage: 70.01%
- Average latency: 8.78 µs
- P50 latency: 8.00 µs
- P95 latency: 11.00 µs
- P99 latency: 16.00 µs

### Shared Memory with Mutex (SHM)
- Operations per second: 31,205.90
- Throughput: 92.43 MiB/s
- CPU usage: 51.20%
- Average latency: 16.89 µs
- P50 latency: 18.00 µs
- P95 latency: 22.00 µs
- P99 latency: 29.00 µs

### Detailed SHM Metrics
- Average Mutex Lock Time: 0.02 µs
- Average Condition Wait Time: 0.00 µs
- Average Notification Time: 1.13 µs
- Average Copy Time: 0.11 µs
- Total Synchronization Overhead: 1.15 µs

### Analysis
- UDS shows better throughput and lower latency compared to SHM
- SHM has lower CPU usage but higher latency due to synchronization overhead
- The synchronization overhead in SHM is primarily from notifications (1.13 µs)
- UDS provides better performance for general-purpose IPC on this system

## Advanced IPC: io_uring with Futex

While not implemented in this benchmark, it's worth noting that modern Linux systems offer another powerful IPC mechanism using io_uring and futex:

- **io_uring with Futex**
  - io_uring can wait on a futex, enabling efficient shared memory IPC
  - Allows processes to sleep on a futex in the kernel, waking up when the futex value changes
  - Can be used to implement a shared memory message queue with kernel-assisted synchronization
  - Provides high performance with minimal context switches
  - Platform-specific to Linux and requires kernel support
  - Example implementation would use:
    - Shared memory for the message buffer
    - Futex for synchronization
    - io_uring to wait on the futex
    - Zero-copy message passing

However, it should be noted that this design is non-trivial.

## Building and Running

```bash
make
make run-all  # Run all benchmarks
make run-uds  # Run Unix Domain Socket benchmark
make run-shm  # Run Shared Memory benchmark
make run-lfshm # Run Lock-free Shared Memory benchmark
```

## Requirements

- Linux operating system
- POSIX shared memory support
- pthread library
- C compiler (gcc or clang)

## Results

- CPU: Apple M4 Pro
- Memory: 48GB (51539607552 bytes)
- OS: macOS 24.4.0

The benchmark measures:
- Message throughput (messages/second)
- Latency (microseconds)
- CPU usage
- Memory usage

### Unix Domain Sockets (UDS)
- Operations per second: 64,716.50
- Throughput: 191.71 MiB/s
- CPU usage: 70.01%
- Average latency: 8.78 µs
- P50 latency: 8.00 µs
- P95 latency: 11.00 µs
- P99 latency: 16.00 µs

### Shared Memory with Mutex (SHM)
- Operations per second: 31,205.90
- Throughput: 92.43 MiB/s
- CPU usage: 51.20%
- Average latency: 16.89 µs
- P50 latency: 18.00 µs
- P95 latency: 22.00 µs
- P99 latency: 29.00 µs

### Detailed SHM Metrics
- Average Mutex Lock Time: 0.02 µs
- Average Condition Wait Time: 0.00 µs
- Average Notification Time: 1.13 µs
- Average Copy Time: 0.11 µs
- Total Synchronization Overhead: 1.15 µs

### Analysis
- UDS shows better throughput and lower latency compared to SHM
- SHM has lower CPU usage but higher latency due to synchronization overhead
- The synchronization overhead in SHM is primarily from notifications (1.13 µs)
- UDS provides better performance for general-purpose IPC on this system

## License

MIT License