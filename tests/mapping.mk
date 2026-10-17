.PHONY: check-mappings
check-mappings: $(BUILD_DIR)/tests/mapping_integration
	@for backend in full pages verify-pages; do \
	  for cache in '' '--tiny-cache'; do \
	    DETSIM_SNAPSHOT_BACKEND=$$backend timeout --kill-after=5s 65s $< $$cache || exit $$?; \
	  done; \
	done

$(BUILD_DIR)/tests/mapping_integration: $(BUILD_DIR)/tests/mapping_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -Wl,--wrap=pread -Wl,--wrap=_Z17tracee_do_syscalliimmmmmm -o $@

-include $(BUILD_DIR)/tests/mapping_integration.d
