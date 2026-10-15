# RedisRaft Example Makefile
# Builds raft server and plugins

NP ?= 3
PLUGIN_CXXFLAGS ?= -std=gnu++20 -O2 -g
DETSIM_DIR = ../..
CFLAGS = -O3 -gdwarf-4 -g -D_FORTIFY_SOURCE=0 -I./raft/include -I$(DETSIM_DIR)/src -I$(DETSIM_DIR)/src/core -I$(DETSIM_DIR)/src/utils -I$(DETSIM_DIR)/src/subsys -DNP=$(NP)
CXXFLAGS = -O3 -gdwarf-4 -g -I./raft/include -I$(DETSIM_DIR)/src -I$(DETSIM_DIR)/src/core -I$(DETSIM_DIR)/src/utils -I$(DETSIM_DIR)/src/subsys -D_FORTIFY_SOURCE=0 -DNP=$(NP)

RAFT_LIB = ./raft/libraft.a

.PHONY: all clean raft plugins

all: raft plugins

raft: $(RAFT_LIB) raft/main.c
	$(CC) $(CFLAGS) raft/main.c $(RAFT_LIB) -o raft/raft

$(RAFT_LIB):
	$(MAKE) -C raft static

plugins: plugins/check.so plugins/choose.so plugins/raft_msg_parser.so

plugins/check.so: plugins/check.cpp ../raft/plugins/check_common.hpp ../../src/api/plugin.h
	$(CXX) $(PLUGIN_CXXFLAGS) -shared -fPIC -I../../src $< -o $@

plugins/choose.so: plugins/choose.cpp ../raft/plugins/choose_common.hpp ../../src/api/plugin.h
	$(CXX) $(PLUGIN_CXXFLAGS) -shared -fPIC -I../../src $< -o $@

plugins/raft_msg_parser.so: plugins/describe.cpp plugins/raft_msg_parser.cpp plugins/raft_msg_parser.h ../../src/api/plugin.h
	$(CXX) $(PLUGIN_CXXFLAGS) -shared -fPIC -I../../src plugins/describe.cpp plugins/raft_msg_parser.cpp -o $@

clean:
	$(MAKE) -C raft clean
	rm -f raft/raft plugins/*.so
