# Run with make -f Makefile -f tests/thread_carrier.mk NP=1 check-thread-carrier.
.PHONY: check-thread-carrier
check-thread-carrier: $(BUILD_DIR)/tests/thread_carrier_integration
	$(BUILD_DIR)/tests/thread_carrier_integration

$(BUILD_DIR)/tests/thread_carrier_integration: $(BUILD_DIR)/tests/thread_carrier_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/thread_carrier_integration.d
