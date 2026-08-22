SHELL := /bin/bash

.PHONY: help build run stop restart \
        backend backend-stop clients clients-stop dashboard dashboard-stop \
        test test-js test-cpp

help:
	@echo "Three tiers, three lifecycles. Each starts and stops on its own."
	@echo ""
	@echo "  make build           - compile C++ and install backend node deps"
	@echo ""
	@echo "  make backend         - supervisor, model cache, workers, api-server, shell"
	@echo "  make clients         - the sample user apps"
	@echo "  make dashboard       - the operator status page"
	@echo ""
	@echo "  make backend-stop    - stop one tier without touching the others"
	@echo "  make clients-stop"
	@echo "  make dashboard-stop"
	@echo ""
	@echo "  make run             - build, then start all three"
	@echo "  make stop            - stop all three"
	@echo "  make restart         - stop, build, start"
	@echo "  make test            - run the node and C++ test suites"

build:
	@bash scripts/build.sh

backend:
	@bash scripts/backend.sh start

backend-stop:
	@bash scripts/backend.sh stop

clients:
	@bash scripts/clients.sh start

clients-stop:
	@bash scripts/clients.sh stop

dashboard:
	@bash scripts/dashboard.sh start

dashboard-stop:
	@bash scripts/dashboard.sh stop

run:
	@bash scripts/stop.sh
	@bash scripts/build.sh
	@bash scripts/backend.sh start
	@bash scripts/clients.sh start
	@bash scripts/dashboard.sh start

stop:
	@bash scripts/stop.sh

restart:
	@bash scripts/stop.sh
	@bash scripts/build.sh
	@bash scripts/backend.sh start
	@bash scripts/clients.sh start
	@bash scripts/dashboard.sh start

test: test-js test-cpp

test-js:
	@echo "[test] node suites"
	@node --test tests/*.test.js

# Configured with the backend off, into build/ so it stays gitignored. The C++
# under test is pure logic, so the llama build is dead weight here.
test-cpp:
	@echo "[test] c++ suites"
	@cmake -S backend -B build/tests -DEDGE_ENABLE_LLAMA=OFF -DCMAKE_BUILD_TYPE=Release > /dev/null
	@cmake --build build/tests --target edge-device-tests edge-worker-json-tests edge-hardware-tests edge-remote-recovery-tests edge-model-cache-tests -j"$$(nproc)" > /dev/null
	@./build/tests/inference-worker/tests/edge-device-tests
	@./build/tests/inference-worker/tests/edge-worker-json-tests
	@./build/tests/inference-worker/tests/edge-hardware-tests
	@./build/tests/inference-worker/tests/edge-remote-recovery-tests
	@./build/tests/inference-worker/tests/edge-model-cache-tests
