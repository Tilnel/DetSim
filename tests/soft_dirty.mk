.PHONY: check-soft-dirty
check-soft-dirty: $(BUILD_DIR)/tests/soft_dirty_integration
	timeout --kill-after=2s 30s $<

$(BUILD_DIR)/tests/soft_dirty_integration: tests/soft_dirty_integration.cpp src/core/state/soft_dirty_experiment.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) -std=c++20 -O2 -Wall -Wextra -Werror $< -o $@
