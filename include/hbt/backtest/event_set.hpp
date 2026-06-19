// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/backtest/event_set.hpp — EventSet: flat timestamp scheduler
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/backtest/evs.rs
//
// The EventSet stores 4 × num_assets timestamps in a flat cache-aligned array.
// Index layout per asset:
//   4*i+0 = LocalData    (next feed event timestamp seen by local)
//   4*i+1 = LocalOrder   (next order response to be received by local)
//   4*i+2 = ExchData     (next feed event to be processed by exchange)
//   4*i+3 = ExchOrder    (next order to be processed by exchange)
//
// next() scans the flat array and returns the minimum non-MAX entry.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace hbt {

// ── EventIntentKind ───────────────────────────────────────────────────────────
enum class EventIntentKind : std::size_t {
    LocalData = 0,
    LocalOrder = 1,
    ExchData = 2,
    ExchOrder = 3,
};

// ── EventIntent ───────────────────────────────────────────────────────────────
struct EventIntent {
    int64_t timestamp;
    std::size_t asset_no;
    EventIntentKind kind;
};

// ── EventSet ──────────────────────────────────────────────────────────────────
class EventSet {
  public:
    explicit EventSet(std::size_t num_assets)
        : num_assets_(num_assets), ts_(num_assets * 4, std::numeric_limits<int64_t>::max()) {
        assert(num_assets > 0);
    }

    /// Return the EventIntent with the smallest timestamp, or nullopt if all MAX.
    [[nodiscard]] std::optional<EventIntent> next() const noexcept {
        std::size_t best_idx = 0;
        int64_t best_ts = ts_[0];
        for (std::size_t i = 1; i < ts_.size(); ++i) {
            if (ts_[i] < best_ts) {
                best_ts = ts_[i];
                best_idx = i;
            }
        }
        if (best_ts == std::numeric_limits<int64_t>::max())
            return std::nullopt;
        return EventIntent{best_ts, best_idx >> 2, static_cast<EventIntentKind>(best_idx & 3)};
    }

    void update_local_data(std::size_t asset, int64_t ts) noexcept { ts_[4 * asset + 0] = ts; }
    void update_local_order(std::size_t asset, int64_t ts) noexcept { ts_[4 * asset + 1] = ts; }
    void update_exch_data(std::size_t asset, int64_t ts) noexcept { ts_[4 * asset + 2] = ts; }
    void update_exch_order(std::size_t asset, int64_t ts) noexcept { ts_[4 * asset + 3] = ts; }

    void invalidate_local_data(std::size_t asset) noexcept {
        ts_[4 * asset + 0] = std::numeric_limits<int64_t>::max();
    }
    void invalidate_local_order(std::size_t asset) noexcept {
        ts_[4 * asset + 1] = std::numeric_limits<int64_t>::max();
    }
    void invalidate_exch_data(std::size_t asset) noexcept {
        ts_[4 * asset + 2] = std::numeric_limits<int64_t>::max();
    }
    void invalidate_exch_order(std::size_t asset) noexcept {
        ts_[4 * asset + 3] = std::numeric_limits<int64_t>::max();
    }

    [[nodiscard]] int64_t local_data_ts(std::size_t asset) const noexcept {
        return ts_[4 * asset + 0];
    }
    [[nodiscard]] int64_t local_order_ts(std::size_t asset) const noexcept {
        return ts_[4 * asset + 1];
    }
    [[nodiscard]] int64_t exch_data_ts(std::size_t asset) const noexcept {
        return ts_[4 * asset + 2];
    }
    [[nodiscard]] int64_t exch_order_ts(std::size_t asset) const noexcept {
        return ts_[4 * asset + 3];
    }

    [[nodiscard]] std::size_t num_assets() const noexcept { return num_assets_; }

  private:
    std::size_t num_assets_;
    std::vector<int64_t> ts_; ///< 4 × num_assets flat array
};

} // namespace hbt
