CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS  ?=
# -MMD -MP emits a .d file per object listing the headers it includes, so
# editing a header rebuilds everything that depends on it.
DEPFLAGS := -MMD -MP

BUILD    ?= build
LIB_SRC  := $(filter-out src/main.cpp,$(wildcard src/*.cpp))
LIB_OBJ  := $(patsubst src/%.cpp,$(BUILD)/%.o,$(LIB_SRC))
MAIN_OBJ := $(BUILD)/main.o
TEST_OBJ := $(BUILD)/test_sim.o

BIN      ?= cpusim
TESTBIN  := $(BUILD)/run_tests

.PHONY: all test clean demo

all: $(BIN)

$(BIN): $(LIB_OBJ) $(MAIN_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

$(TEST_OBJ): tests/test_sim.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

-include $(wildcard $(BUILD)/*.d)

$(TESTBIN): $(LIB_OBJ) $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# The C++ suite, then the trace/viewer suites if node is available. The viewer
# tests degrade to a skip when node or jsdom is missing, so `make test` never
# fails purely for lack of a JS toolchain.
test: $(TESTBIN) $(BIN)
	$(TESTBIN)
	@if command -v node > /dev/null 2>&1; then \
	  $(BIN) --emit-trace $(BUILD)/test-trace.json --sched mlfq --quantum 60 \
	      programs/spinner.asm programs/yielder.asm > /dev/null && \
	  node viewer/verify_trace.js $(BUILD)/test-trace.json && \
	  node viewer/test_viewer.js $(BUILD)/test-trace.json; \
	else \
	  echo "[ skip ] node not found -- trace and viewer tests skipped"; \
	fi

demo: $(BIN)
	$(BIN) programs/vector_add.asm
	$(BIN) --sched mlfq --quantum 50 programs/spinner.asm programs/yielder.asm

# Bundled demo trace for viewer/index.html. Delivered as a .js assignment
# rather than .json so the page works when opened directly off disk, where
# browsers refuse fetch() on file:// URLs.
traces: $(BIN)
	$(BIN) --emit-trace $(BUILD)/demo.json programs/fib.asm > /dev/null
	printf 'window.CPUSIM_TRACE = ' > viewer/demo-trace.js
	cat $(BUILD)/demo.json >> viewer/demo-trace.js
	printf ';\n' >> viewer/demo-trace.js
	@echo "wrote viewer/demo-trace.js"

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) $(BIN)
