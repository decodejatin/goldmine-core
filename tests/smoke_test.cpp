// =============================================================================
// smoke_test.cpp — Compile-time and runtime sanity tests for the Goldmine engine
// =============================================================================

#include "goldmine/types.hpp"
#include "goldmine/lockfree_queue.hpp"
#include "goldmine/bitmask_engine.hpp"
#include "goldmine/rule_registry.hpp"
#include "goldmine/indicators.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <type_traits>

using namespace goldmine;

// ---------------------------------------------------------------------------
// Static assertions — verified at compile time, zero runtime cost
// ---------------------------------------------------------------------------
static_assert(std::is_trivially_copyable_v<Tick>);
static_assert(std::is_trivially_copyable_v<DxyTick>);
static_assert(std::is_trivially_copyable_v<IndicatorSnapshot>);
static_assert(std::is_trivially_copyable_v<EngineSignal>);
static_assert(std::is_trivially_copyable_v<WideBitmask>);
static_assert(sizeof(WideBitmask) == BITMASK_WORDS * 8);
static_assert(WideBitmask::NUM_BITS == 1024);
static_assert(alignof(WideBitmask) == 32, "WideBitmask must be AVX2-aligned");

// ---------------------------------------------------------------------------
// Test: WideBitmask basic operations
// ---------------------------------------------------------------------------
void test_wide_bitmask() {
    WideBitmask m{};
    assert(m.popcount() == 0);

    m.set(0);
    assert(m.test(0));
    assert(m.popcount() == 1);

    m.set(63);
    assert(m.test(63));
    m.set(64);
    assert(m.test(64));
    m.set(999);
    assert(m.test(999));
    assert(m.popcount() == 4);

    m.clear(64);
    assert(!m.test(64));
    assert(m.popcount() == 3);

    // set_if (branchless)
    m.reset();
    m.set_if(100, true);
    m.set_if(200, false);
    assert(m.test(100));
    assert(!m.test(200));

    std::printf("  [PASS] WideBitmask basic ops\n");
}

// ---------------------------------------------------------------------------
// Test: AVX2 bitmask_ops
// ---------------------------------------------------------------------------
void test_bitmask_simd_ops() {
    WideBitmask a{}, b{};
    a.set(0); a.set(10); a.set(100);
    b.set(0); b.set(20); b.set(100);

    // AND
    WideBitmask c = a;
    bitmask_ops::and_inplace(c, b);
    assert(c.test(0));
    assert(!c.test(10));
    assert(!c.test(20));
    assert(c.test(100));
    assert(c.popcount() == 2);

    // OR
    WideBitmask d = a;
    bitmask_ops::or_inplace(d, b);
    assert(d.test(0) && d.test(10) && d.test(20) && d.test(100));
    assert(d.popcount() == 4);

    // any_set
    WideBitmask empty{};
    assert(!bitmask_ops::any_set(empty));
    assert(bitmask_ops::any_set(a));

    std::printf("  [PASS] AVX2 bitmask_ops\n");
}

// ---------------------------------------------------------------------------
// Test: Score calculation
// ---------------------------------------------------------------------------
void test_scoring() {
    alignas(32) std::array<double, MAX_RULES> weights{};
    weights[0]  = +0.5;
    weights[10] = -0.3;
    weights[100]= +0.2;

    WideBitmask m{};
    m.set(0); m.set(10); m.set(100);

    double score = bitmask_ops::score_mask(m, weights.data());
    // 0.5 + (-0.3) + 0.2 = 0.4
    assert(score > 0.39 && score < 0.41);

    std::printf("  [PASS] Scoring\n");
}

// ---------------------------------------------------------------------------
// Test: SPSC Queue
// ---------------------------------------------------------------------------
void test_spsc_queue() {
    SpscQueue<int, 8> q;
    assert(q.empty());

    assert(q.try_push(42));
    assert(q.try_push(99));
    assert(!q.empty());

    auto v1 = q.try_pop();
    assert(v1.has_value() && *v1 == 42);
    auto v2 = q.try_pop();
    assert(v2.has_value() && *v2 == 99);
    assert(q.empty());

    // Fill to capacity (7 items in an 8-slot ring)
    for (int i = 0; i < 7; ++i) assert(q.try_push(i));
    assert(!q.try_push(100));  // full

    std::printf("  [PASS] SPSC Queue\n");
}

// ---------------------------------------------------------------------------
// Test: Rule Registry
// ---------------------------------------------------------------------------
void test_rule_registry() {
    RuleRegistry reg;

    auto idx0 = reg.add_rule("always_true", +0.5,
        [](const MarketState&, const IndicatorSnapshot&) -> bool { return true; });
    auto idx1 = reg.add_rule("always_false", -0.3,
        [](const MarketState&, const IndicatorSnapshot&) -> bool { return false; });

    assert(idx0 == 0);
    assert(idx1 == 1);
    assert(reg.count() == 2);

    MarketState state{};
    WideBitmask mask = reg.evaluate_all(state);
    assert(mask.test(0));
    assert(!mask.test(1));
    assert(mask.popcount() == 1);

    SessionFlags sess{false, false, false};
    double score = reg.score(mask, sess);
    assert(score > 0.49 && score < 0.51);  // only rule 0 fired: +0.5

    std::printf("  [PASS] Rule Registry\n");
}

