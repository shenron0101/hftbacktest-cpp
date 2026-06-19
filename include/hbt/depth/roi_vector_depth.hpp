// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "hbt/depth/market_depth.hpp"
#include "hbt/types.hpp"

namespace hbt {

class ROIVectorMarketDepth : public ApplySnapshotMixin<ROIVectorMarketDepth> {
  public:
    ROIVectorMarketDepth(double tick_size, double lot_size, double roi_lower_bound,
                         double roi_upper_bound)
        : ROIVectorMarketDepth(tick_size, lot_size,
                               make_range(tick_size, lot_size, roi_lower_bound, roi_upper_bound)) {}

  private:
    struct Range {
        int64_t lower;
        int64_t upper;
        std::size_t size;
    };

    ROIVectorMarketDepth(double tick_size, double lot_size, const Range &range)
        : tick_size_(tick_size), lot_size_(lot_size), roi_lb_(range.lower), roi_ub_(range.upper),
          bid_depth_(range.size, 0.0), ask_depth_(range.size, 0.0) {}

    static Range make_range(double tick_size, double lot_size, double roi_lower_bound,
                            double roi_upper_bound) {
        if (!std::isfinite(tick_size) || !std::isfinite(lot_size) ||
            !std::isfinite(roi_lower_bound) || !std::isfinite(roi_upper_bound) ||
            tick_size <= 0.0 || lot_size <= 0.0) {
            throw std::invalid_argument("invalid ROI vector market depth configuration");
        }

        const long double lower_ticks =
            std::round(static_cast<long double>(roi_lower_bound) / tick_size);
        const long double upper_ticks =
            std::round(static_cast<long double>(roi_upper_bound) / tick_size);
        constexpr long double min_tick =
            static_cast<long double>(std::numeric_limits<int64_t>::min());
        constexpr long double max_tick =
            static_cast<long double>(std::numeric_limits<int64_t>::max());
        if (lower_ticks < min_tick || lower_ticks > max_tick || upper_ticks < min_tick ||
            upper_ticks > max_tick || lower_ticks > upper_ticks) {
            throw std::invalid_argument("invalid ROI vector market depth configuration");
        }

        const auto lower = static_cast<int64_t>(lower_ticks);
        const auto upper = static_cast<int64_t>(upper_ticks);
        const long double size = upper_ticks - lower_ticks + 1.0L;
        if (size > static_cast<long double>(std::numeric_limits<std::size_t>::max())) {
            throw std::invalid_argument("ROI vector market depth range is too large");
        }
        return {lower, upper, static_cast<std::size_t>(size)};
    }

  public:
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

    [[nodiscard]] double best_bid_qty() const noexcept { return bid_qty_at_tick(best_bid_tick_); }
    [[nodiscard]] double best_ask_qty() const noexcept { return ask_qty_at_tick(best_ask_tick_); }
    [[nodiscard]] double bid_qty_at_tick(int64_t tick) const noexcept {
        if (!in_roi(tick))
            return std::numeric_limits<double>::quiet_NaN();
        return bid_depth_[index(tick)];
    }
    [[nodiscard]] double ask_qty_at_tick(int64_t tick) const noexcept {
        if (!in_roi(tick))
            return std::numeric_limits<double>::quiet_NaN();
        return ask_depth_[index(tick)];
    }

    DepthUpdate update_bid_depth(double price, double qty, int64_t timestamp) {
        const int64_t tick = static_cast<int64_t>(std::round(price / tick_size_));
        const int64_t previous_best = best_bid_tick_;
        if (!in_roi(tick)) {
            return {tick, previous_best, best_bid_tick_, 0.0, qty, timestamp};
        }
        const double previous_qty = bid_depth_[index(tick)];
        bid_depth_[index(tick)] = qty;
        if (static_cast<int64_t>(std::round(qty / lot_size_)) == 0) {
            if (tick == best_bid_tick_)
                best_bid_tick_ = scan_bid_below(tick);
        } else {
            if (tick > best_bid_tick_) {
                best_bid_tick_ = tick;
                if (best_bid_tick_ >= best_ask_tick_) {
                    best_ask_tick_ = scan_ask_above(best_bid_tick_);
                }
            }
            low_bid_tick_ = std::min(low_bid_tick_, tick);
        }
        if (best_bid_tick_ == INVALID_MIN)
            low_bid_tick_ = INVALID_MAX;
        return {tick, previous_best, best_bid_tick_, previous_qty, qty, timestamp};
    }

    DepthUpdate update_ask_depth(double price, double qty, int64_t timestamp) {
        const int64_t tick = static_cast<int64_t>(std::round(price / tick_size_));
        const int64_t previous_best = best_ask_tick_;
        if (!in_roi(tick)) {
            return {tick, previous_best, best_ask_tick_, 0.0, qty, timestamp};
        }
        const double previous_qty = ask_depth_[index(tick)];
        ask_depth_[index(tick)] = qty;
        if (static_cast<int64_t>(std::round(qty / lot_size_)) == 0) {
            if (tick == best_ask_tick_)
                best_ask_tick_ = scan_ask_above(tick);
        } else {
            if (tick < best_ask_tick_) {
                best_ask_tick_ = tick;
                if (best_bid_tick_ >= best_ask_tick_) {
                    best_bid_tick_ = scan_bid_below(best_ask_tick_);
                }
            }
            high_ask_tick_ = std::max(high_ask_tick_, tick);
        }
        if (best_ask_tick_ == INVALID_MAX)
            high_ask_tick_ = INVALID_MIN;
        return {tick, previous_best, best_ask_tick_, previous_qty, qty, timestamp};
    }

