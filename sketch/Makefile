CXX ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
# Workaround for an incomplete standalone Xcode Command Line Tools install
# on this machine, where the toolchain's own libc++/v1 headers are missing
# most of the standard library (e.g. <cstdint>). Harmless / a no-op on a
# normal Linux/CUDA build box — drop this line there.
CXXFLAGS += -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX26.4.sdk/usr/include/c++/v1

TESTS := test_chain test_nodepool test_inbox test_seedqueue test_beam_search test_pruning_undo_move test_pruning_reduce_positions test_dispatcher_fairness test_end_to_end_sim
BIN_DIR := bin

.PHONY: all test clean

all: $(addprefix $(BIN_DIR)/,$(TESTS))

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/%: tests/%.cpp *.hpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@

test: all
	@for t in $(TESTS); do \
		echo "=== $$t ==="; \
		./$(BIN_DIR)/$$t || exit 1; \
		echo; \
	done

clean:
	rm -rf $(BIN_DIR)
