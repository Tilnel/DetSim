.PHONY: check-getdents
check-getdents: $(BUILD_DIR)/tests/getdents_integration
	timeout --kill-after=5s 60s $<
$(BUILD_DIR)/tests/getdents_integration: $(BUILD_DIR)/tests/getdents_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -Wl,--wrap=XXH3_64bits -o $@
-include $(BUILD_DIR)/tests/getdents_integration.d
