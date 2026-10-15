# Run with make -f Makefile -f tests/thread_control.mk NP=1 check-thread-control.
.PHONY: check-thread-control
check-thread-control: $(BUILD_DIR)/tests/thread_control_integration
	$(BUILD_DIR)/tests/thread_control_integration

$(BUILD_DIR)/tests/thread_control_integration: $(BUILD_DIR)/tests/thread_control_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -Wl,--wrap=ptrace -o $@

-include $(BUILD_DIR)/tests/thread_control_integration.d
