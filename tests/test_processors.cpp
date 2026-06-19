// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

// tests/test_processors.cpp — Phase 6: Local + NoPartialFillExchange tests
// Tests the end-to-end order lifecycle through both processors.

#include <gtest/gtest.h>
#include <memory>

#include "hbt/backtest/exchange.hpp"
#include "hbt/backtest/local.hpp"
#include "hbt/backtest/order_bus.hpp"
#include "hbt/backtest/state.hpp"
#include "hbt/depth/hashmap_depth.hpp"
#include "hbt/models/asset_type.hpp"
#include "hbt/models/fee.hpp"
#include "hbt/models/latency.hpp"
#include "hbt/models/queue.hpp"
#include "hbt/types.hpp"

using namespace hbt;

// ── Convenience types for tests ───────────────────────────────────────────────
using AT = LinearAsset;
using LM = ConstantLatency;
using QM = RiskAdverseQueueModel;
using MD = HashMapMarketDepth;
using FM = TradingValueFeeModel<CommonFees>;
using Q = QM::OrderState; // double

using LocalT = Local<AT, LM, MD, FM>;
using ExchT = NoPartialFillExchange<AT, LM, QM, MD, FM>;
using PartialExchT = PartialFillExchange<AT, LM, QM, MD, FM>;

// ── Factory helper ────────────────────────────────────────────────────────────
// Creates a matched Local + Exchange pair sharing the same order buses.
static std::pair<std::unique_ptr<LocalT>, std::unique_ptr<ExchT>>
make_pair(int64_t entry_lat = 100, int64_t resp_lat = 200) {
    auto [e2l, l2e] = make_order_buses<LM>(ConstantLatency{entry_lat, resp_lat});

    auto local = std::make_unique<LocalT>(
        MD{0.1, 0.001}, State<AT, FM>{LinearAsset{1.0}, FM{CommonFees{0.0, 0.001}}},
        /*last_trades_cap=*/8, std::move(l2e));

    auto exch = std::make_unique<ExchT>(
        MD{0.1, 0.001}, State<AT, FM, Q>{LinearAsset{1.0}, FM{CommonFees{0.0, 0.001}}}, QM{},
        std::move(e2l));

    return {std::move(local), std::move(exch)};
}

// ── Helpers to build Events ───────────────────────────────────────────────────
static Event bid_depth_ev(double px, double qty, int64_t exch_ts, int64_t local_ts) {
    Event e{};
    e.ev = LOCAL_BID_DEPTH_EVENT;
    e.px = px;
    e.qty = qty;
    e.exch_ts = exch_ts;
    e.local_ts = local_ts;
    return e;
}
static Event ask_depth_ev(double px, double qty, int64_t exch_ts, int64_t local_ts) {
    Event e{};
    e.ev = LOCAL_ASK_DEPTH_EVENT;
    e.px = px;
    e.qty = qty;
    e.exch_ts = exch_ts;
    e.local_ts = local_ts;
    return e;
}
static Event exch_bid_depth_ev(double px, double qty, int64_t ts) {
    Event e{};
    e.ev = EXCH_BID_DEPTH_EVENT;
    e.px = px;
    e.qty = qty;
    e.exch_ts = ts;
    e.local_ts = ts;
    return e;
}
static Event exch_ask_depth_ev(double px, double qty, int64_t ts) {
    Event e{};
    e.ev = EXCH_ASK_DEPTH_EVENT;
    e.px = px;
    e.qty = qty;
    e.exch_ts = ts;
    e.local_ts = ts;
    return e;
}
static Event sell_trade_ev(double px, double qty, int64_t ts) {
    Event e{};
    e.ev = EXCH_SELL_TRADE_EVENT;
    e.px = px;
    e.qty = qty;
    e.exch_ts = ts;
    e.local_ts = ts;
    return e;
}

// ── Test: Local processes depth events ───────────────────────────────────────
TEST(LocalProcessor, DepthEvents) {
    auto [local, exch] = make_pair();
    local->process(bid_depth_ev(100.0, 5.0, 0, 1000));
    local->process(ask_depth_ev(100.1, 3.0, 0, 1001));
    // Local doesn't expose depth directly through LocalProcessor interface,
    // but we can call the concrete Local method
    auto &d = local->depth();
    EXPECT_DOUBLE_EQ(d.best_bid(), 100.0);
    EXPECT_DOUBLE_EQ(d.best_ask(), 100.1);
}

TEST(LocalProcessor, TradeAccumulated) {
    auto [local, exch] = make_pair();
    Event t{};
    t.ev = LOCAL_TRADE_EVENT;
    t.px = 100.0;
    t.qty = 1.0;
    t.exch_ts = 0;
    t.local_ts = 1000;
    local->process(t);
    EXPECT_EQ(local->last_trades().size(), 1u);
    local->clear_last_trades();
    EXPECT_EQ(local->last_trades().size(), 0u);
}

