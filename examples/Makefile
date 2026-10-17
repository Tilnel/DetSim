# Examples Makefile
# Build all example programs

EXAMPLES = bakery brk bt cast_test complex expr_test fs hello loop loop_inf malloc member raft simple_complex write

.PHONY: all clean $(EXAMPLES)

all: $(EXAMPLES)

$(EXAMPLES):
	@echo "Building $@..."
	$(MAKE) -C $@

clean:
	@for dir in $(EXAMPLES); do \
		echo "Cleaning $$dir..."; \
		$(MAKE) -C $$dir clean; \
	done
