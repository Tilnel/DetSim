.PHONY: check-store-cache
check-store-cache: $(BUILD_DIR)/tests/store_cache_integration
	$(BUILD_DIR)/tests/store_cache_integration

$(BUILD_DIR)/tests/store_cache_integration: $(BUILD_DIR)/tests/store_cache_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -Wl,--wrap=_ZN16StateStorePacked8compressERKSt6vectorIhSaIhEE -Wl,--wrap=fsync -o $@

-include $(BUILD_DIR)/tests/store_cache_integration.d
