.PHONY: check-pending-io
check-pending-io: $(BUILD_DIR)/tests/pending_io_integration
	timeout --kill-after=5s 60s $<
$(BUILD_DIR)/tests/pending_io_integration: $(BUILD_DIR)/tests/pending_io_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@
-include $(BUILD_DIR)/tests/pending_io_integration.d
