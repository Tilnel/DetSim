# Raft example Makefile
# Builds raft server and tracer plugins

NP ?= 3
CFLAGS = -gdwarf-4 -g -D_FORTIFY_SOURCE=0 -static
CXXFLAGS = -gdwarf-4 -g -I . -I ../../src -I ../../src/core -I ../../src/utils -I ../../src/subsys -D_FORTIFY_SOURCE=0 -static

.PHONY: all clean raft raft_dynamic plugins

all: raft raft_dynamic plugins

# Build raft server (delegates to raft subdir Makefile)
raft:
	$(MAKE) -C raft main

# Build raft_dynamic server
raft_dynamic:
	$(MAKE) -C raft raft_dynamic

# Build plugins
plugins: plugins/check.so plugins/choose.so

plugins/check.so: plugins/check.cpp
	$(CXX) -shared -fPIC $(CXXFLAGS) $< -o $@

plugins/choose.so: plugins/choose.cpp
	$(CXX) -shared -fPIC $(CXXFLAGS) $< -o $@

clean:
	$(MAKE) -C raft clean
	rm -f plugins/*.so
