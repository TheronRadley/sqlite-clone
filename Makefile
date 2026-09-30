# sqlite-clone — build configuration
#
# Targets:
#   make            build the CLI shell (dbx) and the benchmark harness (bench)
#   make test       build and run the full test suite (with ASan/UBSan)
#   make test-fast  run the test suite without sanitizers (faster, for iteration)
#   make clean      remove build artifacts
#
# No external dependencies: C++20 standard library only.

CXX      ?= g++
BUILD    := build
OBJDIR   := $(BUILD)/obj

# Library sources (everything except the two binaries)
LIBSRC   := $(filter-out src/main.cpp src/bench.cpp,$(wildcard src/*.cpp))
LIBOBJ   := $(patsubst src/%.cpp,$(OBJDIR)/%.o,$(LIBSRC))
TESTSRC  := $(wildcard tests/*.cpp)

CXXFLAGS_COMMON := -std=c++20 -Wall -Wextra -Wpedantic -Isrc

.PHONY: all test test-fast clean

all: $(BUILD)/dbx $(BUILD)/bench

# ---------------- release binaries ----------------

$(BUILD)/dbx: src/main.cpp $(LIBOBJ)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS_COMMON) -O2 $(if $(DEBUG),-g -O0,) -o $@ $^

$(BUILD)/bench: src/bench.cpp $(LIBOBJ)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS_COMMON) -O2 -o $@ $^

$(OBJDIR)/%.o: src/%.cpp
	@mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS_COMMON) -O2 -c -o $@ $<

# ---------------- tests ----------------

# Sanitized test build: AddressSanitizer + UndefinedBehaviorSanitizer.
# The engine is a storage layer; every test run checks memory safety.
$(BUILD)/tests: $(TESTSRC) $(LIBSRC)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS_COMMON) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
		-o $@ $(TESTSRC) $(LIBSRC)

$(BUILD)/tests-fast: $(TESTSRC) $(LIBSRC)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS_COMMON) -O2 -o $@ $(TESTSRC) $(LIBSRC)

test: $(BUILD)/tests
	./$(BUILD)/tests

test-fast: $(BUILD)/tests-fast
	./$(BUILD)/tests-fast

clean:
	rm -rf $(BUILD)
