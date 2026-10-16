.PHONY: check-io-boundary
check-io-boundary: $(BUILD_DIR)/tests/io_boundary_integration $(BUILD_DIR)/tests/fs_boundary_extra $(BUILD_DIR)/tests/network_boundary_extra
	timeout --kill-after=5s 60s $(BUILD_DIR)/tests/io_boundary_integration
	timeout --kill-after=5s 60s $(BUILD_DIR)/tests/fs_boundary_extra
	timeout --kill-after=5s 60s $(BUILD_DIR)/tests/network_boundary_extra
$(BUILD_DIR)/tests/io_boundary_integration: $(BUILD_DIR)/tests/io_boundary_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@
-include $(BUILD_DIR)/tests/io_boundary_integration.d
$(BUILD_DIR)/tests/fs_boundary_extra: $(BUILD_DIR)/tests/fs_boundary_extra.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -Wl,--wrap=_ZN9FdManager11allocate_fdE6FdTypeii -o $@
$(BUILD_DIR)/tests/network_boundary_extra: $(BUILD_DIR)/tests/network_boundary_extra.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@
-include $(BUILD_DIR)/tests/fs_boundary_extra.d $(BUILD_DIR)/tests/network_boundary_extra.d
