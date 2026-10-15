# Standalone public-ABI plugins; never build the workload/vendor tree.
CXXFLAGS ?= -std=gnu++20 -O2
.PHONY: all clean
all: check.so choose.so
check.so: check.cpp ../../../src/api/plugin.h
	$(CXX) $(CXXFLAGS) -fPIC -shared -I../../../src $< -o $@
choose.so: choose.cpp ../../../src/api/plugin.h
	$(CXX) $(CXXFLAGS) -fPIC -shared -I../../../src $< -o $@
clean:
	rm -f check.so choose.so
