.PHONY: check-identity
check-identity: $(BUILD_DIR)/tests/identity_integration
	$(BUILD_DIR)/tests/identity_integration

$(BUILD_DIR)/tests/identity_integration: $(BUILD_DIR)/tests/identity_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/identity_integration.d
