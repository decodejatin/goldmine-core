# SYSTEM DESIGN: Goldmine Trading Engine

## 1. Domain Modules

The "Goldmine" repository has been modularized into a decoupled, enterprise-grade architecture. The system is split into the following domain boundaries:

*   **`interfaces/cpp/goldmine/`**: Contains cross-module memory protocol definitions. It establishes the shared memory structs (`live_ingestion.hpp`, `param_shm.hpp`) that allow different processes to communicate with zero syscalls.
*   **`core/`**: The ultra-low-latency C++ execution engine. It strictly enforces zero-allocation on the hot path. Contains business logic, the 4-thread pipeline (`pipeline.hpp`), and execution systems (`risk_engine.hpp`, `expert_engine.cpp`).
*   **`ingest/`**: Responsible for connecting to external data feeds (e.g., Binance WebSockets) and normalizing tick data before pushing it into the C++ shared memory buffers.
*   **`brain/`**: The Machine Learning and RL components (`rl_agent.py`, `regime_detector.py`, `signal_model.py`) that periodically calibrate system weights and push parameter updates to the Control Plane.
*   **`gateway/`**: The secure FastAPI endpoints that expose the Control Plane to external services (e.g., parameter updates, system telemetry dashboard).
*   **`docs/`**: Architectural documentation and runbooks.

---

## 2. IPC Binary Layout

To achieve zero-allocation and sub-microsecond latency, processes communicate via POSIX shared memory (`/dev/shm`).

### Tick Shared Memory (`/dev/shm/goldmine_tick_shm`)
*   **Purpose:** High-frequency, lock-free tick publishing from the `ingest` processes to the `core` engine.
*   **Layout:** Ring buffers structured for single-producer, single-consumer consumption, avoiding standard mutexes.

### Parameter Shared Memory (`/dev/shm/goldmine_param_shm`)
*   **Purpose:** Secure Control Plane for updating runtime configurations (e.g., risk percentage, take-profit multipliers) from the `brain` to the `core`.
*   **Layout:**
    *   Strict `alignas(64)` padding for every field to prevent False Sharing and CPU cache-line bouncing.
    *   Size per field: 8 bytes of data + 56 bytes padding = 64 bytes total.
    *   Offsets: 
        * `0`   : `version_id` (uint64)
        * `64`  : `risk_pct` (double)
        * `128` : `tp_multiplier` (double)
        * `192` : `sl_multiplier` (double)
        * `256` : `conv_threshold` (double)
        * `320` : `p_profitable_gate_bps` (uint32)
        * `384` : `regime_id` (uint8)

---

## 3. Core Zero-Allocation Rules

Contributors working in `core/` MUST adhere to the following zero-allocation rules on the hot path (Threads 1, 2, 3, and 4):

1.  **No Dynamic Memory:** `new`, `delete`, `malloc`, `free`, `std::make_shared`, and `std::make_unique` are strictly prohibited on the hot path.
2.  **No Expanding Containers:** Avoid `std::vector::push_back` or map insertions. Pre-allocate all memory during initialization or use fixed-size arrays (`std::array`) and circular ring buffers.
3.  **Lock-Free Concurrency:** Mutexes (`std::mutex`) and condition variables are banned. Inter-thread communication must use pre-allocated lock-free SPSC queues.
4.  **No Standard Streams:** Do not use `std::cout` or `std::fprintf` on the hot path; use asynchronous SPSC loggers if telemetry is required.
5.  **Compile Strictness:** All code must compile cleanly under `-std=c++20 -O3 -Wall -Wextra -Wpedantic -Werror`.
