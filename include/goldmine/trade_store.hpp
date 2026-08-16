#pragma once
// =============================================================================
// trade_store.hpp — Asynchronous Data Logging (Module 1)
//
// Lock-free SQLite trade & equity persistence.
// Hot path uses SPSC lock-free queue, background thread writes to DB.
// =============================================================================
#include <string>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "sqlite3.h"
#include "goldmine/lockfree_queue.hpp" // Assuming this has SpscQueue
#include "goldmine/cpu_affinity.hpp"

namespace goldmine {

enum class TelemetryType : uint8_t {
    TRADE = 1,
    EQUITY = 2
};

// Unified Frame for both Trade and Equity logs
struct TelemetryFrame {
    TelemetryType type;
    
    // Trade fields
    uint64_t open_time;
    uint64_t close_time;
    char direction[8];
    double entry_price;
    double exit_price;
    double qty;
    double pnl_usd;
    double fees;
    double sl;
    double tp;
    double conviction;
    uint32_t rules_mask;
    uint64_t duration_ticks;
    
    // Equity fields (reusing some fields to save space)
    uint64_t timestamp;
    double equity;
    double drawdown_pct;
    int trade_count;
};

class TradeStore {
public:
    TradeStore() : db_(nullptr), running_(false) {}

    ~TradeStore() {
        stop();
    }

    bool init(const std::string& db_path) {
        if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
            std::fprintf(stderr, "Failed to open SQLite db: %s\n", sqlite3_errmsg(db_));
            return false;
        }

        const char* schema = 
            "CREATE TABLE IF NOT EXISTS trades ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "open_time INTEGER, close_time INTEGER, direction TEXT, "
            "entry_price REAL, exit_price REAL, qty REAL, pnl_usd REAL, "
            "fees REAL, sl REAL, tp REAL, conviction REAL, rules_mask INTEGER, duration_ticks INTEGER);"
            "CREATE TABLE IF NOT EXISTS equity_curve ("
            "timestamp INTEGER, equity REAL, drawdown_pct REAL, trade_count INTEGER);";

        if (sqlite3_exec(db_, schema, nullptr, nullptr, nullptr) != SQLITE_OK) return false;

        const char* ins_trade = "INSERT INTO trades (open_time, close_time, direction, entry_price, exit_price, qty, pnl_usd, fees, sl, tp, conviction, rules_mask, duration_ticks) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
        sqlite3_prepare_v2(db_, ins_trade, -1, &stmt_trade_, nullptr);

        const char* ins_eq = "INSERT INTO equity_curve (timestamp, equity, drawdown_pct, trade_count) VALUES (?, ?, ?, ?);";
        sqlite3_prepare_v2(db_, ins_eq, -1, &stmt_eq_, nullptr);
        
        // Start background worker
        running_ = true;
        worker_ = std::thread(&TradeStore::logger_thread, this);
        return true;
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        if (worker_.joinable()) worker_.join();
        
        if (stmt_trade_) sqlite3_finalize(stmt_trade_);
        if (stmt_eq_) sqlite3_finalize(stmt_eq_);
        if (db_) sqlite3_close(db_);
    }

    // HOT-PATH: Zero-allocation, Lock-free push
    bool log_trade(const TelemetryFrame& frame) {
        return queue_.try_push(frame);
    }
    
    bool log_equity(const TelemetryFrame& frame) {
        return queue_.try_push(frame);
    }

private:
    void logger_thread() {
        auto_pin_thread(5);
        sqlite3_exec(db_, "PRAGMA synchronous = OFF", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "PRAGMA journal_mode = WAL", nullptr, nullptr, nullptr);

        while (running_) {
            process_queue();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Final drain
        process_queue();
    }

    void process_queue() {
        bool in_transaction = false;
        int count = 0;

        while (auto frame_ptr = queue_.try_pop()) {
            if (!in_transaction) {
                sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
                in_transaction = true;
            }

            const auto& frame = *frame_ptr;
            if (frame.type == TelemetryType::TRADE) {
                sqlite3_reset(stmt_trade_);
                sqlite3_bind_int64(stmt_trade_, 1, frame.open_time);
                sqlite3_bind_int64(stmt_trade_, 2, frame.close_time);
                sqlite3_bind_text(stmt_trade_, 3, frame.direction, -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(stmt_trade_, 4, frame.entry_price);
                sqlite3_bind_double(stmt_trade_, 5, frame.exit_price);
                sqlite3_bind_double(stmt_trade_, 6, frame.qty);
                sqlite3_bind_double(stmt_trade_, 7, frame.pnl_usd);
                sqlite3_bind_double(stmt_trade_, 8, frame.fees);
                sqlite3_bind_double(stmt_trade_, 9, frame.sl);
                sqlite3_bind_double(stmt_trade_, 10, frame.tp);
                sqlite3_bind_double(stmt_trade_, 11, frame.conviction);
                sqlite3_bind_int(stmt_trade_, 12, frame.rules_mask);
                sqlite3_bind_int64(stmt_trade_, 13, frame.duration_ticks);
                sqlite3_step(stmt_trade_);
            } else if (frame.type == TelemetryType::EQUITY) {
                sqlite3_reset(stmt_eq_);
                sqlite3_bind_int64(stmt_eq_, 1, frame.timestamp);
                sqlite3_bind_double(stmt_eq_, 2, frame.equity);
                sqlite3_bind_double(stmt_eq_, 3, frame.drawdown_pct);
                sqlite3_bind_int(stmt_eq_, 4, frame.trade_count);
                sqlite3_step(stmt_eq_);
            }
            
            if (++count >= 1000) {
                sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
                in_transaction = false;
                count = 0;
            }
        }

        if (in_transaction) {
            sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
        }
    }

    sqlite3* db_;
    sqlite3_stmt* stmt_trade_ = nullptr;
    sqlite3_stmt* stmt_eq_ = nullptr;
    
    std::atomic<bool> running_;
    std::thread worker_;
    SpscQueue<TelemetryFrame, 8192> queue_{};
};

} // namespace goldmine
