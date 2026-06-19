// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/backtest/order_bus.hpp — OrderBus, ExchToLocal<LM>, LocalToExch<LM>
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/backtest/order.rs
//
// In Rust, ExchToLocal and LocalToExch share two OrderBus queues via
// Rc<UnsafeCell<VecDeque>>.  In C++ we share via two shared_ptr<deque> that
// both halves hold — one for to_exch, one for to_local.

#include <cassert>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "hbt/models/latency.hpp"
#include "hbt/types.hpp"

namespace hbt {

// ── OrderBus ──────────────────────────────────────────────────────────────────
// A single direction FIFO queue of (Order<Q>, scheduled_ts).
// Monotonic timestamp enforcement mirrors the Rust implementation.

template <typename Q = std::monostate> class OrderBus {
  public:
    using Entry = std::pair<Order<Q>, int64_t>;

    OrderBus() : q_(std::make_shared<std::deque<Entry>>()) {}

    /// Append an order scheduled at `timestamp` (enforces monotonicity).
    void append(Order<Q> order, int64_t timestamp) {
        int64_t latest = q_->empty() ? 0 : q_->back().second;
        q_->emplace_back(std::move(order), std::max(timestamp, latest));
    }

    [[nodiscard]] std::optional<int64_t> earliest_timestamp() const noexcept {
        if (q_->empty())
            return std::nullopt;
        return q_->front().second;
    }

    [[nodiscard]] std::optional<Order<Q>> pop_front() {
        if (q_->empty())
            return std::nullopt;
        auto entry = std::move(q_->front());
        q_->pop_front();
        return std::move(entry.first);
    }

    void reset() { q_->clear(); }

    [[nodiscard]] bool empty() const noexcept { return q_->empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return q_->size(); }

    // Shared ownership — ExchToLocal and LocalToExch share the underlying deque.
    [[nodiscard]] std::shared_ptr<std::deque<Entry>> shared() const { return q_; }

    // Construct from an existing shared deque (so both sides share the same object).
    explicit OrderBus(std::shared_ptr<std::deque<Entry>> shared) : q_(std::move(shared)) {}

  private:
    std::shared_ptr<std::deque<Entry>> q_;
};

// ── ExchToLocal<LM> ──────────────────────────────────────────────────────────
// Exchange side: receives order requests from local via to_exch bus,
// sends processed order responses back to local via to_local bus.

template <typename LM, typename Q = std::monostate> class ExchToLocal {
  public:
    ExchToLocal(OrderBus<Q> to_exch, OrderBus<Q> to_local, LM latency_model)
        : to_exch_(std::move(to_exch)), to_local_(std::move(to_local)),
          lm_(std::move(latency_model)) {}

    /// Timestamp of the earliest pending request from local → exchange.
    [[nodiscard]] std::optional<int64_t> earliest_recv_order_timestamp() const noexcept {
        return to_exch_.earliest_timestamp();
    }

    /// Timestamp of the earliest pending response from exchange → local.
    [[nodiscard]] std::optional<int64_t> earliest_send_order_timestamp() const noexcept {
        return to_local_.earliest_timestamp();
    }

    /// Respond to local: append processed order at (exch_ts + response_latency).
    void respond(Order<Q> order) {
        const int64_t resp_lat = lm_.response(order.exch_timestamp, order);
        const int64_t recv_ts = order.exch_timestamp + resp_lat; // save before move
        to_local_.append(std::move(order), recv_ts);
    }

    /// Receive the next order request scheduled at exactly `receipt_timestamp`.
    [[nodiscard]] std::optional<Order<Q>> receive(int64_t receipt_timestamp) {
        auto front_ts = to_exch_.earliest_timestamp();
        if (!front_ts)
            return std::nullopt;
        assert(*front_ts >= receipt_timestamp);
        if (*front_ts == receipt_timestamp)
            return to_exch_.pop_front();
        return std::nullopt;
    }

    LM &latency_model() noexcept { return lm_; }

  private:
    OrderBus<Q> to_exch_;
    OrderBus<Q> to_local_;
    LM lm_;
};

// ── LocalToExch<LM> ──────────────────────────────────────────────────────────
// Local side: submits order requests to exchange via to_exch bus;
// receives processed responses from exchange via to_local bus.

template <typename LM, typename Q = std::monostate> class LocalToExch {
  public:
    LocalToExch(OrderBus<Q> to_exch, OrderBus<Q> to_local, LM latency_model)
        : to_exch_(std::move(to_exch)), to_local_(std::move(to_local)),
          lm_(std::move(latency_model)) {}

    /// Timestamp of the earliest pending response coming back from exchange.
    [[nodiscard]] std::optional<int64_t> earliest_recv_order_timestamp() const noexcept {
        return to_local_.earliest_timestamp();
    }

    /// Timestamp of the earliest pending request going to exchange.
    [[nodiscard]] std::optional<int64_t> earliest_send_order_timestamp() const noexcept {
        return to_exch_.earliest_timestamp();
    }

    /// Submit an order request.
    /// If entry_latency < 0, the order is immediately rejected and a rejection
    /// response is queued at (local_ts - entry_latency) on the to_local bus.
    /// The `reject` callable mutates the order before it is put on the reject bus.
    template <typename RejectFn> void request(Order<Q> order, RejectFn &&reject) {
        const int64_t lat = lm_.entry(order.local_timestamp, order);
        if (lat < 0) {
            // Negative: rejection — abs(lat) is the local→local rejection latency.
            reject(order);
            const int64_t recv_ts = order.local_timestamp - lat;
            to_local_.append(std::move(order), recv_ts);
        } else {
            const int64_t recv_ts = order.local_timestamp + lat;
            to_exch_.append(std::move(order), recv_ts);
        }
    }

    /// Receive the next response from exchange scheduled at exactly `receipt_timestamp`.
    [[nodiscard]] std::optional<Order<Q>> receive(int64_t receipt_timestamp) {
        auto front_ts = to_local_.earliest_timestamp();
        if (!front_ts)
            return std::nullopt;
        assert(*front_ts >= receipt_timestamp);
        if (*front_ts == receipt_timestamp)
            return to_local_.pop_front();
        return std::nullopt;
    }

    LM &latency_model() noexcept { return lm_; }

  private:
    OrderBus<Q> to_exch_;
    OrderBus<Q> to_local_;
    LM lm_;
};

// ── Factory: create a matched ExchToLocal / LocalToExch pair ─────────────────
// Both sides share the same underlying to_exch and to_local deques.

template <typename LM, typename Q = std::monostate>
std::pair<ExchToLocal<LM, Q>, LocalToExch<LM, Q>> make_order_buses(LM latency_model) {
    // Two shared deques: one for local→exch direction, one for exch→local.
    using Entry = std::pair<Order<Q>, int64_t>;
    auto to_exch_deque = std::make_shared<std::deque<Entry>>();
    auto to_local_deque = std::make_shared<std::deque<Entry>>();

    OrderBus<Q> to_exch_a(to_exch_deque);
    OrderBus<Q> to_exch_b(to_exch_deque);
    OrderBus<Q> to_local_a(to_local_deque);
    OrderBus<Q> to_local_b(to_local_deque);

    return {
        ExchToLocal<LM, Q>(std::move(to_exch_a), std::move(to_local_a), latency_model),
        LocalToExch<LM, Q>(std::move(to_exch_b), std::move(to_local_b), std::move(latency_model))};
}

} // namespace hbt
