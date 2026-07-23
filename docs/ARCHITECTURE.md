# Goldmine XAU/USD Algorithmic Trading System
## Developer Architecture & Onboarding Guide

Welcome to the Goldmine trading engine. This document serves as the absolute source of truth for the system's architecture, design philosophy, and module interactions. It is designed to allow any senior developer to immediately understand and continue building the platform.

---

## 1. System Overview & Philosophy

Goldmine is a high-frequency, institutional-grade algorithmic trading platform built specifically for XAU/USD (currently running on PAXG/USDT via Binance). 

**Core Philosophies:**
- **Zero-Allocation Hot Path:** The C++20 trading engine is strictly forbidden from using `new`, `std::vector` (resizing), or dynamic `std::string` on the hot tick-processing path. All data structures are lock-free and pre-allocated.
- **Out-of-Band Intelligence:** Machine Learning (RL, CNNs, XGBoost) never runs inside the C++ engine. ML runs in asynchronous Python processes. The C++ engine remains ultra-lightweight, picking up ML parameter updates dynamically from a shared TOML config file.
- **Fail-Safe Execution:** The engine refuses to trade without an explicitly computed Transaction Cost hurdle and enforces hard circuit breakers. 

---

## 2. Component Architecture (The 4 Layers)

The system is fully containerized and separated into four distinct domains.

### 2.1 The Data Ingestion Layer (Python -> POSIX SHM -> C++)
- **Binance WebSocket Bridge (`scripts/linux_websocket_bridge.py`):** Connects to the Binance WSS feed.
- **Shared Memory (IPC):** Ticks are written directly to a volatile-protected POSIX shared memory block (`/dev/shm/goldmine_shm`).
- **C++ Tick Reader (`include/goldmine/live_ingestion.hpp`):** A dedicated thread in C++ spins on the SHM block. The moment a tick arrives, it is parsed and dispatched in <100 nanoseconds.

### 2.2 The Core Engine (C++20)
The heart of the system resides in `src/expert_engine.cpp`. It runs a 4-thread lock-free pipeline:
1. **Indicator Thread:** Computes EMA, RSI, ATR, and Z-Scores using SIMD.
2. **Logic Thread:** Evaluates price-action rules (`gold_rules.hpp`).
3. **Risk & Cost Engine:** 
   - Uses a Kelly-inspired fraction tracker (`account_state.hpp`).
   - Rejects signals if Take Profit is smaller than the spread + fees + slippage (`cost_model.hpp`).
4. **Execution Router (`execution/order_manager.hpp`):** Wraps exchange connectors and enforces hard daily-loss and position limits before sending orders.

### 2.3 The Intelligence Layer (Python ML Stack)
Located in `ml/`, this layer acts as the "Brain".
- **Database (`trades.db`):** The C++ engine logs every completed trade to a local SQLite database via the `TradeStore` module.
- **Regime Detector (`ml/regime_detector.py`):** A PyTorch 1D-CNN that classifies the market state.
- **Signal Filter (`ml/signal_model.py`):** An XGBoost probability model that acts as a final gatekeeper.
- **PPO Agent (`ml/rl_agent.py`):** Uses `stable-baselines3` to train an RL agent against the `trades.db` to optimally tune strategy hyperparameters.
- **Parameter Server (`ml/parameter_server.py`):** A FastAPI bridge that the RL agent hits to update `config/goldmine.toml`, which the C++ engine reloads.

### 2.4 The Platform Layer (API & Dashboard)
- **FastAPI Server (`api/server.py`):** Exposes REST endpoints for trade history and config management.
- **WebSocket Streaming (`api/ws_manager.py`):** Streams database updates instantly.
- **Web UI (`api/dashboard/index.html`):** A premium glassmorphic, dark-mode dashboard showing a live equity curve via Chart.js and recent execution logs.

---

## 3. Directory Structure

```text
goldmine/
├── api/                        # FastAPI Server & Dashboard
│   ├── dashboard/index.html    # Glassmorphic Web UI
│   ├── models/schemas.py       # Pydantic typing
│   ├── database.py             # SQLite reader
│   └── server.py               # REST / WebSocket server
├── config/
│   └── goldmine.toml           # Mutable RL-tuned config
├── docker/                     # Containerization
│   ├── docker-compose.yml
│   └── Dockerfile.*
├── docs/
│   └── ARCHITECTURE.md         # You are here
├── include/goldmine/           # C++20 Core Headers
│   ├── execution/              # CRTP zero-allocation trade routing
│   │   ├── base_connector.hpp
│   │   ├── binance_connector.hpp
│   │   ├── paper_connector.hpp
│   │   └── order_manager.hpp
│   ├── account_state.hpp       # Dynamic equity tracking
│   ├── cost_model.hpp          # Dynamic volume-slippage & fee model
│   ├── pipeline.hpp            # 4-thread lock-free execution
│   └── trade_store.hpp         # SQLite C-API persistence
├── ml/                         # Python Intelligence Stack
│   ├── training_env.py         # Gymnasium environment for RL
│   ├── rl_agent.py             # PPO Model
│   ├── signal_model.py         # XGBoost classifier
│   └── regime_detector.py      # PyTorch 1D-CNN
├── scripts/                    # Legacy/Data Bridges
└── src/
    └── expert_engine.cpp       # Main C++ binary entry point
```

---

## 4. Key Design Patterns to Follow

If you are contributing to this codebase, strict adherence to these patterns is mandatory:

### 4.1 No Virtual Dispatch on the Hot Path
The `BaseConnector` in the execution layer uses the **Curiously Recurring Template Pattern (CRTP)**. 
```cpp
template <typename Derived>
class BaseConnector {
    bool submit_order(const Order& order) {
        return static_cast<Derived*>(this)->impl_submit_order(order);
    }
};
```
*Why?* V-tables require pointer chasing which thrashes the CPU cache. CRTP statically resolves the call at compile time.

### 4.2 I/O Offloading
Never block the main loop with network requests (e.g., REST/WebSockets). Use lock-free ring buffers (like `LockFreeOrderQueue` in `binance_connector.hpp`) to hand off data to an I/O worker thread.

### 4.3 Deterministic Memory
If you need dynamic allocation, do it *before* the main `while(true)` spin-loop in `expert_engine.cpp` starts. Once the market opens, `malloc` and `free` are strictly prohibited. 

---

## 5. Build & Deployment

To deploy the full multi-container production stack:
```bash
docker-compose -f docker/docker-compose.yml up --build -d
```
This spins up:
1. The **C++ Engine** container (ultra-low latency execution).
2. The **API/Dashboard** container (Port 8000).
3. The **ML Parameter Server** container (PPO Agent updates).
