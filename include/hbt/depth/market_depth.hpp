// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/depth/market_depth.hpp — MarketDepth concept + shared helpers
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/depth/mod.rs

#include <concepts>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "hbt/types.hpp"

namespace hbt {

// ── Sentinel tick values ──────────────────────────────────────────────────────
inline constexpr int64_t INVALID_MIN = std::numeric_limits<int64_t>::min(); ///< no best bid
inline constexpr int64_t INVALID_MAX = std::numeric_limits<int64_t>::max(); ///< no best ask

// ── MarketDepth concept ───────────────────────────────────────────────────────
// Any type satisfying this concept can be used as the MD template parameter
// in processors, backtest engine, and Bot.
template <typename MD>
concept MarketDepthC = requires(const MD &md, int64_t tick) {
    { md.best_bid() } -> std::convertible_to<double>;
    { md.best_ask() } -> std::convertible_to<double>;
    { md.best_bid_tick() } -> std::convertible_to<int64_t>;
    { md.best_ask_tick() } -> std::convertible_to<int64_t>;
    { md.best_bid_qty() } -> std::convertible_to<double>;
    { md.best_ask_qty() } -> std::convertible_to<double>;
    { md.tick_size() } -> std::convertible_to<double>;
    { md.lot_size() } -> std::convertible_to<double>;
    { md.bid_qty_at_tick(tick) } -> std::convertible_to<double>;
    { md.ask_qty_at_tick(tick) } -> std::convertible_to<double>;
};

// ── L2MarketDepth concept ─────────────────────────────────────────────────────
// Adds mutable L2 depth update / clear operations.
// Returns a 6-tuple: (price_tick, prev_best_tick, new_best_tick, prev_qty, new_qty, timestamp)
struct DepthUpdate {
    int64_t price_tick;
    int64_t prev_best_tick;
    int64_t new_best_tick;
    double prev_qty;
    double new_qty;
    int64_t timestamp;
};

template <typename MD>
concept L2MarketDepthC =
    MarketDepthC<MD> && requires(MD &md, double p, double q, int64_t ts, Side s) {
        { md.update_bid_depth(p, q, ts) } -> std::same_as<DepthUpdate>;
        { md.update_ask_depth(p, q, ts) } -> std::same_as<DepthUpdate>;
        { md.clear_depth(s, p) } -> std::same_as<void>;
    };

// ── ApplySnapshot mixin ───────────────────────────────────────────────────────
// Concrete depth types inherit this or implement the two methods inline.
template <typename Derived> struct ApplySnapshotMixin {
    void apply_snapshot(std::span<const Event> data) {
        static_cast<Derived *>(this)->do_apply_snapshot(data);
    }
    [[nodiscard]] std::vector<Event> snapshot() const {
        return static_cast<const Derived *>(this)->do_snapshot();
    }
};

} // namespace hbt
