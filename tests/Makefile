# Run from the project root: make -f tests/Makefile test
# Existing cereal headers are required; no ptrace, tracees, or NP setting.
ifeq ($(origin CXX),default)
CXX = g++
endif
CXXFLAGS ?= -O0 -g
BUILD_DIR ?= build/tests
TEST_CPPFLAGS = -Isrc -Isrc/core -Isrc/utils -Isrc/ui -Ithird_party/cereal/include
TEST_CXXFLAGS = -std=c++20 -Wall -Wextra
SOURCES = tests/state_components_test.cpp tests/component_log_stubs.cpp \
          src/subsys/fs/fd_manager.cpp src/subsys/sync/futexstate.cpp
OBJECTS = $(addprefix $(BUILD_DIR)/,$(SOURCES:.cpp=.o))
TEST_BIN = $(BUILD_DIR)/state_components_test

.DEFAULT_GOAL := test
.DELETE_ON_ERROR:
.PHONY: all test clean legacy-dwarf test-typedef debug FORCE
all: test

test: $(TEST_BIN)
	./$(TEST_BIN)

# Track compiler and flag changes; preserve timestamp when unchanged.
$(BUILD_DIR)/config: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(CXX) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) $(LDFLAGS) $(LDLIBS)' > $@.tmp
	@cmp -s $@ $@.tmp && rm -f $@.tmp || mv -f $@.tmp $@

$(BUILD_DIR)/%.o: %.cpp $(BUILD_DIR)/config
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP -c $< -o $@

$(TEST_BIN): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

# Local experiments retain their source, but are not a correctness check.
legacy-dwarf test-typedef debug:
	@echo "Legacy DWARF tests are unsupported: repair the fake monitor-state layout and stale interfaces first." >&2
	@exit 2

clean:
	rm -f $(OBJECTS) $(OBJECTS:.o=.d) $(TEST_BIN) $(BUILD_DIR)/config $(BUILD_DIR)/config.tmp

-include $(OBJECTS:.o=.d)
