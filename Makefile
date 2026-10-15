# GNU Make; NP is a compile-time part of the state layout.
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.PHONY: all debug release tracer test check check-build-contracts help FORCE \
        wc clean clear benchmark tools pgo-generate pgo-use pgo-clean \
        check-integration check-scenarios check-redis

ifneq ($(filter benchmark-redisraft,$(MAKECMDGOALS)),)
NP ?= 3
endif

ifneq ($(filter check-accept check-redis check-port check-identity check-capture check-io-boundary check-state-view check-plugins check-runtime-profile check-syscall-policy check-process-state check-pending-io check-pending-io-snapshot check-getdents,$(MAKECMDGOALS)),)
NP ?= 2
endif

# Integration fixtures use one node unless explicitly overridden.
ifneq ($(filter check-integration check-dwarf check-elf check-scenarios check-cases audit-runtimes check-xstate check-exec check-state-io check-mappings check-futex-snapshot check-futex check-thread-control check-thread-identity check-thread-carrier check-thread-lifecycle check-thread-tid check-thread-signal-mask check-thread-robust check-pending-syscall check-storage check-store-cache check-store-packed check-store-metadata check-store-tools benchmark-storage check-soft-dirty check-page-snapshot check-vfs-content check-thread-altstack check-dfs-residency check-history-presentation,$(MAKECMDGOALS)),)
NP ?= 1
endif

# Honor standard CC/CXX overrides. ccache is optional (CXX="ccache g++").
ifeq ($(origin CC),default)
CC = gcc
endif
ifeq ($(origin CXX),default)
CXX = g++
endif
LD = $(CXX)
MODE := $(if $(filter debug,$(MAKECMDGOALS)),debug,release)
ifneq ($(words $(filter debug release,$(MAKECMDGOALS))),0)
ifneq ($(words $(filter debug release,$(MAKECMDGOALS))),1)
$(error Select debug or release in separate make invocations)
endif
endif
BUILD_DIR = build/$(MODE)/np$(NP)
INCLUDES = -I. -Isrc -Isrc/core -Isrc/core/engine -Isrc/core/syscall -Isrc/core/scheduler -Isrc/core/state -Isrc/subsys -Isrc/subsys/fs -Isrc/subsys/net -Isrc/subsys/sync -Isrc/utils -Isrc/ui -Isrc/ui/cli -Ithird_party -Ithird_party/cereal/include
PROJECT_CPPFLAGS = $(INCLUDES) -DNP=$(NP)
CXXFLAGS ?= -std=gnu++20 -fno-stack-protector -O3 -msse4.2 -mavx2 -mbmi -mbmi2 $(if $(filter debug,$(MODE)),-g,)
CFLAGS ?= -O3 -g
LDFLAGS ?= -g -rdynamic -fno-omit-frame-pointer
LDLIBS = -ldwarf -lreadline -ldw -lzstd -lxxhash -lunwind-ptrace -lunwind-x86_64 -lunwind -lelf -lfmt -lncurses -lpthread

SRCS = src/main.cpp\
       src/core/scheduler.cpp src/core/guest.cpp src/core/monitor.cpp src/core/config.cpp src/core/syscall_fmt.cpp src/core/resolve.cpp \
       src/core/scheduler/exploration.cpp \
       src/core/dwarf.cpp src/core/plugins.cpp src/core/runtime_profile.cpp src/core/state/state_view.cpp \
       src/core/state/state.cpp src/core/state/page_snapshot.cpp src/core/state/page_capture.cpp src/core/state/xstate.cpp src/core/state/state_store.cpp src/core/state/state_store_packed.cpp src/core/state/sysstate_store.cpp src/core/state/state_transition.cpp src/core/state/serialize.cpp \
       src/core/engine/pending_syscall.cpp src/core/engine/thread_manager.cpp src/core/engine/thread_control.cpp src/core/engine/thread_carrier.cpp src/core/engine/thread_pool.cpp src/core/engine/signal_handler.cpp src/core/engine/exec_engine.cpp \
       src/core/syscall/dispatcher.cpp src/core/syscall/handlers.cpp \
       src/subsys/net/sockstate.cpp src/subsys/net/emu.cpp \
       src/subsys/fs/fsstate.cpp src/subsys/fs/fd_manager.cpp src/subsys/io.cpp src/subsys/pending_io.cpp \
       src/subsys/sync/futexstate.cpp src/subsys/process/process_state.cpp \
       src/utils/utils.cpp src/utils/expr.cpp src/utils/expr_ast.cpp \
       src/utils/expr_lexer.cpp src/utils/expr_parser.cpp \
       src/utils/file_lock.cpp \
       src/ui/ncurses_ui.cpp src/ui/log_wrapper.cpp src/ui/cli/commands.cpp

