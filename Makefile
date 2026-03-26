.PHONY: all debug release wc clean clear benchmark

CC = ccache gcc
CXX = ccache g++
LD = ccache g++ -L/usr/local/lib

# Build directory
BUILD_DIR = build

# Include paths
INCLUDES = -I. -Isrc -Isrc/core -Isrc/subsys -Isrc/utils -Isrc/ui -Ithird_party -Iexamples/redisraft/plugins

CXXFLAGS ?= $(INCLUDES) -MMD -MP -std=gnu++2a -fno-stack-protector -O3 -g -msse4.2 -D_FORTIFY_SOURCE=0 -fno-omit-frame-pointer \
						-mavx2 -mbmi -mbmi2
LDFLAGS = -g -ldwarf -lreadline -ldw -lzstd -lxxhash -rdynamic \
				-lunwind-ptrace -lunwind-x86_64 -lunwind -lelf -lfmt \
				-lncurses -lpthread -fno-omit-frame-pointer

# Source files with new directory structure
SRCS = src/main.cpp\
       src/core/scheduler.cpp src/core/guest.cpp src/core/monitor.cpp src/core/state.cpp src/core/config.cpp src/core/syscall_fmt.cpp \
       src/core/dwarf.cpp src/core/state_store.cpp src/core/state_store_packed.cpp src/core/sysstate_store.cpp \
       src/subsys/serialize.cpp src/subsys/sockstate.cpp src/subsys/fsstate.cpp src/subsys/emu.cpp src/subsys/fd_manager.cpp \
       src/utils/utils.cpp src/utils/expr.cpp src/utils/expr_ast.cpp \
       src/utils/expr_lexer.cpp src/utils/expr_parser.cpp \
       src/utils/resolve.cpp src/utils/crc32.cpp src/utils/file_lock.cpp \
       examples/redisraft/plugins/raft_msg_parser.cpp \
       src/ui/ncurses_ui.cpp src/ui/log_wrapper.cpp src/core/state_transition.cpp

# Object files in build directory
OBJS = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS = $(patsubst %.cpp,$(BUILD_DIR)/%.d,$(SRCS))

# Lex/Yacc generated files
LEX_SRC = src/utils/expr_lexer.cpp
YACC_SRC = src/utils/expr_parser.cpp
YACC_HDR = src/utils/expr_parser.hpp

all: release

release: CXXFLAGS = $(INCLUDES) -MMD -MP -std=gnu++2a -fno-stack-protector -O3 -msse4.2 -mavx2 -mbmi -mbmi2
release: tracer

debug: CXXFLAGS = $(INCLUDES) -MMD -MP -std=gnu++2a -fno-stack-protector -O3 -g -msse4.2 -mavx2 -mbmi -mbmi2
debug: LDFLAGS += 
debug: tracer

tracer: $(OBJS) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) -o tracer
	-mkdir memory sstate

$(BUILD_DIR)/nr2call.o: nr2call.c | $(BUILD_DIR)
	gcc -c $< -O3 -g -o $@

$(LEX_SRC): src/utils/expr_lexer.l $(YACC_HDR)
	flex -o $@ $<

$(YACC_SRC) $(YACC_HDR): src/utils/expr_parser.y
	bison -d -o $(YACC_SRC) $<

# Compile C++ files to build directory
$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	@if [ -z "$(NP)" ]; then \
		echo "NP not set"; exit 1; \
	fi
	@mkdir -p $(dir $@)
	@echo "Compiling $<"
	@start=$$(date +%s.%N); \
	$(CXX) $< $(CXXFLAGS) -c -o $@ -DNP=$(NP); \
	end=$$(date +%s.%N); \
	dur=$$(echo "$$end - $$start" | bc); \
	echo "Compiled $< in $$dur seconds"

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)/src/core $(BUILD_DIR)/src/subsys $(BUILD_DIR)/src/utils $(BUILD_DIR)/src/ui $(BUILD_DIR)/examples/redisraft/plugins

-include $(DEPS)

wc:
	@find src -name "*.[ch]" -o -name "*.cpp" | xargs wc

clear:
	-/bin/rm -rf memory sstate
	-mkdir memory sstate

clean:
	-/bin/rm -rf $(BUILD_DIR) tracer *.so
	-/bin/rm -f $(LEX_SRC) $(YACC_SRC) $(YACC_HDR) src/utils/expr_parser.output

