# Makefile for Distributed File System
# Compiler and flags
CC = gcc
CFLAGS = -std=c11 -g -pthread -Wall -Wextra -Iinclude

# Directories
SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
INC_DIR = include

# Source files
COMMON_SRC = $(SRC_DIR)/common/socket_utils.c
NS_SRC = $(SRC_DIR)/nameserver/nameserver.c
SS_SRC = $(SRC_DIR)/storageserver/storageserver.c
CLIENT_SRC = $(SRC_DIR)/client/client.c

# Object files
COMMON_OBJ = $(OBJ_DIR)/socket_utils.o
NS_OBJ = $(OBJ_DIR)/nameserver.o
SS_OBJ = $(OBJ_DIR)/storageserver.o
CLIENT_OBJ = $(OBJ_DIR)/client.o

# Executables
NS_BIN = $(BIN_DIR)/nameserver
SS_BIN = $(BIN_DIR)/storageserver
CLIENT_BIN = $(BIN_DIR)/client

# Default target: build all executables
all: $(NS_BIN) $(SS_BIN) $(CLIENT_BIN)

# Build nameserver
$(NS_BIN): $(NS_OBJ) $(COMMON_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

# Build storageserver
$(SS_BIN): $(SS_OBJ) $(COMMON_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

# Build client
$(CLIENT_BIN): $(CLIENT_OBJ) $(COMMON_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

# Compile source files to object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/nameserver/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/storageserver/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/client/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/common/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Create directories if they don't exist
$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Clean build artifacts
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

# Rebuild everything
rebuild: clean all

# Run targets (convenient shortcuts)
run-ns: $(NS_BIN)
	./$(NS_BIN)

run-ss: $(SS_BIN)
	./$(SS_BIN)

run-client: $(CLIENT_BIN)
	./$(CLIENT_BIN)

.PHONY: all clean rebuild run-ns run-ss run-client
