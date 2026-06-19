// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

// tests/test_backtest.cpp — Phase 7: Backtest engine end-to-end tests
// Single-asset run to end-of-data; EventSet multi-asset interleaving.

#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "hbt/backtest/backtest.hpp"
#include "hbt/backtest/event_set.hpp"
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

// ── Types ─────────────────────────────────────────────────────────────────────
using AT = LinearAsset;
using LM = ConstantLatency;
using QM = RiskAdverseQueueModel;
using MD = HashMapMarketDepth;
using FM = TradingValueFeeModel<CommonFees>;
using Q = QM::OrderState;
using LocalT = Local<AT, LM, MD, FM>;
using ExchT = NoPartialFillExchange<AT, LM, QM, MD, FM>;

// ── EventSet unit tests ───────────────────────────────────────────────────────
TEST(EventSet, NextReturnsMinimum) {
    EventSet evs(2);
    evs.update_local_data(0, 100);
    evs.update_local_data(1, 200);
    evs.update_exch_data(0, 50);
    evs.update_exch_data(1, 300);

    auto first = evs.next();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->timestamp, 50);
    EXPECT_EQ(first->kind, EventIntentKind::ExchData);
    EXPECT_EQ(first->asset_no, 0u);
}

TEST(EventSet, InvalidateRemovesSlot) {
    EventSet evs(1);
    evs.update_local_data(0, 100);
    evs.invalidate_local_data(0);
    auto opt = evs.next();
    EXPECT_FALSE(opt.has_value());
}

TEST(EventSet, AllSlots) {
    EventSet evs(1);
    evs.update_local_data(0, 400);
    evs.update_local_order(0, 300);
    evs.update_exch_data(0, 100);
    evs.update_exch_order(0, 200);

    auto e1 = evs.next();
    ASSERT_TRUE(e1);
    EXPECT_EQ(e1->timestamp, 100);
    evs.invalidate_exch_data(0);

    auto e2 = evs.next();
    ASSERT_TRUE(e2);
    EXPECT_EQ(e2->timestamp, 200);
    evs.invalidate_exch_order(0);

    auto e3 = evs.next();
    ASSERT_TRUE(e3);
    EXPECT_EQ(e3->timestamp, 300);
    evs.invalidate_local_order(0);

    auto e4 = evs.next();
    ASSERT_TRUE(e4);
    EXPECT_EQ(e4->timestamp, 400);
    evs.invalidate_local_data(0);

    EXPECT_FALSE(evs.next().has_value());
}

// ── Helpers ───────────────────────────────────────────────────────────────────
static std::pair<std::unique_ptr<LocalT>, std::unique_ptr<ExchT>>
make_processor_pair(int64_t entry_lat = 0, int64_t resp_lat = 0) {
    auto [e2l, l2e] = make_order_buses<LM>(ConstantLatency{entry_lat, resp_lat});
    auto local = std::make_unique<LocalT>(MD{0.1, 0.001},
                                          State<AT, FM>{LinearAsset{1.0}, FM{CommonFees{0.0, 0.0}}},
                                          8, std::move(l2e));
    auto exch = std::make_unique<ExchT>(
        MD{0.1, 0.001}, State<AT, FM, Q>{LinearAsset{1.0}, FM{CommonFees{0.0, 0.0}}}, QM{},
        std::move(e2l));
    return {std::move(local), std::move(exch)};
}

static Event make_local_bid(double px, double qty, int64_t ts) {
    Event e{};
    e.ev = LOCAL_BID_DEPTH_EVENT;
    e.px = px;
    e.qty = qty;
    e.exch_ts = ts - 10;
    e.local_ts = ts;
    return e;
}
static Event make_exch_bid(double px, double qty, int64_t ts) {
    Event e{};
    e.ev = EXCH_BID_DEPTH_EVENT;
    e.px = px;
    e.qty = qty;
    e.exch_ts = ts;
    e.local_ts = ts + 5;
    return e;
}

// ── Single-asset run to EndOfData ─────────────────────────────────────────────
TEST(Backtest, SingleAssetRunToEnd) {
    Backtest bt;
    auto [local, exch] = make_processor_pair();

    // 3 local feed events + matching exch events
    std::vector<Event> ldata = {
        make_local_bid(100.0, 5.0, 1000),
        make_local_bid(100.0, 4.0, 2000),
        make_local_bid(100.0, 3.0, 3000),
    };
    std::vector<Event> edata = {
        make_exch_bid(100.0, 5.0, 1000),
        make_exch_bid(100.0, 4.0, 2000),
        make_exch_bid(100.0, 3.0, 3000),
    };

    bt.add_asset(std::move(local), std::move(exch), {{ldata}}, {{edata}});

    auto result = bt.close();
    EXPECT_EQ(result, ElapseResult::EndOfData);
    EXPECT_EQ(bt.num_assets(), 1u);
}

