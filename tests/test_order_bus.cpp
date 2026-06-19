// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

// tests/test_order_bus.cpp — Phase 5: OrderBus / LocalToExch / ExchToLocal tests

#include "hbt/backtest/order_bus.hpp"
#include "hbt/backtest/state.hpp"
#include "hbt/models/asset_type.hpp"
#include "hbt/models/fee.hpp"
#include "hbt/models/latency.hpp"
#include "hbt/types.hpp"
#include <gtest/gtest.h>

using namespace hbt;

// ── OrderBus basic operations ─────────────────────────────────────────────────
TEST(OrderBus, AppendAndPopFront) {
    OrderBus<> bus;
    EXPECT_TRUE(bus.empty());
    EXPECT_FALSE(bus.earliest_timestamp().has_value());

    Order<> o;
    o.order_id = 1;
    bus.append(o, 100);
    EXPECT_FALSE(bus.empty());
    ASSERT_EQ(bus.earliest_timestamp(), 100);

    auto popped = bus.pop_front();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(popped->order_id, 1u);
    EXPECT_TRUE(bus.empty());
}

TEST(OrderBus, MonotonicTimestamp) {
    // Appending with a timestamp earlier than the back → clamps to back.
    OrderBus<> bus;
    Order<> o;
    o.order_id = 1;
    bus.append(o, 200);
    o.order_id = 2;
    bus.append(o, 50); // earlier — should be clamped to 200

    (void)bus.pop_front();                    // pop oid=1
    ASSERT_EQ(bus.earliest_timestamp(), 200); // oid=2 was scheduled at 200
}

TEST(OrderBus, Reset) {
    OrderBus<> bus;
    Order<> o;
    o.order_id = 1;
    bus.append(o, 100);
    bus.reset();
    EXPECT_TRUE(bus.empty());
}

// ── make_order_buses: shared underlying deques ────────────────────────────────
TEST(OrderBusPair, SharedDeques) {
    // make_order_buses creates a linked pair: exch's to_exch == local's to_exch.
    auto [e2l, l2e] = make_order_buses<ConstantLatency>(ConstantLatency{100, 200});

    // submit from local side → appears on exch side
    Order<> o;
    o.order_id = 42;
    o.local_timestamp = 1000;
    l2e.request(o, [](Order<> &req) { req.req = Status::Rejected; });

    ASSERT_EQ(l2e.earliest_send_order_timestamp(), 1100); // 1000 + 100
    auto recv = e2l.receive(1100);
    ASSERT_TRUE(recv.has_value());
    EXPECT_EQ(recv->order_id, 42u);
}

// ── LocalToExch: negative latency → rejection path ───────────────────────────
TEST(LocalToExch, NegativeLatencyReject) {
    // entry_latency = -50: order is rejected, notified at local_ts + 50.
    auto [e2l, l2e] = make_order_buses<ConstantLatency>(ConstantLatency{-50, 200});

    Order<> o;
    o.order_id = 7;
    o.local_timestamp = 500;
    bool reject_called = false;
    l2e.request(o, [&reject_called](Order<> &req) {
        req.req = Status::Rejected;
        reject_called = true;
    });

    EXPECT_TRUE(reject_called);
    // Rejection goes straight onto to_local (not to_exch)
    EXPECT_FALSE(l2e.earliest_send_order_timestamp().has_value()); // nothing going to exch
    ASSERT_EQ(l2e.earliest_recv_order_timestamp(), 550);           // 500 + 50
    auto recv = l2e.receive(550);
    ASSERT_TRUE(recv.has_value());
    EXPECT_EQ(recv->req, Status::Rejected);
}

// ── ExchToLocal: respond path ─────────────────────────────────────────────────
TEST(ExchToLocal, RespondAndReceive) {
    auto [e2l, l2e] = make_order_buses<ConstantLatency>(ConstantLatency{100, 200});

    // Manually push an order onto to_exch so exch side can receive it
    Order<> o;
    o.order_id = 99;
    o.local_timestamp = 1000;
    l2e.request(o, [](Order<> &req) { req.req = Status::Rejected; });

    auto from_local = e2l.receive(1100);
    ASSERT_TRUE(from_local.has_value());
    from_local->exch_timestamp = 1100;
    from_local->status = Status::New;

    e2l.respond(std::move(*from_local));
    // Response should be at exch_ts + response_latency = 1100 + 200 = 1300
    ASSERT_EQ(e2l.earliest_send_order_timestamp(), 1300);
    ASSERT_EQ(l2e.earliest_recv_order_timestamp(), 1300);

    auto resp = l2e.receive(1300);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->order_id, 99u);
    EXPECT_EQ(resp->status, Status::New);
}

// ── State: apply_fill accumulates correctly ───────────────────────────────────
TEST(State, ApplyFillLinear) {
    using S = State<LinearAsset, TradingValueFeeModel<CommonFees>>;
    S state(LinearAsset{1.0},
            TradingValueFeeModel<CommonFees>{CommonFees{0.0, 0.001}}); // 0.1% taker, 0% maker

    Order<> o;
    o.side = Side::Buy;
    o.exec_qty = 2.0;
    o.exec_price_tick = 1000;
    o.tick_size = 0.1;
    o.maker = false;

    state.apply_fill(o);
    const auto &sv = state.values();
    EXPECT_DOUBLE_EQ(sv.position, 2.0);
    EXPECT_DOUBLE_EQ(sv.balance, -200.0); // -amount * sign = -(100 * 2) * 1
    EXPECT_DOUBLE_EQ(sv.num_trades, 1.0);
    EXPECT_DOUBLE_EQ(sv.trading_volume, 2.0);
    EXPECT_DOUBLE_EQ(sv.trading_value, 200.0);
    // fee = 200 * 0.001 = 0.2
    EXPECT_DOUBLE_EQ(sv.fee, 0.2);
}
