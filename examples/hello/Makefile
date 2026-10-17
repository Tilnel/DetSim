ifeq ($(origin CC),default)
CC = gcc
endif
CFLAGS ?= -O0 -g -gdwarf-4
BUILD_DIR ?= ../../build/examples/hello

.PHONY: all clean
all: $(BUILD_DIR)/hello

$(BUILD_DIR)/hello: hello.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

clean:
	rm -f $(BUILD_DIR)/hello
