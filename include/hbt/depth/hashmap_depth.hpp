// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/depth/hashmap_depth.hpp — HashMapMarketDepth (L2)
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/depth/hashmapmarketdepth.rs
//
// Uses two unordered_map<int64_t,double> for bid/ask sides and explicitly
// maintains best_bid_tick / best_ask_tick + low/high watermarks.  This is
// more robust than a tree-based approach under dropped feeds: the watermarks
// allow the "scan for next best" fallback to be bounded.

#include <algorithm>
#include <cmath>
#include <span>
#include <unordered_map>
#include <vector>

#include "hbt/depth/market_depth.hpp"
#include "hbt/types.hpp"

namespace hbt {

// ── Internal helpers ──────────────────────────────────────────────────────────
namespace detail {

/// Scan downward from start (exclusive) to end (inclusive) for the highest
/// price level with qty > 0.  Returns INVALID_MIN if none found.
inline int64_t depth_below(const std::unordered_map<int64_t, double> &depth, int64_t start,
                           int64_t end) noexcept {
    for (int64_t t = start - 1; t >= end; --t) {
        auto it = depth.find(t);
        if (it != depth.end() && it->second > 0.0)
            return t;
    }
    return INVALID_MIN;
}

/// Scan upward from start+1 to end (inclusive) for the lowest price level
/// with qty > 0.  Returns INVALID_MAX if none found.
inline int64_t depth_above(const std::unordered_map<int64_t, double> &depth, int64_t start,
                           int64_t end) noexcept {
    for (int64_t t = start + 1; t <= end; ++t) {
        auto it = depth.find(t);
        if (it != depth.end() && it->second > 0.0)
            return t;
    }
    return INVALID_MAX;
}

} // namespace detail

// ── HashMapMarketDepth ────────────────────────────────────────────────────────
class HashMapMarketDepth : public ApplySnapshotMixin<HashMapMarketDepth> {
  public:
    explicit HashMapMarketDepth(double tick_size, double lot_size)
        : tick_size_(tick_size), lot_size_(lot_size) {}

    // ── MarketDepth interface ─────────────────────────────────────────────────
    [[nodiscard]] double best_bid() const noexcept {
        return best_bid_tick_ == INVALID_MIN ? std::numeric_limits<double>::quiet_NaN()
                                             : static_cast<double>(best_bid_tick_) * tick_size_;
    }
    [[nodiscard]] double best_ask() const noexcept {
        return best_ask_tick_ == INVALID_MAX ? std::numeric_limits<double>::quiet_NaN()
                                             : static_cast<double>(best_ask_tick_) * tick_size_;
    }
    [[nodiscard]] int64_t best_bid_tick() const noexcept { return best_bid_tick_; }
    [[nodiscard]] int64_t best_ask_tick() const noexcept { return best_ask_tick_; }
    [[nodiscard]] double tick_size() const noexcept { return tick_size_; }
    [[nodiscard]] double lot_size() const noexcept { return lot_size_; }

    [[nodiscard]] double best_bid_qty() const noexcept {
        auto it = bid_depth_.find(best_bid_tick_);
        return it != bid_depth_.end() ? it->second : 0.0;
    }
    [[nodiscard]] double best_ask_qty() const noexcept {
        auto it = ask_depth_.find(best_ask_tick_);
        return it != ask_depth_.end() ? it->second : 0.0;
    }
    [[nodiscard]] double bid_qty_at_tick(int64_t tick) const noexcept {
        auto it = bid_depth_.find(tick);
        return it != bid_depth_.end() ? it->second : 0.0;
    }
    [[nodiscard]] double ask_qty_at_tick(int64_t tick) const noexcept {
        auto it = ask_depth_.find(tick);
        return it != ask_depth_.end() ? it->second : 0.0;
    }

