# Makefile for DWARF parser unit tests
# Run from project root: make -f tests/Makefile

CXX = g++
CC = gcc
CXXFLAGS = -std=c++20 -I. -Isrc -Isrc/core -Isrc/utils -Isrc/subsys -Isrc/ui -O0 -g -DDWARF_TEST_MODE
LDFLAGS = -ldwarf -lfmt -lelf -lunwind -lunwind-x86_64 -lunwind-ptrace

BUILD_DIR = tests

# Test data binary
TEST_DATA_SRC = tests/test_dwarf_data.c
TEST_DATA_BIN = $(BUILD_DIR)/test_dwarf_data

# Test objects
DWARF_OBJ = $(BUILD_DIR)/dwarf.o
TEST_OBJ = $(BUILD_DIR)/dwarf_test.o
STUBS_OBJ = $(BUILD_DIR)/dwarf_test_stubs.o
TYPEDEF_TEST_OBJ = $(BUILD_DIR)/raft_typedef_test.o

# Test executables
TEST_BIN = $(BUILD_DIR)/dwarf_test
TYPEDEF_TEST_BIN = $(BUILD_DIR)/raft_typedef_test

# Default target
all: test

# Compile test data binary with DWARF4 debug info
$(TEST_DATA_BIN): $(TEST_DATA_SRC)
	$(CC) -gdwarf-4 -g -O0 -o $@ $<

# Compile dwarf.cpp
$(DWARF_OBJ): src/core/dwarf.cpp src/core/dwarf_info.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile test
$(TEST_OBJ): tests/dwarf_test.cpp src/core/dwarf_info.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile stubs
$(STUBS_OBJ): tests/dwarf_test_stubs.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile typedef test
$(TYPEDEF_TEST_OBJ): tests/raft_typedef_test.cpp src/core/dwarf_info.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Link test executable
$(TEST_BIN): $(TEST_OBJ) $(STUBS_OBJ) $(DWARF_OBJ)
	$(CXX) $^ $(LDFLAGS) -o $@

# Link typedef test executable
$(TYPEDEF_TEST_BIN): $(TYPEDEF_TEST_OBJ) $(STUBS_OBJ) $(DWARF_OBJ)
	$(CXX) $^ $(LDFLAGS) -o $@

# Run tests
test: $(TEST_BIN) $(TEST_DATA_BIN)
	@echo "Running DWARF parser unit tests..."
	./$(TEST_BIN) $(TEST_DATA_BIN)

test-typedef: $(TYPEDEF_TEST_BIN)
	@echo "Running raft_node_t typedef test..."
	./$(TYPEDEF_TEST_BIN) examples/redisraft/raft/raft

# Debug with gdb
debug: $(TEST_BIN) $(TEST_DATA_BIN)
	gdb --args ./$(TEST_BIN) $(TEST_DATA_BIN)

# Clean
clean:
	rm -f $(TEST_BIN) $(TYPEDEF_TEST_BIN) $(BUILD_DIR)/*.o $(TEST_DATA_BIN)

.PHONY: all test test-typedef debug clean