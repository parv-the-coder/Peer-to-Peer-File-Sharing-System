# Thin wrapper around the CMake build, for `make` muscle memory.
# CMake remains the real build system; see README.md.

BUILD ?= build
JOBS  ?= $(shell nproc 2>/dev/null || echo 4)

.PHONY: all build configure test clean

all: build

configure:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Debug

build: configure
	cmake --build $(BUILD) -j$(JOBS)

test: build
	ctest --test-dir $(BUILD) --output-on-failure

clean:
	rm -rf $(BUILD)
