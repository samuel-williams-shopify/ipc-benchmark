# Compiler and flags
CC := gcc
CFLAGS := -Wall -Wextra -O2 -pthread

# Detect operating system
UNAME_S := $(shell uname -s)

# Platform-specific settings
ifeq ($(UNAME_S),Linux)
    LDFLAGS := -lpthread -lrt
    SRC := ipc_benchmark.c buds.c nbuds.c nbshm.c bshm.c lfbshm.c lfnbshm.c benchmark.c interrupt.c
else
    LDFLAGS := -lpthread
    SRC := ipc_benchmark.c buds.c nbuds.c nbshm.c bshm.c benchmark.c interrupt.c
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
run-buds: $(TARGET)
	$(call run_benchmark,buds)

run-nbuds: $(TARGET)
	$(call run_benchmark,nbuds)

run-nbshm: $(TARGET)
	$(call run_benchmark,nbshm)

run-bshm: $(TARGET)
	$(call run_benchmark,bshm)

# Lock-free shared memory benchmark - Linux only
run-lfbshm: $(TARGET)
ifeq ($(UNAME_S),Linux)
	$(call run_benchmark,lfbshm)
else
	@echo "Lock-free shared memory mode is only supported on Linux"
	@exit 0
endif

# Lock-free non-blocking shared memory benchmark - Linux only
run-lfnbshm: $(TARGET)
ifeq ($(UNAME_S),Linux)
	$(call run_benchmark,lfnbshm)
else
	@echo "Lock-free non-blocking shared memory mode is only supported on Linux"
	@exit 0
endif

# Run all benchmarks
run-all:
	@printf "## IPC Benchmark Suite\n\n"
ifeq ($(UNAME_S),Linux)
	$(MAKE) run-buds && \
	$(MAKE) run-nbuds && \
	$(MAKE) run-nbshm && \
	$(MAKE) run-bshm && \
	$(MAKE) run-lfbshm && \
	$(MAKE) run-lfnbshm
else
	$(MAKE) run-buds && \
	$(MAKE) run-nbuds && \
	$(MAKE) run-nbshm && \
	$(MAKE) run-bshm
endif
	@printf "\nIPC Benchmark Suite Complete\n\n"

# Show help
help:
	@echo "Available targets:"
	@echo "  all       - Build the benchmark executable"
	@echo "  clean     - Remove compiled files"
	@echo "  run-buds   - Run Blocking Unix Domain Socket benchmark"
	@echo "  run-nbuds  - Run Non-Blocking Unix Domain Socket benchmark (client side only)"
	@echo "  run-nbshm - Run Notification-based Shared Memory benchmark"
	@echo "  run-bshm  - Run Blocking Shared Memory benchmark"
ifeq ($(UNAME_S),Linux)
	@echo "  run-lfbshm - Run Lock-free Blocking Shared Memory benchmark (Linux only)"
	@echo "  run-lfnbshm - Run Lock-free Non-Blocking Shared Memory benchmark (Linux only)"
endif
	@echo "  run-all   - Run all benchmarks sequentially"

.PHONY: all clean run-buds run-nbuds run-nbshm run-bshm run-lfbshm run-lfnbshm run-all help   