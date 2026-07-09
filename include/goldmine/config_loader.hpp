#pragma once
// =============================================================================
// config_loader.hpp — Phase 2: Lightweight TOML Config Parser
//
// Simple, zero-dependency parser for goldmine.toml format.
// =============================================================================
#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace goldmine {

class ConfigLoader {
public:
    struct Config {
        // [account]
        double starting_equity = 10.0;
        double max_drawdown_pct = 30.0;
        double circuit_breaker_equity = 7.0;

        // [costs]
        double maker_fee_bps = 10.0;
        double taker_fee_bps = 10.0;
        double slippage_bps = 2.0;
        double volume_penalty_bps_per_unit = 50.0;

        // [strategy]
        int signal_persist = 8;
        int warmup_ticks = 80;
        int cooldown_ticks = 25;
        double conv_threshold = 0.25;
        double min_sl = 2.0;
        double max_sl = 8.0;
        double min_tp = 0.60;
        double max_tp = 3.50;
        double trail_activate = 0.25;
        double trail_factor = 0.35;

        // [risk]
        double base_risk_pct = 2.0;
        double max_risk_pct = 4.0;
        double win_streak_bonus = 0.5;
        double drawdown_risk_cut = 1.0;
    };

    static Config load(const std::string& filepath) {
        Config cfg;
        std::ifstream file(filepath);
        if (!file.is_open()) {
            std::fprintf(stderr, "Could not open config file: %s. Using defaults.\n", filepath.c_str());
            return cfg;
        }

        std::string line;
        while (std::getline(file, line)) {
            // Trim whitespace
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);

            // Skip empty lines and comments
            if (line.empty() || line[0] == '#' || line[0] == '[') continue;

            auto delim = line.find('=');
            if (delim == std::string::npos) continue;

            std::string key = line.substr(0, delim);
            std::string val = line.substr(delim + 1);

            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            val.erase(0, val.find_first_not_of(" \t"));
            val.erase(val.find_last_not_of(" \t") + 1);

            // Parse values
            if (key == "starting_equity") cfg.starting_equity = std::stod(val);
            else if (key == "max_drawdown_pct") cfg.max_drawdown_pct = std::stod(val);
            else if (key == "circuit_breaker_equity") cfg.circuit_breaker_equity = std::stod(val);
            else if (key == "maker_fee_bps") cfg.maker_fee_bps = std::stod(val);
            else if (key == "taker_fee_bps") cfg.taker_fee_bps = std::stod(val);
            else if (key == "slippage_bps") cfg.slippage_bps = std::stod(val);
            else if (key == "volume_penalty_bps_per_unit") cfg.volume_penalty_bps_per_unit = std::stod(val);
            else if (key == "signal_persist") cfg.signal_persist = std::stoi(val);
            else if (key == "warmup_ticks") cfg.warmup_ticks = std::stoi(val);
            else if (key == "cooldown_ticks") cfg.cooldown_ticks = std::stoi(val);
            else if (key == "conv_threshold") cfg.conv_threshold = std::stod(val);
            else if (key == "min_sl") cfg.min_sl = std::stod(val);
            else if (key == "max_sl") cfg.max_sl = std::stod(val);
            else if (key == "min_tp") cfg.min_tp = std::stod(val);
            else if (key == "max_tp") cfg.max_tp = std::stod(val);
            else if (key == "trail_activate") cfg.trail_activate = std::stod(val);
            else if (key == "trail_factor") cfg.trail_factor = std::stod(val);
            else if (key == "base_risk_pct") cfg.base_risk_pct = std::stod(val);
            else if (key == "max_risk_pct") cfg.max_risk_pct = std::stod(val);
            else if (key == "win_streak_bonus") cfg.win_streak_bonus = std::stod(val);
            else if (key == "drawdown_risk_cut") cfg.drawdown_risk_cut = std::stod(val);
        }

        return cfg;
    }
};

} // namespace goldmine
