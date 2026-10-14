.PHONY: check-store-packed
check-store-packed: $(BUILD_DIR)/tests/store_packed_integration
	timeout --kill-after=5s 60s $(BUILD_DIR)/tests/store_packed_integration
$(BUILD_DIR)/tests/store_packed_integration: $(BUILD_DIR)/tests/store_packed_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -Wl,--wrap=write -Wl,--wrap=pread -Wl,--wrap=fsync -o $@
-include $(BUILD_DIR)/tests/store_packed_integration.d