    // ── L2MarketDepth interface ───────────────────────────────────────────────
    DepthUpdate update_bid_depth(double price, double qty, int64_t timestamp) {
        const int64_t price_tick = static_cast<int64_t>(std::round(price / tick_size_));
        const int64_t qty_lot = static_cast<int64_t>(std::round(qty / lot_size_));
        const int64_t prev_best = best_bid_tick_;
        double prev_qty = 0.0;

        auto it = bid_depth_.find(price_tick);
        if (it != bid_depth_.end()) {
            prev_qty = it->second;
            if (qty_lot > 0)
                it->second = qty;
            else
                bid_depth_.erase(it);
        } else {
            if (qty_lot > 0)
                bid_depth_.emplace(price_tick, qty);
        }

        if (qty_lot == 0) {
            if (price_tick == best_bid_tick_) {
                best_bid_tick_ = detail::depth_below(bid_depth_, best_bid_tick_, low_bid_tick_);
                if (best_bid_tick_ == INVALID_MIN)
                    low_bid_tick_ = INVALID_MAX;
            }
        } else {
            if (price_tick > best_bid_tick_) {
                best_bid_tick_ = price_tick;
                if (best_bid_tick_ >= best_ask_tick_) {
                    best_ask_tick_ =
                        detail::depth_above(ask_depth_, best_bid_tick_, high_ask_tick_);
                }
            }
            low_bid_tick_ = std::min(low_bid_tick_, price_tick);
        }
        return {price_tick, prev_best, best_bid_tick_, prev_qty, qty, timestamp};
    }

    DepthUpdate update_ask_depth(double price, double qty, int64_t timestamp) {
        const int64_t price_tick = static_cast<int64_t>(std::round(price / tick_size_));
        const int64_t qty_lot = static_cast<int64_t>(std::round(qty / lot_size_));
        const int64_t prev_best = best_ask_tick_;
        double prev_qty = 0.0;

        auto it = ask_depth_.find(price_tick);
        if (it != ask_depth_.end()) {
            prev_qty = it->second;
            if (qty_lot > 0)
                it->second = qty;
            else
                ask_depth_.erase(it);
        } else {
            if (qty_lot > 0)
                ask_depth_.emplace(price_tick, qty);
        }

        if (qty_lot == 0) {
            if (price_tick == best_ask_tick_) {
                best_ask_tick_ = detail::depth_above(ask_depth_, best_ask_tick_, high_ask_tick_);
                if (best_ask_tick_ == INVALID_MAX)
                    high_ask_tick_ = INVALID_MIN;
            }
        } else {
            if (price_tick < best_ask_tick_) {
                best_ask_tick_ = price_tick;
                if (best_bid_tick_ >= best_ask_tick_) {
                    best_bid_tick_ = detail::depth_below(bid_depth_, best_ask_tick_, low_bid_tick_);
                }
            }
            high_ask_tick_ = std::max(high_ask_tick_, price_tick);
        }
        return {price_tick, prev_best, best_ask_tick_, prev_qty, qty, timestamp};
    }

    void clear_depth(Side side, double clear_upto_price) {
        switch (side) {
        case Side::Buy: {
            if (std::isfinite(clear_upto_price)) {
                const int64_t cut = static_cast<int64_t>(std::round(clear_upto_price / tick_size_));
                if (best_bid_tick_ != INVALID_MIN) {
                    for (int64_t t = cut; t <= best_bid_tick_; ++t)
                        bid_depth_.erase(t);
                }
                best_bid_tick_ = detail::depth_below(bid_depth_, cut, low_bid_tick_);
            } else {
                bid_depth_.clear();
                best_bid_tick_ = INVALID_MIN;
            }
            if (best_bid_tick_ == INVALID_MIN)
                low_bid_tick_ = INVALID_MAX;
            break;
        }
        case Side::Sell: {
            if (std::isfinite(clear_upto_price)) {
                const int64_t cut = static_cast<int64_t>(std::round(clear_upto_price / tick_size_));
                if (best_ask_tick_ != INVALID_MAX) {
                    for (int64_t t = best_ask_tick_; t <= cut; ++t)
                        ask_depth_.erase(t);
                }
                best_ask_tick_ = detail::depth_above(ask_depth_, cut, high_ask_tick_);
            } else {
                ask_depth_.clear();
                best_ask_tick_ = INVALID_MAX;
            }
            if (best_ask_tick_ == INVALID_MAX)
                high_ask_tick_ = INVALID_MIN;
            break;
        }
        case Side::None:
        default:
            bid_depth_.clear();
            ask_depth_.clear();
            best_bid_tick_ = INVALID_MIN;
            best_ask_tick_ = INVALID_MAX;
            low_bid_tick_ = INVALID_MAX;
            high_ask_tick_ = INVALID_MIN;
            break;
        }
    }

