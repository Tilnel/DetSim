# Run with make -f Makefile -f tests/elf.mk NP=1 check-elf.
.PHONY: check-elf
check-elf: $(BUILD_DIR)/tests/elf_detection $(addprefix $(BUILD_DIR)/tests/elf_fixture,0 1 2)
	$(BUILD_DIR)/tests/elf_detection $(addprefix $(BUILD_DIR)/tests/elf_fixture,0 1 2)

$(BUILD_DIR)/tests/elf_detection: $(BUILD_DIR)/tests/elf_detection.o $(BUILD_DIR)/src/utils/utils.o $(BUILD_DIR)/tests/component_log_stubs.o
	$(LD) $^ $(LDFLAGS) -lelf -lfmt -o $@

$(BUILD_DIR)/tests/elf_fixture%: tests/elf_fixture.c $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CC) -O0 -DGO_SECTION=$* $< -o $@

-include $(BUILD_DIR)/tests/elf_detection.d $(BUILD_DIR)/tests/component_log_stubs.d
