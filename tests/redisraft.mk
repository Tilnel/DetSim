.PHONY: benchmark-redisraft
# Existing workload binary only; never rebuild or modify the Raft submodule.
benchmark-redisraft: tracer $(BUILD_DIR)/tests/redisraft_choose.so $(BUILD_DIR)/tests/redisraft_check.so $(BUILD_DIR)/tests/redisraft_seed.so
	python3 tests/redisraft_benchmark.py --tracer ./tracer --choose $(BUILD_DIR)/tests/redisraft_choose.so --check $(BUILD_DIR)/tests/redisraft_check.so --seed-shim $(BUILD_DIR)/tests/redisraft_seed.so --depth 8 $(REDISRAFT_BENCH_ARGS)

$(BUILD_DIR)/tests/redisraft_choose.so: examples/redisraft/plugins/choose.cpp examples/raft/plugins/choose_common.hpp src/api/plugin.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(CXXFLAGS) -shared -fPIC -MMD -MP -MF $@.d $< -o $@
$(BUILD_DIR)/tests/redisraft_check.so: examples/redisraft/plugins/check.cpp examples/raft/plugins/check_common.hpp src/api/plugin.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(CXXFLAGS) -shared -fPIC -MMD -MP -MF $@.d $< -o $@
-include $(BUILD_DIR)/tests/redisraft_choose.so.d $(BUILD_DIR)/tests/redisraft_check.so.d

$(BUILD_DIR)/tests/redisraft_seed.so: tests/redisraft_seed.c $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -shared -fPIC $< -ldl -o $@
