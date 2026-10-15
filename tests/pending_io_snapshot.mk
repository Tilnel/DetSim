.PHONY: check-pending-io-snapshot
check-pending-io-snapshot: $(BUILD_DIR)/tests/pending_io_snapshot_integration
	@for backend in full pages; do \
	  for cache in '' '--tiny-cache'; do \
	    DETSIM_SNAPSHOT_BACKEND=$$backend timeout --kill-after=5s 45s $< $$cache || exit $$?; \
	  done; \
	done

$(BUILD_DIR)/tests/pending_io_snapshot_integration: $(BUILD_DIR)/tests/pending_io_snapshot_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/pending_io_snapshot_integration.d
