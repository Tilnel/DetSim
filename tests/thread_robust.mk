.PHONY: check-thread-robust
check-thread-robust: $(BUILD_DIR)/tests/thread_robust_integration
	@for backend in full pages; do \
	  for cache in '' '--tiny-cache'; do \
	    DETSIM_SNAPSHOT_BACKEND=$$backend timeout --kill-after=5s 65s $< $$cache || exit $$?; \
	  done; \
	done

$(BUILD_DIR)/tests/thread_robust_integration: $(BUILD_DIR)/tests/thread_robust_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/thread_robust_integration.d
