# GOLDMINE INSTITUTIONAL ORCHESTRATOR & QUANTITATIVE PLATFORM
**Comprehensive Master Developer Reference & Complete System Architecture**

Welcome to the **Goldmine** platform. This document serves as the absolute source of truth for the entire Goldmine repository. It is written specifically so that any new Quantitative Developer, Low-Latency Systems Engineer, or Machine Learning Researcher can join the project and understand **every single feature, workflow, pipeline, dependency, and tech stack choice** without needing to guess.

Goldmine is an **ACE-tier (Autonomous, Consistent, Enterprise-grade)** High-Frequency Trading (HFT) and quantitative research system. 

---

## TABLE OF CONTENTS
1. [Tech Stack & Core Dependencies](#1-tech-stack--core-dependencies)
2. [Global Architecture & Data Flow](#2-global-architecture--data-flow)
3. [Memory & IPC Philosophy (Zero-Allocation)](#3-memory--ipc-philosophy-zero-allocation)
4. [Module 1: The Control Plane (`orchestrator/`)](#4-module-1-the-control-plane-orchestrator)
5. [Module 2: Native Data Ingestion (`ingest/`)](#5-module-2-native-data-ingestion-ingest)
6. [Module 3: Execution Engine (`core/`)](#6-module-3-execution-engine-core)
7. [Module 4: Risk Verification (`risk_server/`)](#7-module-4-risk-verification-risk_server)
8. [Module 5: Time-Series Data Lake (`tsdb_dumper/` & `docker/`)](#8-module-5-time-series-data-lake-tsdb_dumper--docker)
9. [Module 6: Machine Learning Brain (`brain/`)](#9-module-6-machine-learning-brain-brain)
10. [Module 7: Deterministic Backtester (`backtester/`)](#10-module-7-deterministic-backtester-backtester)
11. [Complete Development Workflow](#11-complete-development-workflow)
12. [Operational Invariants & Safety Constraints](#12-operational-invariants--safety-constraints)

---

## 1. TECH STACK & CORE DEPENDENCIES

The Goldmine project is intentionally polyglot. We use specific languages for specific domains to maximize performance where it matters and maximize developer velocity where latency isn't critical.

### 1.1. C++20 (The Hot Path)
Used strictly for market data ingestion and trade execution.
*   **Compiler:** GCC 11+ or Clang 14+ (Requires `-std=c++20 -O3 -march=native`).
*   **Dependencies:**
    *   `IXWebSocket`: Ultra-low latency WebSocket C++ library used in `binance_feed.cpp`.
    *   `nlohmann/json`: Single-header JSON parsing library used for API payloads.
    *   `rigtorp::SPSCQueue`: Lock-free, zero-allocation Single-Producer Single-Consumer queue for telemetry logging.

### 1.2. Rust 1.70+ (The Control Plane)
Used for process supervision, risk management, and asynchronous data pipelines.
*   **Dependencies (managed via Cargo):**
    *   `tokio` (v1.37): The asynchronous runtime powering the orchestrator and all network bounds.
    *   `reqwest` & `tokio-tungstenite`: Used in the Drop Copy Risk Server for REST and WebSockets.
    *   `serde` & `toml`: For parsing configuration files.
    *   `tracing` & `tracing-subscriber`: For asynchronous, non-blocking logging.

### 1.3. Python 3.10+ (The Brain)
Used exclusively offline for data processing and online for generating trading parameters.
*   **Dependencies (managed via pip/venv):**
    *   `pandas` & `polars`: For high-speed dataframe manipulation and feature engineering.
    *   `pyarrow`: For saving highly compressed Parquet datasets.
    *   `torch` (PyTorch) & `stable-baselines3`: For Deep Reinforcement Learning (PPO Agent).
    *   `gymnasium`: For the custom RL trading environment.
    *   `kaggle`: CLI tool for pushing headless training jobs to Kaggle GPUs.

### 1.4. Infrastructure
*   **Docker & Docker Compose:** Used to spin up the local Time-Series database.
*   **InfluxDB 2.7:** Natively handles millions of ticks for historical charting and Transaction Cost Analysis (TCA).
*   **Grafana 10.2:** Connects to InfluxDB for real-time dashboards of engine latency, PnL, and exposure.

---

## 2. GLOBAL ARCHITECTURE & DATA FLOW

The system is designed as a directed acyclic graph of data flowing from the exchange, through our shared memory, into the execution engine, and finally out to the Time-Series Data Lake.

1. **Phase 1 (Ingestion):** Binance WebSockets stream data to `binance_feed` (C++).
2. **Phase 2 (IPC):** `binance_feed` atomically writes ticks to `/dev/shm/goldmine_tick_shm`.
3. **Phase 3 (Execution):** `expert_engine` (C++) spin-waits on `/dev/shm/goldmine_tick_shm`. The moment a tick arrives, it executes trading logic.
4. **Phase 4 (Parameters):** Simultaneously, `rl_agent.py` evaluates the market state and writes optimized boundaries (Risk %, TP, SL) to `/dev/shm/goldmine_param_shm`, which the C++ engine reads asynchronously.
5. **Phase 5 (Telemetry):** `tsdb_dumper` (Rust) reads the same ticks from `/dev/shm/goldmine_tick_shm` and streams them via HTTP to InfluxDB.
6. **Phase 6 (Risk):** `risk_server` (Rust) independently queries Binance to verify the C++ engine hasn't hallucinated and breached the 10oz maximum exposure limit.

---

## 3. MEMORY & IPC PHILOSOPHY (ZERO-ALLOCATION)

To achieve microsecond latency, the C++ execution engine strictly adheres to a **zero-allocation architecture**.
*   **No `new` or `malloc`:** After the initialization phase, memory allocation is strictly forbidden. 
*   **POSIX Shared Memory (`/dev/shm`):** We use a memory-mapped file located in RAM.
*   **Atomic Sequence IDs:** Inter-Process Communication (IPC) is handled lock-free using `std::atomic<uint64_t> seq_id` with `std::memory_order_release` and `std::memory_order_acquire`.
*   **Cache Line Alignment:** Structs like `SharedTick` are padded to exactly 64 bytes (`alignas(64)`) to perfectly fit an L1 CPU cache line. This prevents "false sharing" (cache invalidation when another thread writes to adjacent memory).

---

## 4. MODULE 1: THE CONTROL PLANE (`orchestrator/`)

Written in Rust, `orchestrator/src/main.rs` is the supervisor daemon that owns the entire lifecycle of the trading platform.

### 4.1. Core Features
*   **`config/orchestrator.toml` Parser:** Reads the configuration defining all microservices, their arguments, their assigned startup Phase, and whether they are `critical`.
*   **Phase 0 (IPC Clean):** Immediately on boot, it executes `fs::remove_file()` on `/dev/shm/goldmine_tick_shm` and `/dev/shm/goldmine_param_shm` to ensure no corrupted data pointers exist from a previous crash.
*   **Phased Boot & Readiness Probing:**
    *   **Phase 1:** Launches Risk Server & TSDB Dumper.
    *   **Phase 2:** Launches Parameter Server and actively loops, probing for the physical existence of `/dev/shm/goldmine_param_shm` before allowing the next phase to start.
    *   **Phase 3:** Launches the C++ `expert_engine` and probes for `/dev/shm/goldmine_tick_shm`.
    *   **Phase 4:** Opens the floodgates by launching the C++ `binance_feed`.
*   **Supervision Loops:** Every process is spawned in an asynchronous `tokio` task. If a non-critical process dies (like TSDB dumper), it logs a warning and restarts it after a backoff delay.
*   **Emergency Cascading Halt:** If `expert_engine` or `risk_server` dies, a global broadcast channel sends an `[EMERGENCY SYSTEM HALT]`, instantly terminating all remaining child processes.
*   **Graceful SIGINT/SIGTERM Teardown:** Upon `Ctrl-C`, it kills Phase 4 (Ingestion) first, waits 2 seconds for Phase 3 (Engine) to drain its lock-free queues, then kills Phase 2 and 1, and finally flushes `/dev/shm`.

---

## 5. MODULE 2: NATIVE DATA INGESTION (`ingest/`)

Written in C++20, this replaces the legacy Python websocket bridge to eliminate GIL latency.

### 5.1. Core Features (`binance_feed.cpp`)
*   **`IXWebSocket` Connection:** Connects directly to Binance Spot/Futures websockets.
*   **JSON Parsing:** Uses `nlohmann_json` to parse `bookTicker` payloads.
*   **Zero-Copy Memory Map:** Maps `/dev/shm/goldmine_tick_shm` using `mmap()`.
*   **Atomic Publishing:** When a tick arrives, it populates the `SharedTick` struct, and increments the `seq_id` with `std::memory_order_release`. This signals the execution engine that the data is ready to be read, requiring exactly 0 mutex locks.

---

## 6. MODULE 3: EXECUTION ENGINE (`core/`)

Written in C++20, this is the brainstem of the operation where trades are generated and executed.

### 6.1. Core Features (`expert_engine.cpp`)
*   **Spin-Wait Loop:** The main thread pins itself to a dedicated CPU core (e.g., Core 3) using `sched_setaffinity`. It enters a `while(true)` loop, continuously polling the `seq_id` of `/dev/shm/goldmine_tick_shm` with `std::memory_order_acquire`.
*   **Asynchronous Parameter Updates:** It independently checks `/dev/shm/goldmine_param_shm` to read the latest `risk_pct` and `tp_multiplier` weights output by the Python RL model.
*   **SPSC Telemetry Queue:** Instead of writing logs synchronously (which would block the thread), the engine pushes execution events to a `rigtorp::SPSCQueue`. A separate background thread consumes this queue and handles disk I/O, keeping the hot path entirely free of blocking calls.

---

## 7. MODULE 4: RISK VERIFICATION (`risk_server/`)

Written in Rust, this operates entirely out-of-band as an independent Drop-Copy server. It does not trust the C++ engine.

### 7.1. Core Features (`risk_server/src/main.rs`)
*   **Real-time WebSocket Exposure Tracking:** Connects to `wss://stream.binance.com:9443/ws/<listenKey>` by authenticating via a REST `POST` to `/api/v3/userDataStream`. It consumes `outboundAccountPosition` events to track the exact physical position in PAXG.
*   **NTP Clock Drift Sync:** Runs a background task querying `GET /api/v3/time` to calculate local machine drift. It applies this `offset_ms` to all outbound signed requests to prevent `-1021 INVALID_TIMESTAMP` errors from the exchange.
*   **Emergency Kill Switch:** If physical exposure exceeds `MAX_ALLOWABLE_EXPOSURE` (10 oz), it bypasses the C++ pipeline entirely, generates an HMAC SHA-256 signature, and fires an instantaneous `DELETE /api/v3/openOrders` to flatline the book.
*   **Dead Man's Switch:** Fires a background heartbeat to the exchange's `countdownCancelAll` endpoint every 10 seconds, ensuring that if our physical server drops offline, Binance automatically cancels all pending limit orders 15 seconds later.

---

## 8. MODULE 5: TIME-SERIES DATA LAKE (`tsdb_dumper/` & `docker/`)

Written in Rust and managed via Docker Compose.

### 8.1. Core Features
*   **`docker-compose.yml`:** Instantiates `influxdb` (Port 8086) and `grafana` (Port 3000) inside the `goldmine-net` virtual network.
*   **`tsdb_dumper` Microservice:** A Rust binary that maps `/dev/shm/goldmine_tick_shm`. It silently listens for new ticks (just like the C++ engine) and batches them. Every 100 ticks, it executes an asynchronous HTTP `POST` to InfluxDB using `reqwest`. This creates a permanent, high-resolution data lake for Transaction Cost Analysis (TCA).

---

## 9. MODULE 6: MACHINE LEARNING BRAIN (`brain/`)

Written in Python. All historical analysis, backtesting, and AI model generation occurs here.

### 9.1. Automated Data Pipeline (`data_downloader.py` & `feature_pipeline.py`)
*   **Downloader:** Automatically pulls historical `PAXGUSDT-bookTicker-YYYY-MM.zip` archives directly from Binance Vision, extracting the CSVs into `data/raw/`.
*   **Feature Engineering:** Uses Pandas to resample raw irregular ticks into deterministic 1-second fixed-interval snapshots (`OHLC`).
*   **Indicators:** Computes Institutional markers: 14-period RSI, 14-period ATR (for dynamic slippage calculation), 50-period Rolling Z-Score (mean reversion), and Spread Basis Points.
*   **Parquet Export:** Drops all `NaN` values and saves the resulting dataframe as a Snappy-compressed `.parquet` file in `data/processed/`, allowing massive datasets to be loaded by the GPU instantly.

### 9.2. Deep Reinforcement Learning Agent (`rl_agent.py`)
*   **Custom Gym Environment:** `GoldmineTradingEnv` simulates the exact C++ engine logic. Its `action_space` represents the output parameters: `[risk_pct, tp_multiplier, sl_multiplier, z_score_threshold]`.
*   **Deep Neural Network Architecture:** Built on `stable-baselines3`. Uses a custom `BaseFeaturesExtractor` (PyTorch) with Dense Layers, Layer Normalization, and Dropout (to prevent overfitting). The output is split into separate Policy (`pi`) and Value (`vf`) networks.
*   **Training Objective:** The agent learns to output the most profitable parameter combinations across millions of timesteps by interacting with historical Z-Scores and ATR dynamics.
*   **Output:** Generates `models/best_parameters.json` upon completion.

### 9.3. Kaggle Headless Runner (`kaggle_runner.py`)
*   Automates Cloud GPU training. Generates `kernel-metadata.json`, pushes the `rl_agent.py` and parquet datasets directly to a Kaggle 30-Hour free compute instance via the Kaggle API, polls for completion, and downloads the output weights automatically.

---

## 10. MODULE 7: DETERMINISTIC BACKTESTER (`backtester/`)

Written in Rust. Validates execution logic outside of the live C++ engine.

### 10.1. Core Features (`backtester/src/main.rs`)
*   **Realistic Queue Position Modeling:** Tracks cumulative volume traded at specific price levels. A simulated limit order is not filled until the traded volume exceeds the order's estimated queue position, preventing wildly optimistic backtest results.
*   **Execution Friction & Latency Injection:** Allows configuration of static network delays (e.g., $2ms$) and dynamically applies slippage based on the pre-computed ATR, simulating the physical cost of paying the spread during market orders.

---

## 11. COMPLETE DEVELOPMENT WORKFLOW

### Step 1: Environment Setup
Ensure you are running on Linux (Ubuntu 22.04+ recommended) with Docker, GCC 11+, and Cargo installed.
```bash
# Start the Data Lake
cd docker && docker-compose up -d

# Set up Python ML environment
python3 -m venv venv
source venv/bin/activate
pip install pandas pyarrow stable-baselines3[extra] torch optuna kaggle
```

### Step 2: Compile C++ Core
```bash
# Build Execution Engine
mkdir -p core/build && cd core/build
cmake .. && make -j

# Build Ingestion Feed
mkdir -p ../../ingest/build && cd ../../ingest/build
cmake .. && make -j
```

### Step 3: Compile Rust Services
```bash
cd orchestrator && cargo build --release
cd ../tsdb_dumper && cargo build --release
cd ../risk_server && cargo build --release
cd ../backtester && cargo build --release
```

### Step 4: Train the Model (Optional)
```bash
cd brain
python3 data_downloader.py
python3 feature_pipeline.py
# (Requires Kaggle API keys configured in ~/.kaggle/kaggle.json)
python3 kaggle_runner.py
```

### Step 5: Launch Goldmine
```bash
export BINANCE_API_KEY="your_api_key_here"
export BINANCE_API_SECRET="your_secret_here"

cd orchestrator
./target/release/orchestrator
```
You will immediately see the Orchestrator execute Phase 0 through Phase 4.

---

## 12. OPERATIONAL INVARIANTS & SAFETY CONSTRAINTS

**If you are a new developer submitting a Pull Request, you MUST adhere to the following rules:**

1. **NO HEAP ALLOCATIONS IN `core/` AFTER BOOT:** Once the `expert_engine` enters the `spin_wait_next()` loop, you are absolutely forbidden from using `std::string`, `std::vector`, or calling `new`/`malloc`. If you need arrays, use `std::array`. If you need strings, use fixed `char[]` buffers.
2. **DO NOT ALTER `SharedTick` PADDING:** The `SharedTick` struct in `live_ingestion.hpp` is strictly padded to 64 bytes (`alignas(64)`). Changing this will break the L1 cache alignment and cause false sharing, destroying the engine's microsecond latency.
3. **NEVER BYPASS THE ORCHESTRATOR:** Do not launch `expert_engine` directly from the terminal. The orchestrator's Phase 0 is mandatory because it cleans `/dev/shm`. Launching directly will cause the C++ engine to read corrupted pointers from previous segfaults.
4. **RISK SERVER MUST REMAIN ISOLATED:** Never integrate the Drop Copy risk logic into the C++ engine. The physical separation between the C++ binary and the Rust binary is an institutional requirement to ensure the Kill Switch survives an engine hallucination or memory corruption event.

---

*Welcome to the cutting edge of quantitative engineering. Respect the latency.*
