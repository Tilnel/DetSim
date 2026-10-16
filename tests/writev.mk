.PHONY: check-writev
check-writev: $(BUILD_DIR)/tests/writev_integration
	timeout --kill-after=5s 45s $<

$(BUILD_DIR)/tests/writev_integration: $(BUILD_DIR)/tests/writev_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/writev_integration.d