    // ── ApplySnapshot support ─────────────────────────────────────────────────
    void do_apply_snapshot(std::span<const Event> data) {
        best_bid_tick_ = INVALID_MIN;
        best_ask_tick_ = INVALID_MAX;
        low_bid_tick_ = INVALID_MAX;
        high_ask_tick_ = INVALID_MIN;
        bid_depth_.clear();
        ask_depth_.clear();

        for (const auto &ev : data) {
            const int64_t tick = static_cast<int64_t>(std::round(ev.px / tick_size_));
            if (ev.ev & BUY_EVENT) {
                best_bid_tick_ = std::max(best_bid_tick_, tick);
                low_bid_tick_ = std::min(low_bid_tick_, tick);
                bid_depth_[tick] = ev.qty;
            } else if (ev.ev & SELL_EVENT) {
                best_ask_tick_ = std::min(best_ask_tick_, tick);
                high_ask_tick_ = std::max(high_ask_tick_, tick);
                ask_depth_[tick] = ev.qty;
            }
        }
    }

    [[nodiscard]] std::vector<Event> do_snapshot() const {
        std::vector<Event> events;
        events.reserve(bid_depth_.size() + ask_depth_.size());

        // Bid side — sorted descending
        std::vector<std::pair<int64_t, double>> bids;
        bids.reserve(bid_depth_.size());
        for (auto &[tick, qty] : bid_depth_)
            if (tick <= best_bid_tick_)
                bids.emplace_back(tick, qty);
        std::sort(bids.begin(), bids.end(), [](auto &a, auto &b) { return a.first > b.first; });
        for (auto &[tick, qty] : bids) {
            events.push_back(Event{EXCH_EVENT | LOCAL_EVENT | BUY_EVENT | DEPTH_SNAPSHOT_EVENT, 0,
                                   0, static_cast<double>(tick) * tick_size_, qty, 0, 0, 0.0});
        }

        // Ask side — sorted ascending
        std::vector<std::pair<int64_t, double>> asks;
        asks.reserve(ask_depth_.size());
        for (auto &[tick, qty] : ask_depth_)
            if (tick >= best_ask_tick_)
                asks.emplace_back(tick, qty);
        std::sort(asks.begin(), asks.end(), [](auto &a, auto &b) { return a.first < b.first; });
        for (auto &[tick, qty] : asks) {
            events.push_back(Event{EXCH_EVENT | LOCAL_EVENT | SELL_EVENT | DEPTH_SNAPSHOT_EVENT, 0,
                                   0, static_cast<double>(tick) * tick_size_, qty, 0, 0, 0.0});
        }

        return events;
    }

    // ── Accessors for direct inspection (tests / processors) ─────────────────
    [[nodiscard]] const std::unordered_map<int64_t, double> &bid_depth_map() const noexcept {
        return bid_depth_;
    }
    [[nodiscard]] const std::unordered_map<int64_t, double> &ask_depth_map() const noexcept {
        return ask_depth_;
    }
    [[nodiscard]] int64_t low_bid_tick() const noexcept { return low_bid_tick_; }
    [[nodiscard]] int64_t high_ask_tick() const noexcept { return high_ask_tick_; }
    [[nodiscard]] int64_t timestamp() const noexcept { return timestamp_; }

  private:
    double tick_size_;
    double lot_size_;
    int64_t timestamp_ = 0;
    std::unordered_map<int64_t, double> bid_depth_;
    std::unordered_map<int64_t, double> ask_depth_;
    int64_t best_bid_tick_ = INVALID_MIN;
    int64_t best_ask_tick_ = INVALID_MAX;
    int64_t low_bid_tick_ = INVALID_MAX;
    int64_t high_ask_tick_ = INVALID_MIN;
};

static_assert(MarketDepthC<HashMapMarketDepth>);
static_assert(L2MarketDepthC<HashMapMarketDepth>);

} // namespace hbt
