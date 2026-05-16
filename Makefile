SHELL := /bin/bash

.PHONY: help run build start stop restart

help:
	@echo "Edge Runtime Commands"
	@echo "  make run     - Build everything and start the full runtime"
	@echo "  make build   - Build C++ targets and install Node dependencies"
	@echo "  make start   - Start supervisor + api-server + shell-app stack"
	@echo "  make stop    - Stop all runtime processes and cleanup IPC files"
	@echo "  make restart - Stop then run"

run:
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
