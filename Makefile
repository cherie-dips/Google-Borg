CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CXXFLAGS += -pthread
INC      := -Icore/include -Ithird_party
BUILD    := build
BIN      := $(BUILD)/bin

CORE_SRC := $(wildcard core/src/*.cpp)
SIM_SRC  := $(wildcard sim/*.cpp)
CORE_OBJ := $(CORE_SRC:%.cpp=$(BUILD)/%.o)
SIM_OBJ  := $(SIM_SRC:%.cpp=$(BUILD)/%.o)

.PHONY: all sim cell test clean

all: sim cell $(BIN)/test_core

sim:  $(BIN)/borgsim
cell: $(BIN)/borgmaster $(BIN)/borglet

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -MMD -MP -c $< -o $@

$(BIN)/borgsim: $(SIM_OBJ) $(CORE_OBJ)
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BIN)/borgmaster: $(BUILD)/master/main.o $(CORE_OBJ)
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BIN)/borglet: $(BUILD)/borglet/main.o $(BUILD)/borglet/cgroups.o $(BUILD)/borglet/worker.o
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BIN)/test_core: $(BUILD)/tests/test_core.o $(CORE_OBJ)
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) $^ -o $@

test: $(BIN)/test_core
	./$(BIN)/test_core

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
