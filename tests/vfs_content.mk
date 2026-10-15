.PHONY: check-vfs-content
check-vfs-content: $(BUILD_DIR)/tests/vfs_content_integration
	timeout --kill-after=5s 60s $<
$(BUILD_DIR)/tests/vfs_content_integration: $(BUILD_DIR)/tests/vfs_content_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -Wl,--wrap=fsync -o $@
-include $(BUILD_DIR)/tests/vfs_content_integration.d