# --- Benchmark Targets ---
# Exclude the main tracer entry point from the object list for linking the benchmark
TRACER_OBJS_NO_MAIN = $(filter-out $(BUILD_DIR)/src/main.o, $(OBJS))

# StateStore benchmark
BENCH1_SRC = perf/state_store_benchmark.cpp
BENCH1_OBJ = $(BUILD_DIR)/perf/state_store_benchmark.o
BENCH1_EXE = ss_bench

# SysStateStore benchmark  
BENCH2_SRC = perf/sysstate_store_benchmark.cpp
BENCH2_OBJ = $(BUILD_DIR)/perf/sysstate_store_benchmark.o
BENCH2_EXE = sys_bench

# XXHash benchmark
BENCH3_SRC = perf/xxhash_benchmark.cpp
BENCH3_OBJ = $(BUILD_DIR)/perf/xxhash_benchmark.o
BENCH3_EXE = xxh_bench

benchmark: $(BENCH1_EXE) $(BENCH2_EXE) $(BENCH3_EXE)

$(BENCH1_EXE): $(BENCH1_OBJ) $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	@echo "==> Linking Benchmark: $(BENCH1_EXE)"
	$(LD) $^ $(LDFLAGS) -o $@

$(BENCH2_EXE): $(BENCH2_OBJ) $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	@echo "==> Linking Benchmark: $(BENCH2_EXE)"
	$(LD) $^ $(LDFLAGS) -o $@

$(BENCH3_EXE): $(BENCH3_OBJ) $(BUILD_DIR)/nr2call.o
	@echo "==> Linking Benchmark: $(BENCH3_EXE)"
	$(CXX) $^ -O3 -o $@

# Compile benchmark files to build directory
$(BUILD_DIR)/perf/%.o: perf/%.cpp | $(BUILD_DIR)
	@if [ -z "$(NP)" ]; then \
		echo "NP not set"; exit 1; \
	fi
	@mkdir -p $(dir $@)
	@echo "Compiling benchmark $<"
	@start=$$(date +%s.%N); \
	$(CXX) $< $(CXXFLAGS) -c -o $@ -DNP=$(NP); \
	end=$$(date +%s.%N); \
	dur=$$(echo "$$end - $$start" | bc); \
	echo "Compiled $< in $$dur seconds"

# --- Tool Targets ---
TOOLS_DIR = tools

# Tool executables
SSTATE_QUERY_SRC = $(TOOLS_DIR)/sstate_query.cpp
SSTATE_QUERY_OBJ = $(BUILD_DIR)/$(TOOLS_DIR)/sstate_query.o
SSTATE_QUERY_EXE = $(TOOLS_DIR)/sstate_query

VERIFY_SSTATE_SRC = $(TOOLS_DIR)/verify_sstate.cpp
VERIFY_SSTATE_OBJ = $(BUILD_DIR)/$(TOOLS_DIR)/verify_sstate.o
VERIFY_SSTATE_EXE = $(TOOLS_DIR)/verify_sstate

# Tool dependencies - need UI objects too
TOOL_DEPS = $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o \
            $(BUILD_DIR)/src/ui/ncurses_ui.o $(BUILD_DIR)/src/ui/log_wrapper.o

tools: $(SSTATE_QUERY_EXE) $(VERIFY_SSTATE_EXE)

$(SSTATE_QUERY_EXE): $(SSTATE_QUERY_OBJ) $(TOOL_DEPS)
	@echo "==> Linking Tool: $(SSTATE_QUERY_EXE)"
	$(LD) $^ $(LDFLAGS) -o $@

$(VERIFY_SSTATE_EXE): $(VERIFY_SSTATE_OBJ) $(TOOL_DEPS)
	@echo "==> Linking Tool: $(VERIFY_SSTATE_EXE)"
	$(LD) $^ $(LDFLAGS) -o $@

# Compile tool files to build directory
$(BUILD_DIR)/$(TOOLS_DIR)/%.o: $(TOOLS_DIR)/%.cpp | $(BUILD_DIR)
	@if [ -z "$(NP)" ]; then \
		echo "NP not set"; exit 1; \
	fi
	@mkdir -p $(dir $@)
	@echo "Compiling tool $<"
	$(CXX) $< $(CXXFLAGS) -c -o $@ -DNP=$(NP)
