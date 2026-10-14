# Run with make -f Makefile -f tests/xstate.mk NP=1 check-xstate.
.PHONY: check-xstate
check-xstate: $(BUILD_DIR)/tests/xstate_integration
	$(BUILD_DIR)/tests/xstate_integration

$(BUILD_DIR)/tests/xstate_integration: $(BUILD_DIR)/tests/xstate_integration.o $(BUILD_DIR)/src/core/state/xstate.o
	$(LD) $^ $(LDFLAGS) -o $@

-include $(BUILD_DIR)/tests/xstate_integration.d
