# Host builds: unit tests and the simulator (the firmware itself builds with PlatformIO).
CXX ?= c++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread
# AddressSanitizer aborts at start on some macOS releases; add it where it works.
SAN ?= -fsanitize=undefined
CORE := lib/bwa_core/src
NET := lib/bwa_net/src
INC := -I$(CORE) -I$(NET) -Isim
CORE_SRC := $(wildcard $(CORE)/*.cpp)
NET_SRC := $(wildcard $(NET)/*.cpp)
BUILD := build/host

.PHONY: all test sim pytest clean
all: test sim

$(BUILD):
	mkdir -p $@

$(BUILD)/unit_tests: tests/unit_tests.cpp $(CORE_SRC) $(wildcard $(CORE)/*.h) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SAN) $(INC) tests/unit_tests.cpp $(CORE_SRC) -o $@

$(BUILD)/bridge_sim: sim/sim_main.cpp sim/fake_board.cpp sim/fake_board.h $(CORE_SRC) $(NET_SRC) $(wildcard $(CORE)/*.h $(NET)/*.h) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SAN) $(INC) sim/sim_main.cpp sim/fake_board.cpp $(CORE_SRC) $(NET_SRC) -o $@

test: $(BUILD)/unit_tests
	$(BUILD)/unit_tests

sim: $(BUILD)/bridge_sim

# The simulator against pybalboa. PYTHON must have pybalboa and pytest.
PYTHON ?= python3
pytest: sim
	PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 $(PYTHON) -m pytest -q -p no:cacheprovider tests

clean:
	rm -rf build
