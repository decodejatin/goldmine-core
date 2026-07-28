#pragma once
// =============================================================================
// order_manager.hpp — Central Execution Router & Safety Guard
//
// Wraps a BaseConnector. Implements production safety constraints:
//   - Position size hard limits
//   - Daily loss auto-stop
//   - Stale price detection
// =============================================================================
#include "base_connector.hpp"
#include "goldmine/account_state.hpp"
#include <unordered_map>
#include <chrono>
#include <cstdio>
#include <cmath>

namespace goldmine::execution {

template <typename Connector>
class OrderManager {
public:
    OrderManager(Connector& connector, double daily_loss_limit, double max_position_size)
        : connector_(connector)
        , daily_loss_limit_(daily_loss_limit)
        , max_position_size_(max_position_size) {}

    bool start() {
        return connector_.connect();
    }

    void stop() {
        connector_.disconnect();
    }

    // Hot-path tick injection for stale price detection
    void on_tick(uint64_t tick_time_ms) {
        last_tick_time_ = tick_time_ms;
    }

    bool is_price_stale(uint64_t current_time_ms) const noexcept {
        return (current_time_ms - last_tick_time_) > 30000; // 30 seconds stale
    }

    // Safety-gated order submission
    bool place_order(uint64_t client_id, OrderSide side, double qty, double price, uint64_t current_time_ms, const AccountState& account) {
        // 1. Check Circuit Breaker
        if (account.is_trading_halted()) {
            std::printf("[GUARD] Rejected: Account is halted by circuit breaker.\n");
            return false;
        }

        // 2. Check Daily Loss Limit
        if (account.realized_pnl <= -daily_loss_limit_) {
            std::printf("[GUARD] Rejected: Daily loss limit reached (-$%.2f).\n", daily_loss_limit_);
            return false;
        }

        // 3. Check Position Size Hard Limit
        if (qty > max_position_size_) {
            std::printf("[GUARD] Rejected: Quantity %.4f exceeds hard limit %.4f.\n", qty, max_position_size_);
            return false;
        }

        // 4. Stale Price Detection
        if (is_price_stale(current_time_ms)) {
            std::printf("[GUARD] Rejected: Price feed is stale (>30s). Halting execution.\n");
            return false;
        }

        // Proceed
        Order o{};
        o.client_order_id = client_id;
        o.side = side;
        o.type = (price <= 0.0) ? OrderType::MARKET : OrderType::LIMIT;
        o.price = price;
        o.quantity = qty;
        o.status = OrderStatus::NEW;
        o.filled_qty = 0.0;
        o.avg_fill_price = 0.0;

        if (connector_.submit_order(o)) {
            std::printf("[EXEC] Order %llu submitted successfully.\n", (unsigned long long)client_id);
            return true;
        }
        return false;
    }

    void poll() {
        connector_.poll_execution_reports();
    }

private:
    Connector& connector_;
    double daily_loss_limit_;
    double max_position_size_;
    uint64_t last_tick_time_{0};
};

} // namespace goldmine::execution
