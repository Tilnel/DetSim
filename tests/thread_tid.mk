.PHONY: check-thread-tid
check-thread-tid: $(BUILD_DIR)/tests/thread_tid_integration
	@for backend in full pages; do \
	  for variant in '' '--clone3'; do \
	    for cache in '' '--tiny-cache'; do \
	      DETSIM_SNAPSHOT_BACKEND=$$backend timeout --kill-after=5s 65s $< $$variant $$cache || exit $$?; \
	    done; \
	  done; \
	done

$(BUILD_DIR)/tests/thread_tid_integration: $(BUILD_DIR)/tests/thread_tid_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/thread_tid_integration.d
