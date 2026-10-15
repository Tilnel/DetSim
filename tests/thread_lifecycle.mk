.PHONY: check-thread-lifecycle
check-thread-lifecycle: $(BUILD_DIR)/tests/thread_lifecycle_integration
	timeout --kill-after=5s 65s $(BUILD_DIR)/tests/thread_lifecycle_integration
	timeout --kill-after=5s 65s $(BUILD_DIR)/tests/thread_lifecycle_integration --tiny-cache
	DETSIM_SNAPSHOT_BACKEND=pages timeout --kill-after=5s 65s $(BUILD_DIR)/tests/thread_lifecycle_integration
	DETSIM_SNAPSHOT_BACKEND=pages timeout --kill-after=5s 65s $(BUILD_DIR)/tests/thread_lifecycle_integration --tiny-cache

$(BUILD_DIR)/tests/thread_lifecycle_integration: $(BUILD_DIR)/tests/thread_lifecycle_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/thread_lifecycle_integration.d
