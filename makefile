# Compiler and flags
CC := gcc
CFLAGS := -Wall -Wextra -O2 -pthread

# Detect operating system
UNAME_S := $(shell uname -s)

# Platform-specific settings
ifeq ($(UNAME_S),Linux)
    LDFLAGS := -lpthread -lrt
    SRC := ipc_benchmark.c shm_notification.c shm_blocking.c lfshm_blocking.c lfshm_nonblocking.c benchmark.c interrupt.c uds_blocking.c uds_nonblocking.c
else
    LDFLAGS := -lpthread
    SRC := ipc_benchmark.c shm_notification.c shm_blocking.c benchmark.c interrupt.c uds_blocking.c uds_nonblocking.c
endif

# Target executable
TARGET := ipc_benchmark

# Object files
OBJ := $(SRC:.c=.o)

# Default target
all: $(TARGET)

# Link the executable
$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

# Compile source files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up
clean:
	rm -f $(OBJ) $(TARGET)

# Helper function to run a benchmark safely
define run_benchmark
	@printf "\n## Running $(1) Benchmark\n\n"; \
	trap 'kill $$SERVER_PID 2>/dev/null || true' EXIT; \
	./$(TARGET) --server --mode $(1) & \
	SERVER_PID=$$!; \
	printf "Server PID: %d\n" $$SERVER_PID; \
	sleep 2; \
	./$(TARGET) --client --mode $(1); \
	RESULT=$$?; \
	kill $$SERVER_PID 2>/dev/null || true; \
	wait $$SERVER_PID 2>/dev/null || true; \
	printf "$(1) Benchmark Complete\n\n"; \
	exit $$RESULT
endef

# Run the benchmarks
run-uds-blocking: $(TARGET)
	$(call run_benchmark,uds-blocking)

run-uds-nonblocking: $(TARGET)
	$(call run_benchmark,uds-nonblocking)

run-shm-notification: $(TARGET)
	$(call run_benchmark,shm-notification)

run-shm-blocking: $(TARGET)
	$(call run_benchmark,shm-blocking)

# Lock-free shared memory benchmark - Linux only
run-lfshm-blocking: $(TARGET)
ifeq ($(UNAME_S),Linux)
	$(call run_benchmark,lfshm-blocking)
else
	@echo "Lock-free shared memory mode is only supported on Linux"
	@exit 0
endif

# Lock-free non-blocking shared memory benchmark - Linux only
run-lfshm-nonblocking: $(TARGET)
ifeq ($(UNAME_S),Linux)
	$(call run_benchmark,lfshm-nonblocking)
else
	@echo "Lock-free non-blocking shared memory mode is only supported on Linux"
	@exit 0
endif

# Run all benchmarks
run-all:
	@printf "## IPC Benchmark Suite\n\n"
ifeq ($(UNAME_S),Linux)
	$(MAKE) run-uds-blocking && \
	$(MAKE) run-uds-nonblocking && \
	$(MAKE) run-shm-notification && \
	$(MAKE) run-shm-blocking && \
	$(MAKE) run-lfshm-blocking && \
	$(MAKE) run-lfshm-nonblocking
else
	$(MAKE) run-uds-blocking && \
	$(MAKE) run-uds-nonblocking && \
	$(MAKE) run-shm-notification && \
	$(MAKE) run-shm-blocking
endif
	@printf "\nIPC Benchmark Suite Complete\n\n"

# Show help
help:
	@echo "Available targets:"
	@echo "  all       - Build the benchmark executable"
	@echo "  clean     - Remove compiled files"
	@echo "  run-uds-blocking - Run Blocking Unix Domain Socket benchmark"
	@echo "  run-uds-nonblocking - Run Non-Blocking Unix Domain Socket benchmark"
	@echo "  run-shm-notification - Run Notification-based Shared Memory benchmark"
	@echo "  run-shm-blocking  - Run Blocking Shared Memory benchmark"
	@echo "  run-lfshm-blocking - Run Lock-free Blocking Shared Memory benchmark (Linux only)"
	@echo "  run-lfshm-nonblocking - Run Lock-free Non-Blocking Shared Memory benchmark (Linux only)"
	@echo "  run-all   - Run all benchmarks sequentially"

.PHONY: all clean run-uds-blocking run-uds-nonblocking run-shm-notification run-shm-blocking run-lfshm-blocking run-lfshm-nonblocking run-all help   