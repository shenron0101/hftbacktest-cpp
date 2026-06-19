// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

// tests/test_models.cpp — Phase 3: model unit tests

#include <cmath>
#include <gtest/gtest.h>

#include "hbt/depth/hashmap_depth.hpp"
#include "hbt/models/asset_type.hpp"
#include "hbt/models/fee.hpp"
#include "hbt/models/latency.hpp"
#include "hbt/models/queue.hpp"

using namespace hbt;

// ── LinearAsset ───────────────────────────────────────────────────────────────
TEST(LinearAsset, Amount) {
    LinearAsset a(1.0);
    EXPECT_DOUBLE_EQ(a.amount(100.0, 2.0), 200.0);
}
TEST(LinearAsset, Equity) {
    LinearAsset a(1.0);
    // balance=1000, position=2, price=100, fee=5 → 1000 + 1*2*100 - 5 = 1195
    EXPECT_DOUBLE_EQ(a.equity(100.0, 1000.0, 2.0, 5.0), 1195.0);
}

// ── InverseAsset ──────────────────────────────────────────────────────────────
TEST(InverseAsset, Amount) {
    InverseAsset a(100.0);
    EXPECT_DOUBLE_EQ(a.amount(50000.0, 2.0), 100.0 * 2.0 / 50000.0);
}

// ── TradingValueFeeModel ──────────────────────────────────────────────────────
TEST(FeeModel, TradingValueMaker) {
    TradingValueFeeModel<CommonFees> fm(CommonFees{-0.0001, 0.0006});
    Order<> o;
    o.maker = true;
    EXPECT_DOUBLE_EQ(fm.amount(o, 1000.0), -0.1);
}
TEST(FeeModel, TradingValueTaker) {
    TradingValueFeeModel<CommonFees> fm(CommonFees{-0.0001, 0.0006});
    Order<> o;
    o.maker = false;
    EXPECT_DOUBLE_EQ(fm.amount(o, 1000.0), 0.6);
}

// ── RiskAdverseQueueModel ─────────────────────────────────────────────────────
TEST(RiskAdverseQueue, NewOrder) {
    HashMapMarketDepth d(0.1, 0.001);
    d.update_bid_depth(100.0, 10.0, 0);

    RiskAdverseQueueModel qm;
    Order<double> o;
    o.price_tick = 1000;
    o.side = Side::Buy;
    qm.new_order(o, d);
    EXPECT_DOUBLE_EQ(o.q, 10.0);
}

TEST(RiskAdverseQueue, TradeAdvancesQueue) {
    HashMapMarketDepth d(0.1, 0.001);
    d.update_bid_depth(100.0, 10.0, 0);

    RiskAdverseQueueModel qm;
    Order<double> o;
    o.price_tick = 1000;
    o.side = Side::Buy;
    qm.new_order(o, d);
    qm.trade(o, 7.0, d); // 7 lots traded
    EXPECT_DOUBLE_EQ(o.q, 3.0);

    // Not yet filled
    EXPECT_DOUBLE_EQ(qm.is_filled(o, d), 0.0);

    qm.trade(o, 5.0, d); // now front_q_qty = 3 - 5 = -2 → exec rounds to 2 lots
    double filled = qm.is_filled(o, d);
    EXPECT_GT(filled, 0.0);
}

// ── ProbQueueModel ────────────────────────────────────────────────────────────
TEST(ProbQueueModel, FillMath) {
    HashMapMarketDepth d(0.1, 0.001);
    d.update_bid_depth(100.0, 10.0, 0);

    ProbQueueModel<PowerProbQueueFunc> qm(PowerProbQueueFunc{2.0});
    Order<QueuePos> o;
    o.price_tick = 1000;
    o.side = Side::Buy;
    qm.new_order(o, d);
    EXPECT_DOUBLE_EQ(o.q.front_q_qty, 10.0);

    // Simulate 3 qty traded
    qm.trade(o, 3.0, d);
    EXPECT_DOUBLE_EQ(o.q.front_q_qty, 7.0);
    EXPECT_DOUBLE_EQ(o.q.cum_trade_qty, 3.0);

    // Depth changes: prev=10, new=5 → chg = 10-5 = 5, minus cum_trade=3 → chg=2
    // front=7, back=10-7=3, prob=3^2/(3^2+7^2)=9/58
    qm.depth(o, 10.0, 5.0, d);
    EXPECT_DOUBLE_EQ(o.q.cum_trade_qty, 0.0); // reset after depth update
    EXPECT_LT(o.q.front_q_qty, 7.0);          // queue advanced
}

TEST(IntpOrderLatency, InterpolatesEntryAndResponse) {
    IntpOrderLatency latency({
        OrderLatencyRow{100, 110, 130, 0},
        OrderLatencyRow{200, 230, 270, 0},
    });
    Order<> order;

    EXPECT_EQ(latency.entry(150, order), 20);
    EXPECT_EQ(latency.response(170, order), 30);
}

TEST(IntpOrderLatency, RejectionUsesNegativeNotificationLatency) {
    IntpOrderLatency latency({
        OrderLatencyRow{100, 0, 140, 0},
        OrderLatencyRow{200, 0, 260, 0},
    });
    Order<> order;

    EXPECT_EQ(latency.entry(150, order), -50);
}
