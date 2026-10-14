# Run with make -f Makefile -f tests/exec.mk NP=1 check-exec.
.PHONY: check-exec
check-exec: $(BUILD_DIR)/tests/exec_integration
	$(BUILD_DIR)/tests/exec_integration

$(BUILD_DIR)/tests/exec_integration: $(BUILD_DIR)/tests/exec_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/exec_integration.d
