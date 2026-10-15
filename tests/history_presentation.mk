.PHONY: check-history-presentation
check-history-presentation: $(BUILD_DIR)/tests/history_presentation_integration
	$(BUILD_DIR)/tests/history_presentation_integration

$(BUILD_DIR)/tests/history_presentation_integration: $(BUILD_DIR)/tests/history_presentation_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -Wl,--wrap=_Z8exec_bfsv,--wrap=_Z8exec_dfsi,--wrap=_Z9exec_randi -o $@

-include $(BUILD_DIR)/tests/history_presentation_integration.d