    void clear_depth(Side side, double clear_upto_price) {
        if (side == Side::None || side == Side::Unsupported) {
            std::fill(bid_depth_.begin(), bid_depth_.end(), 0.0);
            std::fill(ask_depth_.begin(), ask_depth_.end(), 0.0);
            reset_bounds();
            return;
        }
        const bool finite = std::isfinite(clear_upto_price);
        const int64_t clear_tick =
            finite ? static_cast<int64_t>(std::round(clear_upto_price / tick_size_)) : 0;
        if (side == Side::Buy) {
            for (int64_t tick = roi_lb_; tick <= roi_ub_; ++tick) {
                if (!finite || tick >= clear_tick)
                    bid_depth_[index(tick)] = 0.0;
            }
            recompute_bids();
        } else {
            for (int64_t tick = roi_lb_; tick <= roi_ub_; ++tick) {
                if (!finite || tick <= clear_tick)
                    ask_depth_[index(tick)] = 0.0;
            }
            recompute_asks();
        }
    }

    void do_apply_snapshot(std::span<const Event> events) {
        clear_depth(Side::None, 0.0);
        for (const auto &event : events) {
            if (event.ev & BUY_EVENT) {
                update_bid_depth(event.px, event.qty, event.exch_ts);
            } else if (event.ev & SELL_EVENT) {
                update_ask_depth(event.px, event.qty, event.exch_ts);
            }
        }
    }

    [[nodiscard]] std::vector<Event> do_snapshot() const {
        std::vector<Event> events;
        events.reserve(bid_depth_.size() + ask_depth_.size());
        for (int64_t tick = roi_ub_; tick >= roi_lb_; --tick) {
            const double qty = bid_depth_[index(tick)];
            if (qty > 0.0) {
                events.push_back(Event{LOCAL_EVENT | EXCH_EVENT | BUY_EVENT | DEPTH_SNAPSHOT_EVENT,
                                       0, 0, static_cast<double>(tick) * tick_size_, qty, 0, 0,
                                       0.0});
            }
        }
        for (int64_t tick = roi_lb_; tick <= roi_ub_; ++tick) {
            const double qty = ask_depth_[index(tick)];
            if (qty > 0.0) {
                events.push_back(Event{LOCAL_EVENT | EXCH_EVENT | SELL_EVENT | DEPTH_SNAPSHOT_EVENT,
                                       0, 0, static_cast<double>(tick) * tick_size_, qty, 0, 0,
                                       0.0});
            }
        }
        return events;
    }

    [[nodiscard]] const std::vector<double> &bid_depth() const noexcept { return bid_depth_; }
    [[nodiscard]] const std::vector<double> &ask_depth() const noexcept { return ask_depth_; }
    [[nodiscard]] std::pair<int64_t, int64_t> roi_tick() const noexcept {
        return {roi_lb_, roi_ub_};
    }

  private:
    [[nodiscard]] bool in_roi(int64_t tick) const noexcept {
        return tick >= roi_lb_ && tick <= roi_ub_;
    }
    [[nodiscard]] std::size_t index(int64_t tick) const noexcept {
        return static_cast<std::size_t>(tick - roi_lb_);
    }
    [[nodiscard]] int64_t scan_bid_below(int64_t start) const noexcept {
        for (int64_t tick = std::min(start - 1, roi_ub_); tick >= roi_lb_; --tick) {
            if (bid_depth_[index(tick)] > 0.0)
                return tick;
        }
        return INVALID_MIN;
    }
    [[nodiscard]] int64_t scan_ask_above(int64_t start) const noexcept {
        for (int64_t tick = std::max(start + 1, roi_lb_); tick <= roi_ub_; ++tick) {
            if (ask_depth_[index(tick)] > 0.0)
                return tick;
        }
        return INVALID_MAX;
    }
    void recompute_bids() noexcept {
        best_bid_tick_ = INVALID_MIN;
        low_bid_tick_ = INVALID_MAX;
        for (int64_t tick = roi_lb_; tick <= roi_ub_; ++tick) {
            if (bid_depth_[index(tick)] > 0.0) {
                best_bid_tick_ = tick;
                low_bid_tick_ = std::min(low_bid_tick_, tick);
            }
        }
    }
    void recompute_asks() noexcept {
        best_ask_tick_ = INVALID_MAX;
        high_ask_tick_ = INVALID_MIN;
        for (int64_t tick = roi_lb_; tick <= roi_ub_; ++tick) {
            if (ask_depth_[index(tick)] > 0.0) {
                best_ask_tick_ = std::min(best_ask_tick_, tick);
                high_ask_tick_ = tick;
            }
        }
    }
    void reset_bounds() noexcept {
        best_bid_tick_ = INVALID_MIN;
        best_ask_tick_ = INVALID_MAX;
        low_bid_tick_ = INVALID_MAX;
        high_ask_tick_ = INVALID_MIN;
    }

    double tick_size_;
    double lot_size_;
    int64_t roi_lb_;
    int64_t roi_ub_;
    std::vector<double> bid_depth_;
    std::vector<double> ask_depth_;
    int64_t best_bid_tick_ = INVALID_MIN;
    int64_t best_ask_tick_ = INVALID_MAX;
    int64_t low_bid_tick_ = INVALID_MAX;
    int64_t high_ask_tick_ = INVALID_MIN;
};

static_assert(MarketDepthC<ROIVectorMarketDepth>);
static_assert(L2MarketDepthC<ROIVectorMarketDepth>);

} // namespace hbt
