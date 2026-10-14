# Run with make -f Makefile -f tests/dwarf.mk NP=1 check-dwarf.
.PHONY: check-dwarf
check-dwarf: $(BUILD_DIR)/tests/dwarf_integration $(BUILD_DIR)/tests/dwarf_fixture4 $(BUILD_DIR)/tests/dwarf_fixture5 $(BUILD_DIR)/tests/dwarf_forward4 $(BUILD_DIR)/tests/dwarf_forward5 $(BUILD_DIR)/tests/dwarf_class4 $(BUILD_DIR)/tests/dwarf_class5
	$(BUILD_DIR)/tests/dwarf_integration $(BUILD_DIR)/tests/dwarf_fixture4 $(BUILD_DIR)/tests/dwarf_fixture5
	timeout 10s $(BUILD_DIR)/tests/dwarf_integration --forward $(BUILD_DIR)/tests/dwarf_forward4
	timeout 10s $(BUILD_DIR)/tests/dwarf_integration --forward $(BUILD_DIR)/tests/dwarf_forward5
	$(BUILD_DIR)/tests/dwarf_integration --class $(BUILD_DIR)/tests/dwarf_class4
	$(BUILD_DIR)/tests/dwarf_integration --class $(BUILD_DIR)/tests/dwarf_class5

$(BUILD_DIR)/tests/dwarf_integration: $(BUILD_DIR)/tests/dwarf_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/tests/dwarf_fixture%: tests/dwarf_fixture.c tests/dwarf_fixture_other.c tests/dwarf_fixture.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CC) -O0 -gdwarf-$* -fno-pie -no-pie tests/dwarf_fixture.c tests/dwarf_fixture_other.c -o $@

$(BUILD_DIR)/tests/dwarf_forward%: tests/dwarf_forward_decl.c tests/dwarf_forward_def.c $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CC) -O0 -gdwarf-$* -fno-pie -no-pie tests/dwarf_forward_decl.c tests/dwarf_forward_def.c -o $@

$(BUILD_DIR)/tests/dwarf_class%: tests/dwarf_class.cpp $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) -O0 -gdwarf-$* -fno-pie -no-pie $< -o $@

-include $(BUILD_DIR)/tests/dwarf_integration.d
