# Optional diagnostic only; deliberately outside the default build/check graph.
# make -f tests/startup_layout.mk NP=1 startup-layout-dump CC=/usr/bin/gcc CXX=/usr/bin/g++
include Makefile
ifneq ($(NP),1)
$(error startup-layout-dump requires NP=1)
endif
.PHONY: startup-layout-dump
startup-layout-dump: $(BUILD_DIR)/tests/startup_layout_dump
$(BUILD_DIR)/tests/startup_layout_dump: $(BUILD_DIR)/tests/startup_layout_dump.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@
-include $(BUILD_DIR)/tests/startup_layout_dump.d
