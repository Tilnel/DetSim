.PHONY: check-pending-syscall
check-pending-syscall: $(BUILD_DIR)/tests/pending_syscall_integration
	@for backend in full pages; do \
	  for cache in '' '--tiny-cache'; do \
	    DETSIM_SNAPSHOT_BACKEND=$$backend timeout --kill-after=5s 45s $< $$cache || exit $$?; \
	  done; \
	done

$(BUILD_DIR)/tests/pending_syscall_integration: $(BUILD_DIR)/tests/pending_syscall_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/pending_syscall_integration.d
