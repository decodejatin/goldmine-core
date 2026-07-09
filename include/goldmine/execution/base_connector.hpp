#pragma once
// =============================================================================
// base_connector.hpp — CRTP Zero-Allocation Execution Interface
//
// Defines the high-performance contract for sending and managing live orders
// across exchanges without virtual dispatch overhead.
// =============================================================================
#include <cstdint>
#include <string_view>

namespace goldmine::execution {

enum class OrderSide {
    BUY,
    SELL
};

enum class OrderType {
    MARKET,
    LIMIT,
    STOP_LOSS,
    TAKE_PROFIT
};

enum class OrderStatus {
    NEW,
    PARTIALLY_FILLED,
    FILLED,
    CANCELED,
    REJECTED
};

struct Order {
    uint64_t client_order_id;
    OrderSide side;
    OrderType type;
    double price;       // 0 for MARKET
    double quantity;
    OrderStatus status;
    double filled_qty;
    double avg_fill_price;
};

// CRTP Base for Connectors
template <typename Derived>
class BaseConnector {
public:
    // API Contract
    bool connect() {
        return static_cast<Derived*>(this)->impl_connect();
    }

    bool disconnect() {
        return static_cast<Derived*>(this)->impl_disconnect();
    }

    // Returns true if the order was successfully dispatched to the exchange
    bool submit_order(const Order& order) {
        return static_cast<Derived*>(this)->impl_submit_order(order);
    }

    // Cancel an active order
    bool cancel_order(uint64_t client_order_id) {
        return static_cast<Derived*>(this)->impl_cancel_order(client_order_id);
    }

    // Query the latest status of an order
    OrderStatus query_status(uint64_t client_order_id) {
        return static_cast<Derived*>(this)->impl_query_status(client_order_id);
    }

    // High-frequency hot-path method to drain incoming execution reports from WebSocket
    void poll_execution_reports() {
        static_cast<Derived*>(this)->impl_poll_execution_reports();
    }
};

} // namespace goldmine::execution
