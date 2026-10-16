.PHONY: check-pending-timeout
check-pending-timeout: $(BUILD_DIR)/tests/pending_timeout_integration $(BUILD_DIR)/tests/pending_timeout_plugin.so
	@for backend in full pages; do \
	  for cache in '' '--tiny-cache'; do \
	    DETSIM_SNAPSHOT_BACKEND=$$backend timeout --kill-after=5s 90s $(BUILD_DIR)/tests/pending_timeout_integration $(BUILD_DIR)/tests/pending_timeout_plugin.so $$cache || exit $$?; \
	  done; \
	done

$(BUILD_DIR)/tests/pending_timeout_integration: $(BUILD_DIR)/tests/pending_timeout_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/tests/pending_timeout_plugin.so: tests/pending_timeout_integration.cpp src/api/plugin.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) -std=c++20 -Wall -Wextra -Werror -shared -fPIC -Isrc -DDETSIM_TIMEOUT_PLUGIN $< -o $@

-include $(BUILD_DIR)/tests/pending_timeout_integration.d
