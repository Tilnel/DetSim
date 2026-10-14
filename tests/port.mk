# Run with make -f Makefile -f tests/port.mk NP=2 check-port.
.PHONY: check-port
check-port: $(BUILD_DIR)/tests/port_integration
	$(BUILD_DIR)/tests/port_integration

$(BUILD_DIR)/tests/port_integration: $(BUILD_DIR)/tests/port_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/port_integration.d
