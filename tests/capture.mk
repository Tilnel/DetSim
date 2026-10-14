.PHONY: check-capture
check-capture: $(BUILD_DIR)/tests/capture_integration
	timeout --kill-after=5s 60s $(BUILD_DIR)/tests/capture_integration
	timeout --kill-after=5s 60s $(BUILD_DIR)/tests/capture_integration --tiny-cache

$(BUILD_DIR)/tests/capture_integration: $(BUILD_DIR)/tests/capture_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/capture_integration.d
