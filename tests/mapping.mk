.PHONY: check-mappings
check-mappings: $(BUILD_DIR)/tests/mapping_integration
	$(BUILD_DIR)/tests/mapping_integration

$(BUILD_DIR)/tests/mapping_integration: $(BUILD_DIR)/tests/mapping_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/mapping_integration.d
