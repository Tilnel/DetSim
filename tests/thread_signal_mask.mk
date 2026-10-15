.PHONY: check-thread-signal-mask
check-thread-signal-mask: $(BUILD_DIR)/tests/thread_signal_mask_integration
	@for backend in full pages; do \
	  for pending in '' '--shared-pending'; do \
	    for cache in '' '--tiny-cache'; do \
	      DETSIM_SNAPSHOT_BACKEND=$$backend timeout --kill-after=5s 65s $< $$pending $$cache || exit $$?; \
	    done; \
	  done; \
	done

$(BUILD_DIR)/tests/thread_signal_mask_integration: $(BUILD_DIR)/tests/thread_signal_mask_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/thread_signal_mask_integration.d
