# Run with make -f Makefile -f tests/thread_identity.mk NP=1 check-thread-identity.
.PHONY: check-thread-identity
check-thread-identity: $(BUILD_DIR)/tests/thread_identity_integration
	$(BUILD_DIR)/tests/thread_identity_integration

$(BUILD_DIR)/tests/thread_identity_integration: $(BUILD_DIR)/tests/thread_identity_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/thread_identity_integration.d
