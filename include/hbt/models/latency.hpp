// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/models/latency.hpp — Latency models (ConstantLatency, IntpOrderLatency)
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/backtest/models/latency.rs
//
// Includes constant and historical interpolation latency models.

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <stdexcept>
#include <variant>
#include <vector>

#include "hbt/types.hpp"

namespace hbt {

// ── LatencyModel concept ──────────────────────────────────────────────────────
// Latency models are mutable (they advance internal position through latency data).
template <typename LM, typename Q = std::monostate>
concept LatencyModelC = requires(LM &lm, int64_t ts, const Order<Q> &order) {
    { lm.entry(ts, order) } -> std::convertible_to<int64_t>;
    { lm.response(ts, order) } -> std::convertible_to<int64_t>;
};

// ── ConstantLatency ───────────────────────────────────────────────────────────
// Negative entry_latency → the exchange rejects the order; the absolute value
// is the rejection notification latency from the exchange to local.
struct ConstantLatency {
    int64_t entry_latency = 0;
    int64_t response_latency = 0;

    constexpr ConstantLatency() = default;
    constexpr ConstantLatency(int64_t entry, int64_t response)
        : entry_latency(entry), response_latency(response) {}

    template <typename Q>
    [[nodiscard]] constexpr int64_t entry(int64_t /*ts*/,
                                          const Order<Q> & /*order*/) const noexcept {
        return entry_latency;
    }

    template <typename Q>
    [[nodiscard]] constexpr int64_t response(int64_t /*ts*/,
                                             const Order<Q> & /*order*/) const noexcept {
        return response_latency;
    }
};

// ── OrderLatencyRow ───────────────────────────────────────────────────────────
// 32-byte aligned POD matching the numpy dtype used by hftbacktest's latency files.
struct alignas(32) OrderLatencyRow {
    int64_t req_ts = 0;  ///< local timestamp of request
    int64_t exch_ts = 0; ///< exchange processing timestamp (0 = rejection)
    int64_t resp_ts = 0; ///< local timestamp of response receipt
    int64_t _padding = 0;
};
static_assert(sizeof(OrderLatencyRow) == 32);
static_assert(std::is_trivially_copyable_v<OrderLatencyRow>);

class IntpOrderLatency {
  public:
    explicit IntpOrderLatency(std::vector<OrderLatencyRow> rows, int64_t latency_offset = 0)
        : rows_(std::move(rows)) {
        if (rows_.empty()) {
            throw std::invalid_argument("interpolated latency requires rows");
        }
        for (auto &row : rows_) {
            row.req_ts += latency_offset;
            if (row.exch_ts > 0)
                row.exch_ts += latency_offset;
            row.resp_ts += latency_offset;
        }
        if (!std::is_sorted(rows_.begin(), rows_.end(), [](const auto &lhs, const auto &rhs) {
                return lhs.req_ts < rhs.req_ts;
            })) {
            throw std::invalid_argument("latency rows must be ordered by request timestamp");
        }
    }

    template <typename Q> [[nodiscard]] int64_t entry(int64_t timestamp, const Order<Q> &) const {
        if (timestamp <= rows_.front().req_ts)
            return entry_latency(rows_.front());
        const auto [first, second] = request_bracket(timestamp);
        if (first == second)
            return entry_latency(*first);
        if (first->exch_ts <= 0 || second->exch_ts <= 0) {
            return -interpolate(timestamp, first->req_ts, first->resp_ts - first->req_ts,
                                second->req_ts, second->resp_ts - second->req_ts);
        }
        return interpolate(timestamp, first->req_ts, first->exch_ts - first->req_ts, second->req_ts,
                           second->exch_ts - second->req_ts);
    }

    template <typename Q>
    [[nodiscard]] int64_t response(int64_t timestamp, const Order<Q> &) const {
        const auto positive = [](const OrderLatencyRow &row) { return row.exch_ts > 0; };
        auto first_valid = std::find_if(rows_.begin(), rows_.end(), positive);
        if (first_valid == rows_.end())
            return 0;
        if (timestamp <= first_valid->exch_ts) {
            return first_valid->resp_ts - first_valid->exch_ts;
        }
        auto previous = first_valid;
        for (auto current = std::next(first_valid); current != rows_.end(); ++current) {
            if (!positive(*current))
                continue;
            if (timestamp < current->exch_ts) {
                return interpolate(timestamp, previous->exch_ts,
                                   previous->resp_ts - previous->exch_ts, current->exch_ts,
                                   current->resp_ts - current->exch_ts);
            }
            previous = current;
        }
        return previous->resp_ts - previous->exch_ts;
    }

  private:
    using Iterator = std::vector<OrderLatencyRow>::const_iterator;

    [[nodiscard]] static int64_t interpolate(int64_t x, int64_t x1, int64_t y1, int64_t x2,
                                             int64_t y2) {
        if (x1 == x2)
            return y1;
        return static_cast<int64_t>(static_cast<double>(y2 - y1) / static_cast<double>(x2 - x1) *
                                    static_cast<double>(x - x1)) +
               y1;
    }

    [[nodiscard]] static int64_t entry_latency(const OrderLatencyRow &row) {
        return row.exch_ts <= 0 ? -(row.resp_ts - row.req_ts) : row.exch_ts - row.req_ts;
    }

    [[nodiscard]] std::pair<Iterator, Iterator> request_bracket(int64_t timestamp) const {
        auto second = std::upper_bound(
            rows_.begin(), rows_.end(), timestamp,
            [](int64_t value, const OrderLatencyRow &row) { return value < row.req_ts; });
        if (second == rows_.end()) {
            auto last = std::prev(rows_.end());
            return {last, last};
        }
        return {std::prev(second), second};
    }

    std::vector<OrderLatencyRow> rows_;
};

static_assert(LatencyModelC<IntpOrderLatency>);

} // namespace hbt
