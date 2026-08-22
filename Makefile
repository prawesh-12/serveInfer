SHELL := /bin/bash

.PHONY: help run build start stop restart test test-js test-cpp

help:
	@echo "Edge Runtime Commands"
	@echo "  make run     - Build everything and start the full runtime"
	@echo "  make build   - Build C++ targets and install Node dependencies"
	@echo "  make start   - Start supervisor + api-server + shell-app stack"
	@echo "  make stop    - Stop all runtime processes and cleanup IPC files"
	@echo "  make restart - Stop then run"
	@echo "  make test    - Run the Node and C++ test suites"

run:
	@bash scripts/stop.sh
	@bash scripts/build.sh
	@bash scripts/start.sh

build:
	@bash scripts/build.sh

start:
	@bash scripts/start.sh

stop:
	@bash scripts/stop.sh

restart:
	@bash scripts/stop.sh
	@bash scripts/build.sh
	@bash scripts/start.sh

# Neither suite needs the model file, a GPU or a running stack.
test: test-js test-cpp

test-js:
	@echo "[test] node suites"
	@node --test tests/*.test.js

# Configured with the backend off, into build/ so it stays gitignored. The C++
# under test is pure logic, so the llama build is dead weight here.
test-cpp:
	@echo "[test] c++ suites"
	@cmake -S . -B build/tests -DEDGE_ENABLE_LLAMA=OFF -DCMAKE_BUILD_TYPE=Release > /dev/null
	@cmake --build build/tests --target edge-device-tests edge-worker-json-tests -j"$$(nproc)" > /dev/null
	@./build/tests/inference-worker/tests/edge-device-tests
	@./build/tests/inference-worker/tests/edge-worker-json-tests
