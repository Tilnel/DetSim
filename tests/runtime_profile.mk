.PHONY: check-runtime-profile
check-runtime-profile: $(BUILD_DIR)/tests/runtime_profile_integration
	$(BUILD_DIR)/tests/runtime_profile_integration

$(BUILD_DIR)/tests/runtime_profile_integration: $(BUILD_DIR)/tests/runtime_profile_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/runtime_profile_integration.d
