# Compiler and flags
CC := gcc
CFLAGS := -Wall -O2
LDFLAGS := -luring -lpthread -lrt

# Target executable
TARGET := ipc_benchmark

# Source files
SRC := ipc_benchmark.c
OBJ := $(SRC:.c=.o)

# Default target
all: $(TARGET)

# Link the executable
$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile source files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up
clean:
	rm -f $(OBJ) $(TARGET)

# Run the benchmarks
run-uds:
	./$(TARGET) --server --mode uds & \
	SERVER_PID=$$!; \
	sleep 1; \
	./$(TARGET) --client --mode uds; \
	kill $$SERVER_PID

run-shm:
	./$(TARGET) --server --mode shm & \
	SERVER_PID=$$!; \
	sleep 1; \
	./$(TARGET) --client --mode shm; \
	kill $$SERVER_PID

run-lfshm:
	./$(TARGET) --server --mode lfshm & \
	SERVER_PID=$$!; \
	sleep 1; \
	./$(TARGET) --client --mode lfshm; \
	kill $$SERVER_PID

# Run all benchmarks
run-all: run-uds run-shm run-lfshm

# Show help
help:
	@echo "Available targets:"
	@echo "  all       - Build the benchmark executable"
	@echo "  clean     - Remove compiled files"
	@echo "  run-uds   - Run Unix Domain Socket benchmark"
	@echo "  run-shm   - Run Shared Memory with pthread_mutex benchmark"
	@echo "  run-lfshm - Run Lock-free Shared Memory benchmark" 
	@echo "  run-all   - Run all benchmarks sequentially"

.PHONY: all clean run-uds run-shm run-lfshm run-all help