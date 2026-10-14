# Run with make -f Makefile -f tests/futex.mk NP=1 check-futex.
.PHONY: check-futex
check-futex: $(BUILD_DIR)/tests/futex_integration
	$(BUILD_DIR)/tests/futex_integration

$(BUILD_DIR)/tests/futex_integration: $(BUILD_DIR)/tests/futex_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/futex_integration.d
