.PHONY: check-syscall-policy
check-syscall-policy: $(BUILD_DIR)/tests/syscall_policy_integration
	timeout --kill-after=5s 60s $(BUILD_DIR)/tests/syscall_policy_integration
$(BUILD_DIR)/tests/syscall_policy_integration: $(BUILD_DIR)/tests/syscall_policy_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@
-include $(BUILD_DIR)/tests/syscall_policy_integration.d
