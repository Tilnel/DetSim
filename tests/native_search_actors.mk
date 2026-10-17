# Optional actual CLI gate; the supplied tracer must be an immutable NP1 build.
NP ?= 2
include Makefile
.DEFAULT_GOAL := check-native-search-actors
.PHONY: check-native-search-actors check-native-search-policy
check-native-search-actors:
	@test -n "$(NATIVE_SEARCH_TRACER)" || { echo 'set NATIVE_SEARCH_TRACER to an NP1 tracer' >&2; exit 1; }
	python3 tests/native_search_actors.py --tracer "$(NATIVE_SEARCH_TRACER)"
check-native-search-policy: $(BUILD_DIR)/tests/native_search_actors_policy
	python3 -c 'import pathlib,subprocess,tempfile; temporary=tempfile.TemporaryDirectory(prefix="ds-pure-actors-"); directory=pathlib.Path(temporary.name); subprocess.run(["$(abspath $<)"],cwd=directory,check=True); assert not list(directory.iterdir()),"pure policy wrote files"'
$(BUILD_DIR)/tests/native_search_actors_policy: tests/native_search_actors.cpp $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	@mkdir -p $(@D)
	$(CXX) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(CXXFLAGS) $< $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o $(LDFLAGS) $(LDLIBS) -o $@