OBJS = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SRCS))
LEX_SRC = src/utils/expr_lexer.cpp
YACC_SRC = src/utils/expr_parser.cpp
YACC_HDR = src/utils/expr_parser.hpp

all release debug: tracer

# Relink the selected configuration even when returning to older cached objects.
tracer: $(OBJS) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

# A content stamp invalidates objects when compiler or flags change. FORCE runs
# the comparison each time; unchanged content preserves the timestamp.
FORCE:

$(BUILD_DIR)/.config: FORCE
	@case '$(NP)' in ''|*[!0-9]*|0*) echo 'NP must be a positive integer (e.g. make NP=1)' >&2; exit 1;; esac
	@mkdir -p $(dir $@)
	@printf '%s\n' 'CC=$(CC)' 'CXX=$(CXX)' 'CPPFLAGS=$(CPPFLAGS)' 'PROJECT_CPPFLAGS=$(PROJECT_CPPFLAGS)' 'CFLAGS=$(CFLAGS)' 'CXXFLAGS=$(CXXFLAGS)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(BUILD_DIR)/%.o: %.cpp $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/nr2call.o: nr2call.c $(BUILD_DIR)/.config
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(LEX_SRC): src/utils/expr_lexer.l $(YACC_HDR)
	flex -o $@ $<

# Grouped outputs prevent concurrent bison invocations under make -j.
$(YACC_SRC) $(YACC_HDR) &: src/utils/expr_parser.y
	bison -d -o $(YACC_SRC) $<

-include $(OBJS:.o=.d) $(BUILD_DIR)/nr2call.d