// ---------------------------------------------------------------------------
// Test: Default Gold rules registration
// ---------------------------------------------------------------------------
#include "goldmine/gold_rules.hpp"
void test_default_gold_rules() {
    RuleRegistry reg;
    EngineConfig cfg{};
    register_all_gold_rules(reg, cfg);
    std::printf("  [PASS] All Gold rules (%zu registered)\n", reg.count());
}

// ---------------------------------------------------------------------------
// Test: Indicator computations (basic sanity)
// ---------------------------------------------------------------------------
void test_indicators() {
    TickStore store{};

    // Feed 100 synthetic ticks
    for (int i = 0; i < 100; ++i) {
        Tick t{};
        t.timestamp_ns = static_cast<Nanos>(i) * 1'000'000'000LL;
        t.bid    = 2350.0 + static_cast<double>(i % 10) * 0.1;
        t.ask    = t.bid + 0.10;
        t.last   = t.bid + 0.05;
        t.volume = 1.0 + static_cast<double>(i % 3);
        store.push(t, 104.50);
    }

    assert(store.filled() == 100);

    // VWAP
    auto vwap = indicators::compute_vwap(store, 100);
    assert(vwap.vwap > 2350.0 && vwap.vwap < 2360.0);

    // ATR
    auto atr = indicators::compute_atr(store);
    assert(atr.atr > 0.0);

    // RSI
    auto rsi = indicators::compute_rsi(store);
    assert(rsi > 0.0 && rsi < 100.0);

    // Bollinger
    auto bb = indicators::compute_bollinger(store);
    assert(bb.upper > bb.lower);
    assert(bb.width > 0.0);

    // Z-Score
    auto zs = indicators::compute_zscore(store);
    assert(zs.stddev > 0.0);

    std::printf("  [PASS] Indicators (VWAP=%.2f, ATR=%.4f, RSI=%.1f)\n",
                vwap.vwap, atr.atr, rsi);
}

#include "goldmine/online_learner.hpp"

// ---------------------------------------------------------------------------
// Test: Power-of-2 constraints and Bitwise masking
// ---------------------------------------------------------------------------
void test_power_of_2_masks() {
    using Q = SpscQueue<int, 1024>;
    static_assert(Q::CAPACITY == 1024);
    static_assert(Q::MASK == 1023);
    assert((1024 & 1023) == 0); // basic sanity check
    std::printf("  [PASS] Power-of-2 LockFree Queue Constraints\n");
}

// ---------------------------------------------------------------------------
// Test: Order Flow Microstructure (OFI/Micro-price)
// ---------------------------------------------------------------------------
void test_ofi_micro_price() {
    TickStore store{};
    // Generate a sequence with a clear buying imbalance
    for (int i = 0; i < 20; ++i) {
        Tick t{};
        t.timestamp_ns = i * 1'000'000'000LL;
        // Bids moving up, asks staying same
        t.bid = 2350.0 + i * 0.1;
        t.ask = 2355.0;
        t.last = t.bid;
        t.volume = 10.0 + i;
        store.push(t, 100.0);
    }
    auto ofi = indicators::compute_order_flow(store, 20);
    // Since bids went up, OFI should be heavily positive
    assert(ofi.ofi_sum > 0.0);
    assert(ofi.micro_price > 0.0);
    std::printf("  [PASS] Microstructure (OFI=%.1f, MicroDelta=%.3f)\n",
                ofi.ofi_sum, ofi.micro_delta);
}

// ---------------------------------------------------------------------------
// Test: Online Learner O(N) SGD
// ---------------------------------------------------------------------------
void test_online_learner() {
    OnlineLearner learner{};
    double initial[10] = {0};
    learner.initialize(initial, 10);
    
    TradeOutcome outcome{};
    outcome.label = 1.0; // Win
    outcome.active_rules.set(3);
    outcome.active_rules.set(7);
    
    learner.update(outcome);
    
    // Weight 3 and 7 should now be positive (η * error * 1)
    const double* w = learner.weights();
    assert(w[3] > 0.0);
    assert(w[7] > 0.0);
    assert(w[0] == 0.0); // inactive rule untouched
    
    std::printf("  [PASS] Online Learner (O(N) SGD update)\n");
}

// ===========================================================================
int main() {
    std::printf("[GOLDMINE] Running smoke tests...\n\n");

    test_wide_bitmask();
    test_bitmask_simd_ops();
    test_scoring();
    test_spsc_queue();
    test_power_of_2_masks();
    test_rule_registry();
    test_default_gold_rules();
    test_indicators();
    test_ofi_micro_price();
    test_online_learner();

    std::printf("\n[GOLDMINE] All tests passed.\n");
    return 0;
}
