.PHONY: check-state-io
check-state-io: $(BUILD_DIR)/tests/state_io_integration
	$(BUILD_DIR)/tests/state_io_integration

$(BUILD_DIR)/tests/state_io_integration: $(BUILD_DIR)/tests/state_io_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -Wl,--wrap=process_vm_readv,--wrap=process_vm_writev -o $@

-include $(BUILD_DIR)/tests/state_io_integration.d