TRACER_OBJS_NO_MAIN = $(filter-out $(BUILD_DIR)/src/main.o,$(OBJS))
benchmark: ss_bench sys_bench xxh_bench
.PHONY: ss_bench sys_bench xxh_bench tools/sstate_query tools/verify_sstate
ss_bench: $(BUILD_DIR)/perf/state_store_benchmark.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@
sys_bench: $(BUILD_DIR)/perf/sysstate_store_benchmark.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@
xxh_bench: $(BUILD_DIR)/perf/xxhash_benchmark.o $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) -o $@
tools: tools/sstate_query tools/verify_sstate
tools/sstate_query: $(BUILD_DIR)/tools/sstate_query.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@
tools/verify_sstate: $(BUILD_DIR)/tools/verify_sstate.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@
-include $(wildcard $(BUILD_DIR)/perf/*.d $(BUILD_DIR)/tools/*.d)

test:
	$(MAKE) -f tests/Makefile test CC="$(CC)" CXX="$(CXX)"
check-build-contracts:
	python3 tests/build_contract_test.py
# Component/build checks only; does not assert ptrace/exploration correctness.
check: test check-build-contracts

# Optional fragments are absent in the disposable build-contract fixtures.
-include tests/dwarf.mk tests/elf.mk tests/accept.mk tests/xstate.mk tests/exec.mk tests/state_io.mk tests/port.mk tests/identity.mk tests/mapping.mk tests/futex_snapshot.mk tests/futex.mk tests/storage.mk tests/store_cache.mk tests/store_packed.mk tests/redisraft.mk tests/capture.mk tests/soft_dirty.mk tests/page_snapshot.mk tests/io_boundary.mk tests/framework.mk tests/runtime_profile.mk tests/syscall_policy.mk tests/process_state.mk tests/thread_control.mk tests/thread_identity.mk tests/thread_carrier.mk tests/thread_lifecycle.mk tests/thread_tid.mk tests/thread_signal_mask.mk tests/thread_robust.mk tests/pending_syscall.mk tests/pending_io.mk tests/pending_io_snapshot.mk tests/vfs_content.mk tests/thread_altstack.mk tests/getdents.mk tests/history_presentation.mk
check-integration: check-dwarf check-elf

.PHONY: check-dfs-residency
check-dfs-residency: tracer
	@test "$(NP)" = 1 || { echo 'check-dfs-residency requires NP=1' >&2; exit 1; }
	CC="$(CC)" CXX="$(CXX)" python3 tests/dfs_residency.py --tracer ./tracer

# Case-driven acceptance and explicitly non-green runtime admission audit.
.PHONY: check-cases audit-runtimes
check-cases: tracer
	@test "$(NP)" = 1 || { echo 'check-cases requires NP=1' >&2; exit 1; }
	CC="$(CC)" CXX="$(CXX)" python3 examples/cases/run.py --tracer ./tracer --mode checks
	CC="$(CC)" CXX="$(CXX)" python3 tests/case_runner_integration.py --tracer ./tracer

audit-runtimes: tracer
	@test "$(NP)" = 1 || { echo 'audit-runtimes requires NP=1' >&2; exit 1; }
	CC="$(CC)" CXX="$(CXX)" python3 examples/cases/run.py --tracer ./tracer --mode runtimes

# This gate executes native programs via ptrace; keep it separate from make check.
check-scenarios: tracer
	@test "$(NP)" = 1 || { echo 'check-scenarios requires NP=1' >&2; exit 1; }
	CC="$(CC)" DETSIM_TRACER="$(abspath tracer)" python3 tests/tracer_scenarios.py

check-redis: tracer
	@test "$(NP)" = 2 || { echo 'check-redis requires NP=2' >&2; exit 1; }
	CC="$(CC)" python3 tests/redis_scenario.py --tracer "$(abspath tracer)"

help:
	@echo 'make NP=N [release|debug]   Build tracer for N processes'
	@echo 'make check                  Component tests + build regression tests (no ptrace)'
	@echo 'make check-integration      Production DWARF/ELF fixtures (default NP=1; no ptrace)'
	@echo 'make check-cases           NP=1 healthy/mutant cases and replay evidence'
	@echo 'make audit-runtimes        NP=1 language admission matrix (non-green until supported)'
	@echo 'make check-scenarios        Build NP=1 tracer and run isolated ptrace scenarios'
	@echo 'make check-accept           NP=2 remote-memory accept regression'
	@echo 'make check-xstate           FP/SIMD capture, restore and archive regression'
	@echo 'make check-storage         Cache, packed-log and metadata storage regressions'
	@echo 'make benchmark-storage     Build byte-checked hot/pressure/metadata benchmark'
	@echo 'make check-futex-snapshot   Futex branch capture/copy/archive/restore regression'
	@echo 'make check-futex            Futex syscall wait/wake/requeue regression'
	@echo 'make check-exec             Syscall input and automatic-step boundary regression'
	@echo 'make check-state-io         Snapshot format and exact I/O failure regression'
	@echo 'make check-mappings         Private mapping contents and protections regression'
	@echo 'make check-capture         NP=2 full-versus-optimized snapshot equivalence'
	@echo 'make check-soft-dirty      Experimental kernel soft-dirty byte oracle'
	@echo 'make check-page-snapshot   v5/v6 page storage, reads, corruption and reopen'
	@echo 'make check-state-view check-plugins check-runtime-profile  Generic framework regressions (NP=2)'
	@echo 'make check-history-presentation NP=1 cheap current edge and scoped quiet display'
	@echo 'make check-getdents        NP=2 directory streams, file identity and fault ABI'
	@echo 'make check-dfs-residency   NP=1 bounded DFS memory, backtracking and replay'
	@echo 'make check-thread-altstack NP=1 per-task alternate-stack registration and restore'
	@echo 'make check-vfs-content    Shared file payloads, references and cold reopen'
	@echo 'make check-io-boundary     NP=2 virtual FD/socket/pipe/epoll and archive gates'
	@echo 'make check-identity         NP=2 ordered metadata identity regression'
	@echo 'make check-port             NP=2 deterministic virtual TCP ports regression'
	@echo 'make benchmark-redisraft    NP=3 bounded Raft replication and DFS benchmark'
	@echo 'make check-redis            NP=2 Redis SET/GET (requires existing Redis binary and ptrace)'
	@echo 'make NP=N benchmark|tools   Build optional utilities'
	@echo 'make NP=N pgo-generate     Instrument; run workload; then make NP=N pgo-use'
	@echo 'Compiler overrides: CC=/usr/bin/gcc CXX=/usr/bin/g++'

wc:
	@find src -name '*.[ch]' -o -name '*.cpp' | xargs wc -l

# Only explicit clear removes user runtime snapshots.
clear:
	rm -rf memory sstate
	mkdir -p memory sstate

clean:
	rm -rf build
	rm -f tracer ss_bench sys_bench xxh_bench tools/sstate_query tools/verify_sstate

# Both PGO phases use the same paths for GCC profile lookup. A phase stamp
# rebuilds objects in sequence; no parallel clean prerequisite can race them.
PGO_BUILD_DIR = build-pgo/np$(NP)
PGO_PROF_DIR = $(abspath pgo-profiles/np$(NP))
PGO_OBJS = $(patsubst %.cpp,$(PGO_BUILD_DIR)/%.o,$(SRCS))
PGO_NRCALL = $(PGO_BUILD_DIR)/nr2call.o
ifneq ($(words $(filter pgo-generate pgo-use,$(MAKECMDGOALS))),0)
ifneq ($(words $(filter pgo-generate pgo-use,$(MAKECMDGOALS))),1)
$(error Run pgo-generate and pgo-use in separate invocations)
endif
endif
PGO_FLAGS = $(if $(filter pgo-use,$(MAKECMDGOALS)),-fprofile-use -fprofile-correction,-fprofile-generate) -fprofile-dir=$(PGO_PROF_DIR)
pgo-generate pgo-use: $(PGO_OBJS) $(PGO_NRCALL)
	$(LD) $^ $(LDFLAGS) $(PGO_FLAGS) $(LDLIBS) -o tracer-pgo

$(PGO_BUILD_DIR)/.config: FORCE
	@case '$(NP)' in ''|*[!0-9]*|0*) echo 'NP must be a positive integer' >&2; exit 1;; esac
	@mkdir -p $(dir $@) $(PGO_PROF_DIR)
	@printf '%s\n' 'CC=$(CC)' 'CXX=$(CXX)' 'CPPFLAGS=$(CPPFLAGS)' 'PROJECT_CPPFLAGS=$(PROJECT_CPPFLAGS)' 'CXXFLAGS=$(CXXFLAGS)' 'CFLAGS=$(CFLAGS)' 'PGO_FLAGS=$(PGO_FLAGS)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(PGO_BUILD_DIR)/%.o: %.cpp $(PGO_BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(CXXFLAGS) $(PGO_FLAGS) -MMD -MP -c $< -o $@
$(PGO_BUILD_DIR)/nr2call.o: nr2call.c $(PGO_BUILD_DIR)/.config
	$(CC) $(CFLAGS) $(PGO_FLAGS) -MMD -MP -c $< -o $@
-include $(PGO_OBJS:.o=.d) $(PGO_NRCALL:.o=.d)

pgo-clean:
	rm -rf build-pgo pgo-profiles
	rm -f tracer-pgo
