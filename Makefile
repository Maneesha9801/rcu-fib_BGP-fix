.DEFAULT_GOAL := help
SHELL := /bin/bash

BUILD ?= build
TYPE  ?= RelWithDebInfo
SOURCES := $(shell find include src tests bench tools -name '*.hpp' -o -name '*.cpp' 2>/dev/null)

.PHONY: help build test bench info tsan asan check format format-check clean

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2}'

build: ## Configure and build everything
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=$(TYPE)
	cmake --build $(BUILD)

test: build ## Run the test suite
	ctest --test-dir $(BUILD) --output-on-failure

bench: ## Build optimised and run the benchmark
	cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
	cmake --build build-release --target rcufib_bench
	./build-release/rcufib_bench --prefixes 200000 --readers 4 --duration 3000 --sweep

info: build ## Print the trie's shape for a generated table
	./$(BUILD)/rcufib info --prefixes 50000

tsan: ## Run the tests under ThreadSanitizer
	cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRCUFIB_SANITIZER=thread
	cmake --build build-tsan
	TSAN_OPTIONS="halt_on_error=1" ./build-tsan/rcufib_tests

asan: ## Run the tests under AddressSanitizer and UBSan
	cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DRCUFIB_SANITIZER=address+undefined
	cmake --build build-asan
	UBSAN_OPTIONS="print_stacktrace=1 halt_on_error=1" ./build-asan/rcufib_tests

check: test tsan asan format-check ## Everything CI runs

format: ## Reformat the sources in place
	clang-format -i $(SOURCES)

format-check: ## Fail if anything is unformatted
	clang-format --dry-run --Werror $(SOURCES)

clean: ## Remove every build directory
	rm -rf build build-* compile_commands.json
