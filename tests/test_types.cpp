// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

// tests/test_types.cpp — Phase 1: types.hpp unit tests
// Ported from
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/types.rs
// (test_event_is)

#include "hbt/types.hpp"
#include <gtest/gtest.h>

using namespace hbt;

// ── sizeof / alignment ────────────────────────────────────────────────────────
TEST(EventLayout, Size64) { EXPECT_EQ(sizeof(Event), 64u); }
TEST(EventLayout, Align64) { EXPECT_EQ(alignof(Event), 64u); }

// ── event_is() truth table (mirrors test_event_is in types.rs) ───────────────
static Event make_ev(uint64_t ev_flags) {
    Event e{};
    e.ev = ev_flags;
    return e;
}

TEST(EventIs, LocalBidDepth) {
    auto ev = make_ev(LOCAL_BID_DEPTH_EVENT);
    EXPECT_TRUE(event_is(ev, LOCAL_BID_DEPTH_EVENT));
    EXPECT_FALSE(event_is(ev, LOCAL_ASK_DEPTH_EVENT));
    EXPECT_TRUE(event_is(ev, LOCAL_EVENT));
    EXPECT_TRUE(event_is(ev, BUY_EVENT));
    EXPECT_FALSE(event_is(ev, EXCH_EVENT));
}

TEST(EventIs, ExchSellTrade) {
    auto ev = make_ev(EXCH_SELL_TRADE_EVENT);
    EXPECT_TRUE(event_is(ev, EXCH_SELL_TRADE_EVENT));
    EXPECT_FALSE(event_is(ev, EXCH_BUY_TRADE_EVENT));
    EXPECT_TRUE(event_is(ev, EXCH_EVENT));
    EXPECT_TRUE(event_is(ev, TRADE_EVENT));
    EXPECT_TRUE(event_is(ev, SELL_EVENT));
    EXPECT_FALSE(event_is(ev, LOCAL_EVENT));
}

// Kind mask: LOCAL_BID_DEPTH_EVENT has low-8 = DEPTH_EVENT=1
// so it should NOT match TRADE_EVENT (low-8 = 2)
TEST(EventIs, KindMismatch) {
    auto ev = make_ev(LOCAL_BID_DEPTH_EVENT); // low-8 = 1
    EXPECT_FALSE(event_is(ev, LOCAL_TRADE_EVENT));
}

// Upper-flag-only query (kind=0): just check flags
TEST(EventIs, FlagOnlyQuery) {
    auto ev = make_ev(LOCAL_BID_DEPTH_EVENT);
    EXPECT_TRUE(event_is(ev, LOCAL_EVENT | BUY_EVENT)); // kind=0, both flags set
    EXPECT_FALSE(event_is(ev, EXCH_EVENT | BUY_EVENT)); // EXCH not set
}

// ── Order methods ─────────────────────────────────────────────────────────────
TEST(OrderMethods, PriceAndExecPrice) {
    Order<> o;
    o.price_tick = 500;
    o.tick_size = 0.1;
    o.exec_price_tick = 499;
    EXPECT_DOUBLE_EQ(o.price(), 50.0);
    EXPECT_DOUBLE_EQ(o.exec_price(), 49.9);
}

TEST(OrderMethods, CancellableAndActive) {
    Order<> o;
    o.status = Status::New;
    o.req = Status::None;
    EXPECT_TRUE(o.cancellable());
    EXPECT_TRUE(o.active());

    o.req = Status::Canceled;
    EXPECT_FALSE(o.cancellable());

    o.status = Status::Filled;
    EXPECT_FALSE(o.active());
}

// ── StateValues trivially copyable ───────────────────────────────────────────
TEST(StateValues, TrivialCopy) {
    static_assert(std::is_trivially_copyable_v<StateValues>);
    SUCCEED();
}
