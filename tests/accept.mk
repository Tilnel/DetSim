# Run with make -f Makefile -f tests/accept.mk NP=2 check-accept.
.PHONY: check-accept
check-accept: $(BUILD_DIR)/tests/accept_integration
	$(BUILD_DIR)/tests/accept_integration

$(BUILD_DIR)/tests/accept_integration: $(BUILD_DIR)/tests/accept_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/accept_integration.d
