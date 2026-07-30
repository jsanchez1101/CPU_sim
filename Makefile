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

test: $(TESTBIN)
	$(TESTBIN)

demo: $(BIN)
	$(BIN) programs/vector_add.asm
	$(BIN) --sched mlfq --quantum 50 programs/spinner.asm programs/yielder.asm

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) $(BIN)
