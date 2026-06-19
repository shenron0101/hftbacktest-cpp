// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

// tests/test_depth.cpp — Phase 2: depth unit tests

#include "hbt/depth/hashmap_depth.hpp"
#include "hbt/depth/roi_vector_depth.hpp"
#include <cmath>
#include <gtest/gtest.h>

using namespace hbt;

// ── Basic bid/ask update ──────────────────────────────────────────────────────
TEST(HashMapDepth, BidUpdate) {
    HashMapMarketDepth d(0.1, 0.001);
    auto r = d.update_bid_depth(100.0, 5.0, 0);
    EXPECT_EQ(r.price_tick, 1000);
    EXPECT_EQ(r.new_best_tick, 1000);
    EXPECT_DOUBLE_EQ(d.best_bid(), 100.0);
    EXPECT_EQ(d.best_bid_tick(), 1000);
    EXPECT_DOUBLE_EQ(d.bid_qty_at_tick(1000), 5.0);
}

TEST(HashMapDepth, AskUpdate) {
    HashMapMarketDepth d(0.1, 0.001);
    auto r = d.update_ask_depth(101.0, 3.0, 0);
    EXPECT_EQ(r.new_best_tick, 1010);
    EXPECT_DOUBLE_EQ(d.best_ask(), 101.0);
}

// ── Remove level drops best ───────────────────────────────────────────────────
TEST(HashMapDepth, RemoveBestBid) {
    HashMapMarketDepth d(0.1, 0.001);
    d.update_bid_depth(100.0, 5.0, 0);
    d.update_bid_depth(99.0, 2.0, 0);
    d.update_bid_depth(100.0, 0.0, 0); // remove 100
    EXPECT_EQ(d.best_bid_tick(), 990);
    EXPECT_DOUBLE_EQ(d.best_bid(), 99.0);
}

// ── clear_depth(Side::None) wipes everything ─────────────────────────────────
TEST(HashMapDepth, ClearAll) {
    HashMapMarketDepth d(0.1, 0.001);
    d.update_bid_depth(100.0, 5.0, 0);
    d.update_ask_depth(101.0, 3.0, 0);
    d.clear_depth(Side::None, 0.0);
    EXPECT_EQ(d.best_bid_tick(), INVALID_MIN);
    EXPECT_EQ(d.best_ask_tick(), INVALID_MAX);
    EXPECT_TRUE(std::isnan(d.best_bid()));
    EXPECT_TRUE(std::isnan(d.best_ask()));
}

// ── Snapshot round-trip ───────────────────────────────────────────────────────
TEST(HashMapDepth, SnapshotRoundTrip) {
    HashMapMarketDepth d(0.1, 0.001);
    d.update_bid_depth(100.0, 5.0, 0);
    d.update_bid_depth(99.5, 2.0, 0);
    d.update_ask_depth(100.5, 3.0, 0);
    d.update_ask_depth(101.0, 4.0, 0);

    auto snap = d.do_snapshot();
    EXPECT_FALSE(snap.empty());

    HashMapMarketDepth d2(0.1, 0.001);
    d2.do_apply_snapshot(snap);
    EXPECT_EQ(d2.best_bid_tick(), d.best_bid_tick());
    EXPECT_EQ(d2.best_ask_tick(), d.best_ask_tick());
    EXPECT_DOUBLE_EQ(d2.bid_qty_at_tick(d.best_bid_tick()), d.bid_qty_at_tick(d.best_bid_tick()));
}

TEST(ROIVectorDepth, UpdatesDeletesAndIgnoresOutsideRange) {
    ROIVectorMarketDepth depth(0.5, 0.1, 99.0, 102.0);
    depth.update_bid_depth(100.0, 2.0, 1);
    depth.update_ask_depth(101.0, 3.0, 2);
    depth.update_bid_depth(98.0, 9.0, 3);

    EXPECT_DOUBLE_EQ(depth.best_bid(), 100.0);
    EXPECT_DOUBLE_EQ(depth.best_ask(), 101.0);
    EXPECT_DOUBLE_EQ(depth.bid_qty_at_tick(200), 2.0);
    EXPECT_TRUE(std::isnan(depth.bid_qty_at_tick(196)));

    depth.update_bid_depth(100.0, 0.0, 4);
    EXPECT_TRUE(std::isnan(depth.best_bid()));
}

TEST(ROIVectorDepth, ClearAllResetsBothSides) {
    ROIVectorMarketDepth depth(1.0, 1.0, 90.0, 110.0);
    depth.update_bid_depth(99.0, 1.0, 1);
    depth.update_ask_depth(101.0, 1.0, 1);

    depth.clear_depth(Side::None, 0.0);

    EXPECT_TRUE(std::isnan(depth.best_bid()));
    EXPECT_TRUE(std::isnan(depth.best_ask()));
}

TEST(ROIVectorDepth, RejectsInvalidConfigurationBeforeAllocating) {
    EXPECT_THROW(ROIVectorMarketDepth(0.0, 1.0, 90.0, 110.0), std::invalid_argument);
    EXPECT_THROW(ROIVectorMarketDepth(1.0, 1.0, 110.0, 90.0), std::invalid_argument);
}
