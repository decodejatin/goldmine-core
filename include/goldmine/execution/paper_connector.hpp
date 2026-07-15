#pragma once
// =============================================================================
// paper_connector.hpp — Simulated Execution Engine
//
// Matches orders instantly based on the latest tick data, tracking state internally.
// Perfect for forward-testing RL agents.
// =============================================================================
#include "base_connector.hpp"
#include <unordered_map>
#include <cstdio>

namespace goldmine::execution {

class PaperConnector : public BaseConnector<PaperConnector> {
public:
    PaperConnector() = default;

    // Interface implementations
    bool impl_connect() {
        std::printf("[PAPER] Connected to Paper Execution Engine.\n");
        return true;
    }

    bool impl_disconnect() {
        std::printf("[PAPER] Disconnected from Paper Execution Engine.\n");
        return true;
    }

    bool impl_submit_order(const Order& order) {
        Order copy = order;
        // In paper trading, MARKET orders fill immediately
        if (copy.type == OrderType::MARKET) {
            copy.status = OrderStatus::FILLED;
            copy.filled_qty = copy.quantity;
            copy.avg_fill_price = copy.price; // Expected to be passed as current bid/ask
            std::printf("[PAPER-EXEC] Market %s filled at %.2f\n", 
                copy.side == OrderSide::BUY ? "BUY" : "SELL", copy.avg_fill_price);
        } else {
            copy.status = OrderStatus::NEW;
        }
        
        active_orders_[copy.client_order_id] = copy;
        return true;
    }

    bool impl_cancel_order(uint64_t client_order_id) {
        auto it = active_orders_.find(client_order_id);
        if (it != active_orders_.end()) {
            it->second.status = OrderStatus::CANCELED;
            std::printf("[PAPER-EXEC] Order %llu canceled.\n", (unsigned long long)client_order_id);
            return true;
        }
        return false;
    }

    OrderStatus impl_query_status(uint64_t client_order_id) {
        auto it = active_orders_.find(client_order_id);
        if (it != active_orders_.end()) {
            return it->second.status;
        }
        return OrderStatus::REJECTED;
    }

    void impl_poll_execution_reports() {
        // No-op for paper, as execution happens instantly in submit or via tick-matching elsewhere
    }

    // Helper for paper engine to match limit/stop orders against a new tick
    void match_against_tick(double current_bid, double current_ask) {
        for (auto& [id, order] : active_orders_) {
            if (order.status != OrderStatus::NEW) continue;

            if (order.side == OrderSide::BUY && order.type == OrderType::STOP_LOSS && current_ask >= order.price) {
                order.status = OrderStatus::FILLED;
                order.filled_qty = order.quantity;
                order.avg_fill_price = current_ask;
            } else if (order.side == OrderSide::SELL && order.type == OrderType::STOP_LOSS && current_bid <= order.price) {
                order.status = OrderStatus::FILLED;
                order.filled_qty = order.quantity;
                order.avg_fill_price = current_bid;
            }
            // Similar logic for Limit / Take Profit...
        }
    }

private:
    std::unordered_map<uint64_t, Order> active_orders_;
};

} // namespace goldmine::execution
