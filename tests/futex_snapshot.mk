.PHONY: check-futex-snapshot
check-futex-snapshot: $(BUILD_DIR)/tests/futex_snapshot_integration
	timeout --kill-after=5s 60s $(BUILD_DIR)/tests/futex_snapshot_integration

$(BUILD_DIR)/tests/futex_snapshot_integration: $(BUILD_DIR)/tests/futex_snapshot_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/futex_snapshot_integration.d
