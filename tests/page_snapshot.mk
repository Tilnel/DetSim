.PHONY: check-page-snapshot
check-page-snapshot: $(BUILD_DIR)/tests/page_snapshot_integration
	timeout --kill-after=5s 60s $<

$(BUILD_DIR)/tests/page_snapshot_integration: $(BUILD_DIR)/tests/page_snapshot_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) -Wl,--wrap=fsync $(LDLIBS) -o $@
-include $(BUILD_DIR)/tests/page_snapshot_integration.d
