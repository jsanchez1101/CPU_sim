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
FUZZ_OBJ := $(BUILD)/fuzz_assembler.o

# Kept invocable as-is (leading ./) so recipes can call $(BIN) directly while
# still allowing an absolute override, e.g. make BIN=/tmp/out/cpusim.
BIN      ?= ./cpusim
TESTBIN  := $(BUILD)/run_tests
FUZZBIN  := $(BUILD)/fuzz_assembler

# Iterations for `make fuzz`; CI raises this.
FUZZ_RUNS ?= 20000

# Flags for `make sanitize`: AddressSanitizer and UBSan, with UB made fatal so
# a finding fails the build instead of scrolling past.
SAN_FLAGS := -std=c++17 -O1 -g -fno-omit-frame-pointer -Iinclude \
             -Wall -Wextra -Wpedantic \
             -fsanitize=address,undefined -fno-sanitize-recover=undefined

.PHONY: all test clean demo traces invariants fuzz sanitize

all: $(BIN)

$(BIN): $(LIB_OBJ) $(MAIN_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

$(TEST_OBJ): tests/test_sim.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

$(FUZZ_OBJ): tests/fuzz_assembler.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

-include $(wildcard $(BUILD)/*.d)

$(TESTBIN): $(LIB_OBJ) $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(FUZZBIN): $(LIB_OBJ) $(FUZZ_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# The C++ suite, then the trace/viewer suites if node is available. The viewer
# tests degrade to a skip when node or jsdom is missing, so `make test` never
# fails purely for lack of a JS toolchain.
test: $(TESTBIN) $(BIN)
	$(TESTBIN)
	@BIN=$(BIN) ./tools/check_cli.sh
	@if command -v node > /dev/null 2>&1; then \
	  $(BIN) --emit-trace $(BUILD)/test-trace.json --sched mlfq --quantum 60 \
	      programs/spinner.asm programs/yielder.asm > /dev/null && \
	  node viewer/verify_trace.js $(BUILD)/test-trace.json && \
	  node viewer/test_viewer.js $(BUILD)/test-trace.json; \
	else \
	  echo "[ skip ] node not found -- trace and viewer tests skipped"; \
	fi

# A program's result must not depend on microarchitecture, only its cycle
# count. See ARCHITECTURE.md; this is the first thing to run when a result
# looks wrong.
invariants: $(BIN)
	@BIN=$(BIN) ./tools/check_invariants.sh

# Feed mutated .asm files to the assembler and run whatever assembles. Only
# meaningful with sanitizers on, so `make sanitize` runs it too.
fuzz: $(FUZZBIN)
	$(FUZZBIN) $(FUZZ_RUNS) 1 programs/*.asm

# Everything above under ASan + UBSan, in a separate build directory.
sanitize:
	$(MAKE) BUILD=$(BUILD)/san BIN=$(BUILD)/san/cpusim CXXFLAGS="$(SAN_FLAGS)" test invariants fuzz

demo: $(BIN)
	$(BIN) programs/vector_add.asm
	$(BIN) --sched mlfq --quantum 50 programs/spinner.asm programs/yielder.asm

# Bundled demo trace for viewer/index.html. Delivered as a .js assignment
# rather than .json so the page works when opened directly off disk, where
# browsers refuse fetch() on file:// URLs.
#
# matmul rather than a short program: anything that retires only a couple of
# hundred instructions is dominated by its two cold-start page faults (400
# cycles), which makes a poor first impression for a pipeline viewer. Capped
# at 2000 cycles to keep the bundled file small.
traces: $(BIN)
	$(BIN) --emit-trace $(BUILD)/demo.json --trace-cycles 2000 programs/matmul.asm > /dev/null
	printf 'window.CPUSIM_TRACE = ' > viewer/demo-trace.js
	cat $(BUILD)/demo.json >> viewer/demo-trace.js
	printf ';\n' >> viewer/demo-trace.js
	@echo "wrote viewer/demo-trace.js"

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) $(BIN)