// ── Test: submit → reject (GTX crossing) ─────────────────────────────────────
// With negative latency the order gets immediately rejected.
TEST(LocalProcessor, NegativeLatencyRejectsOrder) {
    auto [local, exch] = make_pair(/*entry_lat=*/-50, /*resp_lat=*/200);
    local->submit_order(1, Side::Buy, 100.0, 1.0, OrdType::Limit, TimeInForce::GTC, 1000);

    // Rejection lands at local_ts + abs(-50) = 1050
    EXPECT_EQ(local->orders().size(), 1u);

    bool got = local->process_recv_order(1050, std::nullopt);
    EXPECT_FALSE(got); // no wait_id

    // The order should now be Expired (rejected New)
    const auto &o = local->orders().at(1);
    EXPECT_EQ(o.status, Status::Expired);
}

// ── Test: Normal order lifecycle ──────────────────────────────────────────────
// submit → exch receives at +100 → acks new → exch sends at +200 → local sees ack
TEST(Processors, OrderAckLifecycle) {
    auto [local, exch] = make_pair(100, 200);

    // Give exch a book so it can accept orders
    exch->process(exch_bid_depth_ev(99.9, 10.0, 500));
    exch->process(exch_ask_depth_ev(100.1, 10.0, 500));

    // Submit buy at 100.0 (inside spread, passive)
    local->submit_order(1, Side::Buy, 100.0, 1.0, OrdType::Limit, TimeInForce::GTC, 1000);

    // Exch receives at 1000 + 100 = 1100
    EXPECT_EQ(exch->earliest_recv_order_timestamp(), 1100);
    exch->process_recv_order(1100, std::nullopt);

    // Ack response scheduled at 1100 + 200 = 1300 on local side
    EXPECT_EQ(local->earliest_recv_order_timestamp(), 1300);
    local->process_recv_order(1300, std::nullopt);

    // Local order should now be acknowledged as New
    const auto &o = local->orders().at(1);
    EXPECT_EQ(o.status, Status::New);
    EXPECT_EQ(o.req, Status::None);
}

// ── Test: Fill on sell trade ──────────────────────────────────────────────────
TEST(Processors, BuyOrderFilledBySellTrade) {
    auto [local, exch] = make_pair(100, 200);

    // Set up exchange book: ask at 100.1 (so 100.0 is passive)
    exch->process(exch_bid_depth_ev(99.9, 5.0, 500));
    exch->process(exch_ask_depth_ev(100.1, 5.0, 500));

    // Submit buy order at 100.0, acknowledged as New
    local->submit_order(1, Side::Buy, 100.0, 1.0, OrdType::Limit, TimeInForce::GTC, 1000);
    exch->process_recv_order(1100, std::nullopt);  // ack
    local->process_recv_order(1300, std::nullopt); // receive ack

    // Set bid depth at 100.0 for queue model: 1 lot in front
    exch->process(exch_bid_depth_ev(100.0, 2.0, 1100));

    // A sell trade at 100.0 for qty > front_q_qty fills our order
    exch->process(sell_trade_ev(100.0, 5.0, 2000)); // trade > front → fills

    // The fill response should be scheduled
    int64_t fill_ts = local->earliest_recv_order_timestamp();
    EXPECT_GT(fill_ts, 2000);

    local->process_recv_order(fill_ts, std::nullopt);
    const auto &o = local->orders().at(1);
    EXPECT_EQ(o.status, Status::Filled);
    EXPECT_GT(local->position(), 0.0);
}

TEST(Processors, PartialFillPreservesLeavesQuantityAndUpdatesPosition) {
    auto [e2l, l2e] = make_order_buses<LM>(ConstantLatency{100, 200});
    auto local = std::make_unique<LocalT>(
        MD{0.1, 0.001}, State<AT, FM>{LinearAsset{1.0}, FM{CommonFees{0.0, 0.001}}}, 8,
        std::move(l2e));
    auto exch = std::make_unique<PartialExchT>(
        MD{0.1, 0.001}, State<AT, FM, Q>{LinearAsset{1.0}, FM{CommonFees{0.0, 0.001}}}, QM{},
        std::move(e2l));

    exch->process(exch_bid_depth_ev(100.0, 1.0, 500));
    exch->process(exch_ask_depth_ev(100.1, 5.0, 500));
    local->submit_order(1, Side::Buy, 100.0, 5.0, OrdType::Limit, TimeInForce::GTC, 1000);
    exch->process_recv_order(1100, std::nullopt);
    local->process_recv_order(1300, std::nullopt);

    exch->process(sell_trade_ev(100.0, 3.0, 2000));
    local->process_recv_order(local->earliest_recv_order_timestamp(), std::nullopt);

    const auto &order = local->orders().at(1);
    EXPECT_EQ(order.status, Status::PartiallyFilled);
    EXPECT_DOUBLE_EQ(order.exec_qty, 2.0);
    EXPECT_DOUBLE_EQ(order.leaves_qty, 3.0);
    EXPECT_DOUBLE_EQ(local->position(), 2.0);
}
