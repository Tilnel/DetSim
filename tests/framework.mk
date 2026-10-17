.PHONY: check-state-view check-plugins
check-state-view: $(BUILD_DIR)/tests/state_view_integration $(BUILD_DIR)/tests/dwarf_fixture4 $(BUILD_DIR)/tests/dwarf_fixture5 $(BUILD_DIR)/tests/state_view_layout
	$(BUILD_DIR)/tests/state_view_integration $(BUILD_DIR)/tests/dwarf_fixture4 $(BUILD_DIR)/tests/dwarf_fixture5 $(BUILD_DIR)/tests/state_view_layout
check-plugins: $(BUILD_DIR)/tests/plugin_integration $(BUILD_DIR)/tests/plugin_good.so $(BUILD_DIR)/tests/plugin_legacy.so $(BUILD_DIR)/tests/plugin_bad_abi.so $(BUILD_DIR)/tests/plugin_decoder.so
	$(BUILD_DIR)/tests/plugin_integration $(BUILD_DIR)/tests/plugin_good.so $(BUILD_DIR)/tests/plugin_legacy.so $(BUILD_DIR)/tests/plugin_bad_abi.so $(BUILD_DIR)/tests/plugin_decoder.so
$(BUILD_DIR)/tests/state_view_integration: $(BUILD_DIR)/tests/state_view_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@
$(BUILD_DIR)/tests/plugin_integration: $(BUILD_DIR)/tests/plugin_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@
$(BUILD_DIR)/tests/plugin_good.so: tests/plugin_fixture.cpp src/api/plugin.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) -std=c++20 -shared -fPIC -Isrc $< -o $@
$(BUILD_DIR)/tests/plugin_legacy.so: tests/plugin_fixture.cpp src/api/plugin.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) -std=c++20 -shared -fPIC -Isrc -DDETSIM_FIXTURE_LEGACY $< -o $@
$(BUILD_DIR)/tests/plugin_bad_abi.so: tests/plugin_fixture.cpp src/api/plugin.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) -std=c++20 -shared -fPIC -Isrc -DDETSIM_FIXTURE_BAD_ABI $< -o $@
-include $(BUILD_DIR)/tests/state_view_integration.d $(BUILD_DIR)/tests/plugin_integration.d

.PHONY: check-raft-plugin-contract
check-raft-plugin-contract: $(BUILD_DIR)/tests/raft_checker_unit $(BUILD_DIR)/tests/redisraft_checker_unit $(BUILD_DIR)/tests/redisraft_quorum_checker_unit
	$(BUILD_DIR)/tests/raft_checker_unit
	$(BUILD_DIR)/tests/redisraft_checker_unit
	$(BUILD_DIR)/tests/redisraft_quorum_checker_unit
	python3 tests/raft_case_runner_test.py
	python3 tests/redisraft_case_runner_test.py
	python3 tests/redisraft_quorum_runner_test.py
	python3 tests/redisraft_repair_runner_test.py
check-plugins: check-raft-plugin-contract
$(BUILD_DIR)/tests/raft_checker_unit: tests/raft_checker_unit.cpp examples/raft/plugins/check_common.hpp src/api/plugin.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) -std=c++20 -Wall -Wextra -Werror -Isrc $< -o $@
$(BUILD_DIR)/tests/redisraft_checker_unit: tests/raft_checker_unit.cpp examples/raft/plugins/check_common.hpp src/api/plugin.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) -std=c++20 -Wall -Wextra -Werror -DREDISRAFT_PLUGIN -Isrc $< -o $@

$(BUILD_DIR)/tests/plugin_decoder.so: tests/plugin_fixture.cpp src/api/plugin.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) -std=c++20 -shared -fPIC -Isrc -DDETSIM_FIXTURE_DECODER $< -o $@

$(BUILD_DIR)/tests/state_view_layout: tests/state_view_layout.c $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CC) -O0 -g -fno-pie -no-pie $< -o $@

$(BUILD_DIR)/tests/redisraft_quorum_checker_unit: tests/redisraft_quorum_checker_unit.cpp examples/redisraft/plugins/quorum.cpp examples/raft/plugins/check_common.hpp src/api/plugin.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) -std=c++20 -Wall -Wextra -Werror -Isrc $< -o $@