TEST(Backtest, CurrentTimestampTracksLastProcessedEvent) {
    Backtest bt;
    auto [local, exch] = make_processor_pair();
    std::vector<Event> local_data = {
        make_local_bid(100.0, 1.0, 1'000),
        make_local_bid(100.0, 1.0, 3'000),
    };
    std::vector<Event> exchange_data = {
        make_exch_bid(100.0, 1.0, 1'000),
        make_exch_bid(100.0, 1.0, 3'000),
    };
    bt.add_asset(std::move(local), std::move(exch), {{std::move(local_data)}},
                 {{std::move(exchange_data)}});

    EXPECT_EQ(bt.close(), ElapseResult::EndOfData);
    EXPECT_EQ(bt.current_timestamp(), 3'000);
}

// ── elapse() stops at target timestamp ───────────────────────────────────────
TEST(Backtest, ElapseStopsAtTarget) {
    Backtest bt;
    auto [local, exch] = make_processor_pair();

    std::vector<Event> ldata = {
        make_local_bid(100.0, 5.0, 1000),
        make_local_bid(100.0, 4.0, 5000), // will not be processed yet
    };
    std::vector<Event> edata = {
        make_exch_bid(100.0, 5.0, 1000),
        make_exch_bid(100.0, 4.0, 5000),
    };

    bt.add_asset(std::move(local), std::move(exch), {{ldata}}, {{edata}});

    // Elapse 2000 ns — should stop before ts=5000 events
    auto result = bt.elapse(2000);
    EXPECT_EQ(result, ElapseResult::Ok);
    EXPECT_LE(bt.current_timestamp(), 3000);
}

// ── wait_next_feed() stops at next market data event ─────────────────────────
TEST(Backtest, WaitNextFeedStops) {
    Backtest bt;
    auto [local, exch] = make_processor_pair();

    std::vector<Event> ldata = {
        make_local_bid(100.0, 5.0, 1000),
        make_local_bid(100.0, 4.0, 2000),
    };
    std::vector<Event> edata = {
        make_exch_bid(100.0, 5.0, 1000),
        make_exch_bid(100.0, 4.0, 2000),
    };

    bt.add_asset(std::move(local), std::move(exch), {{ldata}}, {{edata}});

    auto r1 = bt.wait_next_feed(true, false);
    EXPECT_EQ(r1, ElapseResult::MarketFeed);

    auto r2 = bt.wait_next_feed(true, false);
    EXPECT_EQ(r2, ElapseResult::MarketFeed);

    auto r3 = bt.wait_next_feed(true, false);
    EXPECT_EQ(r3, ElapseResult::EndOfData);
}

// ── bt.mid() returns correct mid after depth events ───────────────────────────
TEST(Backtest, MidPriceFromDepth) {
    Backtest bt;
    auto [local, exch] = make_processor_pair();

    // Both LOCAL and EXCH flags set — same vector for both sides
    std::vector<Event> data;
    {
        Event e{};
        e.ev = LOCAL_BID_DEPTH_EVENT | EXCH_BID_DEPTH_EVENT;
        e.px = 100.0;
        e.qty = 5.0;
        e.exch_ts = 500;
        e.local_ts = 1000;
        data.push_back(e);
    }
    {
        Event e{};
        e.ev = LOCAL_ASK_DEPTH_EVENT | EXCH_ASK_DEPTH_EVENT;
        e.px = 100.4;
        e.qty = 5.0;
        e.exch_ts = 500;
        e.local_ts = 1000;
        data.push_back(e);
    }

    bt.add_asset(std::move(local), std::move(exch), {{data}}, {{data}});
    bt.close();

    EXPECT_DOUBLE_EQ(bt.best_bid(0), 100.0);
    EXPECT_DOUBLE_EQ(bt.best_ask(0), 100.4);
    EXPECT_DOUBLE_EQ(bt.mid(0), 100.2);
    EXPECT_DOUBLE_EQ(bt.tick_size(0), 0.1);
    EXPECT_DOUBLE_EQ(bt.lot_size(0), 0.001);
}

// ── Multi-asset: events from two assets interleave correctly ──────────────────
TEST(Backtest, MultiAssetInterleaving) {
    Backtest bt;

    // Asset 0: events at ts 100, 300
    auto [l0, e0] = make_processor_pair();
    std::vector<Event> ld0 = {make_local_bid(100.0, 1.0, 100), make_local_bid(100.0, 2.0, 300)};
    std::vector<Event> ed0 = {make_exch_bid(100.0, 1.0, 100), make_exch_bid(100.0, 2.0, 300)};

    // Asset 1: events at ts 200, 400
    auto [l1, e1] = make_processor_pair();
    std::vector<Event> ld1 = {make_local_bid(200.0, 1.0, 200), make_local_bid(200.0, 2.0, 400)};
    std::vector<Event> ed1 = {make_exch_bid(200.0, 1.0, 200), make_exch_bid(200.0, 2.0, 400)};

    bt.add_asset(std::move(l0), std::move(e0), {{ld0}}, {{ed0}});
    bt.add_asset(std::move(l1), std::move(e1), {{ld1}}, {{ed1}});

    EXPECT_EQ(bt.num_assets(), 2u);
    auto result = bt.close();
    EXPECT_EQ(result, ElapseResult::EndOfData);
}

static std::pair<double, double> run_with_chunks(std::vector<std::vector<Event>> chunks) {
    Backtest bt;
    auto [local, exch] = make_processor_pair();
    bt.add_asset(std::move(local), std::move(exch), chunks, chunks);
    EXPECT_EQ(bt.close(), ElapseResult::EndOfData);
    return {bt.best_bid(0), bt.best_ask(0)};
}

TEST(Backtest, ShardBoundariesDoNotChangeReplayResult) {
    Event bid1{};
    bid1.ev = LOCAL_EVENT | EXCH_EVENT | BUY_EVENT | DEPTH_EVENT;
    bid1.exch_ts = 100;
    bid1.local_ts = 100;
    bid1.px = 100.0;
    bid1.qty = 1.0;

    Event ask{};
    ask.ev = LOCAL_EVENT | EXCH_EVENT | SELL_EVENT | DEPTH_EVENT;
    ask.exch_ts = 200;
    ask.local_ts = 200;
    ask.px = 101.0;
    ask.qty = 2.0;

    Event bid2 = bid1;
    bid2.exch_ts = 300;
    bid2.local_ts = 300;
    bid2.qty = 3.0;

    const auto monolithic = run_with_chunks({{bid1, ask, bid2}});
    const auto sharded = run_with_chunks({{bid1}, {ask}, {bid2}});

    EXPECT_EQ(monolithic, sharded);
}
