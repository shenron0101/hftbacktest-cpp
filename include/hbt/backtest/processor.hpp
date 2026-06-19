// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/backtest/processor.hpp — Abstract Processor base for type-erasure boundary
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/backtest/proc/mod.rs
//
// The multi-asset Backtest<MD> engine owns vectors of LocalProcessorBase and
// ExchProcessorBase pointers.  All hot-path virtual dispatch is limited to
// these two interfaces; the templated concrete types (Local<...>, Exchange<...>)
// live behind them.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "hbt/types.hpp"

namespace hbt {

// ── Processor (exchange-side interface) ───────────────────────────────────────
class Processor {
  public:
    virtual ~Processor() = default;

    /// Returns the timestamp at which this processor will see the given event,
    /// or nullopt if the event is not meant for this side.
    [[nodiscard]] virtual std::optional<int64_t>
    event_seen_timestamp(const Event &ev) const noexcept = 0;

    /// Process one feed event (depth or trade).
    virtual void process(const Event &ev) = 0;

    /// Process any pending order responses/requests scheduled at `timestamp`.
    /// Returns true if the order matching `wait_resp_order_id` was received.
    virtual bool process_recv_order(int64_t timestamp,
                                    std::optional<OrderId> wait_resp_order_id) = 0;

    /// Earliest timestamp of an order response waiting to be delivered to local.
    [[nodiscard]] virtual int64_t earliest_recv_order_timestamp() const noexcept = 0;

    /// Earliest timestamp of an order request waiting to be delivered to exchange.
    [[nodiscard]] virtual int64_t earliest_send_order_timestamp() const noexcept = 0;
};

// ── LocalProcessor (local-side interface) ─────────────────────────────────────
// Adds the Bot-facing accessors + submit/cancel/modify.
class LocalProcessor : public Processor {
  public:
    // ── Strategy API ─────────────────────────────────────────────────────────
    virtual void submit_order(OrderId order_id, Side side, double price, double qty,
                              OrdType order_type, TimeInForce tif, int64_t current_ts) = 0;

    virtual void modify(OrderId order_id, double price, double qty, int64_t current_ts) = 0;

    virtual void cancel(OrderId order_id, int64_t current_ts) = 0;

    virtual void clear_inactive_orders() = 0;

    // ── Read-only observers ───────────────────────────────────────────────────
    [[nodiscard]] virtual double position() const noexcept = 0;
    [[nodiscard]] virtual const StateValues &state_values() const noexcept = 0;

    /// Returns a read-only map of order_id → Order<> (queue scratch stripped).
    [[nodiscard]] virtual const std::unordered_map<OrderId, Order<>> &orders() const noexcept = 0;

    /// Last market trades captured during the current step.
    [[nodiscard]] virtual const std::vector<Event> &last_trades() const noexcept = 0;
    virtual void clear_last_trades() noexcept = 0;

    /// (exch_ts, local_ts) of the last feed event received.
    [[nodiscard]] virtual std::optional<std::pair<int64_t, int64_t>>
    feed_latency() const noexcept = 0;
    /// (req_ts, exch_ts, resp_ts) of the last order round-trip.
    [[nodiscard]] virtual std::optional<std::tuple<int64_t, int64_t, int64_t>>
    order_latency() const noexcept = 0;

    // ── Depth accessors ───────────────────────────────────────────────────────
    [[nodiscard]] virtual double best_bid() const noexcept = 0;
    [[nodiscard]] virtual double best_ask() const noexcept = 0;
    [[nodiscard]] virtual int64_t best_bid_tick() const noexcept = 0;
    [[nodiscard]] virtual int64_t best_ask_tick() const noexcept = 0;
    [[nodiscard]] virtual double tick_size() const noexcept = 0;
    [[nodiscard]] virtual double lot_size() const noexcept = 0;
};

} // namespace hbt
