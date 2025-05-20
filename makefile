# Compiler and flags
CC := gcc
CFLAGS := -Wall -Wextra -O2 -pthread

# Detect operating system
UNAME_S := $(shell uname -s)

# Platform-specific settings
ifeq ($(UNAME_S),Linux)
    LDFLAGS := -lpthread -lrt
    SRC := ipc_benchmark.c uds.c shm.c lfshm.c bshm.c benchmark.c interrupt.c
else
    LDFLAGS := -lpthread
    SRC := ipc_benchmark.c uds.c shm.c bshm.c benchmark.c interrupt.c
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
run-uds: $(TARGET)
	$(call run_benchmark,uds)

run-shm: $(TARGET)
	$(call run_benchmark,shm)

run-bshm: $(TARGET)
	$(call run_benchmark,bshm)

# Lock-free shared memory benchmark - Linux only
run-lfshm: $(TARGET)
ifeq ($(UNAME_S),Linux)
	$(call run_benchmark,lfshm)
else
	@echo "Lock-free shared memory mode is only supported on Linux"
	@exit 0
endif

# Run all benchmarks
run-all:
	@printf "## IPC Benchmark Suite\n\n"
ifeq ($(UNAME_S),Linux)
	$(MAKE) run-uds && \
	$(MAKE) run-shm && \
	$(MAKE) run-bshm && \
	$(MAKE) run-lfshm
else
	$(MAKE) run-uds && \
	$(MAKE) run-shm && \
	$(MAKE) run-bshm
endif
	@printf "\nIPC Benchmark Suite Complete\n\n"

# Show help
help:
	@echo "Available targets:"
	@echo "  all       - Build the benchmark executable"
	@echo "  clean     - Remove compiled files"
	@echo "  run-uds   - Run Unix Domain Socket benchmark"
	@echo "  run-shm   - Run Shared Memory with pthread_mutex benchmark"
	@echo "  run-bshm  - Run Blocking Shared Memory benchmark"
ifeq ($(UNAME_S),Linux)
	@echo "  run-lfshm - Run Lock-free Shared Memory benchmark (Linux only)"
endif
	@echo "  run-all   - Run all benchmarks sequentially"

.PHONY: all clean run-uds run-shm run-bshm run-lfshm run-all help   