#pragma once
#include "goldmine/rule_registry.hpp"
namespace goldmine {

// ==========================================================================
// DISABLED: The 130+ multi-rule system produces noisy conviction scores
// on low-frequency PAXG/USDT feeds. Mean-reversion strategy is implemented
// directly in expert_engine.cpp instead.
//
// These rules can be re-enabled for backtesting on real XAU/USD tick data
// where DXY, XAG, and yield feeds are available.
// ==========================================================================
inline void register_all_gold_rules(RuleRegistry& reg, const EngineConfig&) noexcept {
    // Intentionally empty — strategy logic is in expert_engine.cpp
    (void)reg;
}

} // namespace goldmine
