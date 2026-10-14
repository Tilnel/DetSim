.PHONY: check-storage check-store-cache check-store-packed check-store-metadata benchmark-storage
check-storage: check-store-cache check-store-packed check-store-metadata

check-store-metadata: $(BUILD_DIR)/tests/store_metadata
	timeout --kill-after=5s 60s $<
$(BUILD_DIR)/tests/store_metadata: $(BUILD_DIR)/tests/store_metadata.o $(BUILD_DIR)/src/core/state/sysstate_store.o
	$(LD) $^ $(LDFLAGS) -Wl,--wrap=pread,--wrap=pwrite,--wrap=fdatasync -lpthread -o $@

# Byte-checked benchmark. Caller supplies a disposable directory and workload:
# build/release/np1/tests/storage_benchmark /tmp/new-store hot|pressure|metadata label
benchmark-storage: $(BUILD_DIR)/tests/storage_benchmark
$(BUILD_DIR)/tests/storage_benchmark: $(BUILD_DIR)/tests/storage_benchmark.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(BUILD_DIR)/tests/store_metadata.d $(BUILD_DIR)/tests/storage_benchmark.d

.PHONY: check-store-tools
check-store-tools: tools
	python3 tests/store_metadata_tools.py ./tools/sstate_query ./tools/verify_sstate $(NP)
check-storage: check-store-tools
