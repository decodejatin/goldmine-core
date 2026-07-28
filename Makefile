# ==============================================================================
# Goldmine Institutional ACE Pipeline
# ==============================================================================

.PHONY: help build-cpp build-rust build infra-up infra-down train run clean

help:
	@echo "Goldmine ACE Platform Commands:"
	@echo "  make build       - Compiles all C++ and Rust microservices"
	@echo "  make infra-up    - Starts the InfluxDB & Grafana telemetry stack via Docker"
	@echo "  make infra-down  - Stops the telemetry stack"
	@echo "  make train       - Runs the full Python ML pipeline (Download -> Process -> Kaggle)"
	@echo "  make run         - Launches the Rust Orchestrator and full platform"
	@echo "  make clean       - Cleans all build artifacts and logs"

build-cpp:
	@echo "[*] Building C++ Core Engine..."
	@mkdir -p core/build
	@cd core/build && cmake .. && make -j$$(nproc)
	@echo "[*] Building C++ Binance Ingestion Feed..."
	@mkdir -p ingest/build
	@cd ingest/build && cmake .. && make -j$$(nproc)

build-rust:
	@echo "[*] Building Rust Orchestrator..."
	@cd orchestrator && cargo build --release
	@echo "[*] Building Rust Risk Server..."
	@cd risk_server && cargo build --release
	@echo "[*] Building Rust TSDB Dumper..."
	@cd tsdb_dumper && cargo build --release
	@echo "[*] Building Rust Backtester..."
	@cd backtester && cargo build --release

build: build-cpp build-rust
	@echo "[+] All microservices successfully built."

infra-up:
	@echo "[*] Booting Infrastructure (InfluxDB + Grafana)..."
	@docker-compose up -d

infra-down:
	@echo "[*] Shutting down Infrastructure..."
	@docker-compose down

train:
	@echo "[*] Executing Automated ML Pipeline..."
	@cd brain && ../venv/bin/python data_downloader.py
	@cd brain && ../venv/bin/python feature_pipeline.py
	@cd brain && ../venv/bin/python kaggle_runner.py

run:
	@echo "[*] Starting Goldmine ACE Orchestrator..."
	@cd orchestrator && ./target/release/orchestrator

clean:
	@echo "[*] Cleaning build directories and IPC files..."
	@rm -rf core/build ingest/build
	@cd orchestrator && cargo clean
	@cd risk_server && cargo clean
	@cd tsdb_dumper && cargo clean
	@cd backtester && cargo clean
	@rm -f /dev/shm/goldmine_tick_shm /dev/shm/goldmine_param_shm
	@echo "[+] Clean complete."
