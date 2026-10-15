.PHONY: check-thread-altstack
check-thread-altstack: $(BUILD_DIR)/tests/thread_altstack_integration
	@for backend in full pages; do \
	    for cache in '' '--tiny-cache'; do \
	      DETSIM_SNAPSHOT_BACKEND=$$backend timeout --kill-after=5s 65s $< $$cache || exit $$?; \
	  done; \
	done

$(BUILD_DIR)/tests/thread_altstack_integration: $(BUILD_DIR)/tests/thread_altstack_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/thread_altstack_integration.d
