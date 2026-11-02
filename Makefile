# fixwire: common entry points. J defaults to half the logical CPUs.
J ?= $(shell n=$$( (sysctl -n hw.ncpu 2>/dev/null || nproc) ); echo $$(( n/2 > 2 ? n/2 : 2 )))
BUILD ?= build
PY ?= python3
.PHONY: build test sanitize bench lint fuzz demo data interop clean

build:
	cmake -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j $(J)

test: build
	ctest --test-dir $(BUILD) -j $(J) --output-on-failure

sanitize:
	cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFIXWIRE_SANITIZE=address,undefined
	cmake --build build-asan -j $(J)
	ctest --test-dir build-asan -j $(J) --output-on-failure

# Codec micro-benchmarks and loopback order->fill round trips (run on an idle machine).
bench: build
	$(BUILD)/fixwire-bench | tee bench/results/bench.json

# Interop with QuickFIX (pip install quickfix): both directions + crash/restart/resend.
interop: build
	$(PY) interop/qf_interop.py $(BUILD) research/results/interop_quickfix.json

lint:
	@command -v clang-format >/dev/null && clang-format --dry-run --Werror include/fixwire/*.hpp tools/*.cpp tests/*.cpp fuzz/*.cpp || echo "clang-format not installed; skipping"

FUZZ_CXX ?= clang++
fuzz:
	cmake -S . -B build-fuzz -G Ninja -DCMAKE_CXX_COMPILER=$(FUZZ_CXX) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFIXWIRE_FUZZ=ON
	cmake --build build-fuzz --target fuzz-parser -j $(J)
	mkdir -p fuzz/corpus && build-fuzz/fuzz-parser fuzz/corpus -max_total_time=$${FUZZ_SECONDS:-300} -rss_limit_mb=2048

# No external data: FIX messages are generated.
data:
	@echo "fixwire needs no data download"

demo: build
	$(BUILD)/fixwire-exchange --port 19878 --once & sleep 0.5; $(BUILD)/fixwire-client --port 19878 --orders 20000 --window 16

clean:
	rm -rf build build-asan build-fuzz
