// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/backtest/local.hpp — Local<AT,LM,MD,FM> processor
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/backtest/proc/local.rs

#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "hbt/backtest/order_bus.hpp"
#include "hbt/backtest/processor.hpp"
#include "hbt/backtest/state.hpp"
#include "hbt/depth/market_depth.hpp"
#include "hbt/models/asset_type.hpp"
#include "hbt/models/fee.hpp"
#include "hbt/models/latency.hpp"
#include "hbt/types.hpp"

namespace hbt {

/// Local processor: the strategy-facing side of one asset in the backtest.
///
/// Template parameters:
///   AT  — asset type (LinearAsset / InverseAsset)
///   LM  — latency model (ConstantLatency / IntpOrderLatency)
///   MD  — market depth type satisfying L2MarketDepthC
///   FM  — fee model
///
/// The Local always uses Order<> (monostate Q) — it knows nothing about the
/// exchange's queue model.  The exchange initialises queue scratch on `ack_new`.
template <AssetTypeC AT, typename LM, L2MarketDepthC MD, typename FM>
class Local final : public LocalProcessor {
  public:
    Local(MD depth, State<AT, FM> state, std::size_t last_trades_cap, LocalToExch<LM> order_l2e)
        : depth_(std::move(depth)), state_(std::move(state)), order_l2e_(std::move(order_l2e)),
          last_trades_cap_(last_trades_cap) {}

    // ── LocalProcessor API ────────────────────────────────────────────────────
    void submit_order(OrderId order_id, Side side, double price, double qty, OrdType order_type,
                      TimeInForce tif, int64_t current_ts) override {
        if (orders_.count(order_id))
            throw std::runtime_error("submit_order: order_id already exists");

        const int64_t price_tick = static_cast<int64_t>(std::round(price / depth_.tick_size()));
        Order<> order(order_id, price_tick, depth_.tick_size(), qty, side, order_type, tif);
        order.req = Status::New;
        order.local_timestamp = current_ts;
        orders_.emplace(order_id, order);

        order_l2e_.request(order, [](Order<> &o) { o.req = Status::Rejected; });
    }

    void modify(OrderId order_id, double price, double qty, int64_t current_ts) override {
        auto it = orders_.find(order_id);
        if (it == orders_.end())
            throw std::runtime_error("modify: order not found");
        auto &order = it->second;
        if (order.req != Status::None)
            throw std::runtime_error("modify: order request in process");

        const int64_t orig_price_tick = order.price_tick;
        const double orig_qty = order.qty;

        order.price_tick = static_cast<int64_t>(std::round(price / depth_.tick_size()));
        order.qty = qty;
        order.req = Status::Replaced;
        order.local_timestamp = current_ts;

        order_l2e_.request(order, [orig_price_tick, orig_qty](Order<> &o) {
            o.req = Status::Rejected;
            o.price_tick = orig_price_tick;
            o.qty = orig_qty;
        });
    }

    void cancel(OrderId order_id, int64_t current_ts) override {
        auto it = orders_.find(order_id);
        if (it == orders_.end())
            throw std::runtime_error("cancel: order not found");
        auto &order = it->second;
        if (order.req != Status::None)
            throw std::runtime_error("cancel: order request in process");

        order.req = Status::Canceled;
        order.local_timestamp = current_ts;
        order_l2e_.request(order, [](Order<> &o) { o.req = Status::Rejected; });
    }

    void clear_inactive_orders() override {
        for (auto it = orders_.begin(); it != orders_.end();) {
            const auto &s = it->second.status;
            if (s == Status::Expired || s == Status::Filled || s == Status::Canceled)
                it = orders_.erase(it);
            else
                ++it;
        }
    }

    [[nodiscard]] double position() const noexcept override { return state_.values().position; }
    [[nodiscard]] const StateValues &state_values() const noexcept override {
        return state_.values();
    }
    [[nodiscard]] const std::unordered_map<OrderId, Order<>> &orders() const noexcept override {
        return orders_;
    }
    [[nodiscard]] const std::vector<Event> &last_trades() const noexcept override {
        return trades_;
    }
    void clear_last_trades() noexcept override { trades_.clear(); }

    [[nodiscard]] std::optional<std::pair<int64_t, int64_t>>
    feed_latency() const noexcept override {
        return last_feed_latency_;
    }

    [[nodiscard]] std::optional<std::tuple<int64_t, int64_t, int64_t>>
    order_latency() const noexcept override {
        return last_order_latency_;
    }

    // ── Processor API ─────────────────────────────────────────────────────────
    [[nodiscard]] std::optional<int64_t>
    event_seen_timestamp(const Event &ev) const noexcept override {
        return event_is(ev, LOCAL_EVENT) ? std::optional<int64_t>{ev.local_ts} : std::nullopt;
    }

    void process(const Event &ev) override {
        if (event_is(ev, LOCAL_BID_DEPTH_CLEAR_EVENT))
            depth_.clear_depth(Side::Buy, ev.px);
        else if (event_is(ev, LOCAL_ASK_DEPTH_CLEAR_EVENT))
            depth_.clear_depth(Side::Sell, ev.px);
        else if (event_is(ev, LOCAL_DEPTH_CLEAR_EVENT))
            depth_.clear_depth(Side::None, 0.0);
        else if (event_is(ev, LOCAL_BID_DEPTH_EVENT) ||
                 event_is(ev, LOCAL_BID_DEPTH_SNAPSHOT_EVENT))
            depth_.update_bid_depth(ev.px, ev.qty, ev.local_ts);
        else if (event_is(ev, LOCAL_ASK_DEPTH_EVENT) ||
                 event_is(ev, LOCAL_ASK_DEPTH_SNAPSHOT_EVENT))
            depth_.update_ask_depth(ev.px, ev.qty, ev.local_ts);
        else if (event_is(ev, LOCAL_TRADE_EVENT)) {
            if (last_trades_cap_ > 0 && trades_.size() < last_trades_cap_)
                trades_.push_back(ev);
        }
        last_feed_latency_ = {ev.exch_ts, ev.local_ts};
    }

    bool process_recv_order(int64_t timestamp, std::optional<OrderId> wait_id) override {
        bool got_wait = false;
        while (auto opt = order_l2e_.receive(timestamp)) {
            auto &order = *opt;

            if (order.exch_timestamp > 0) {
                last_order_latency_ = {order.local_timestamp, order.exch_timestamp, timestamp};
            }
            if (wait_id && order.order_id == *wait_id)
                got_wait = true;

            if (order.status == Status::Filled || order.status == Status::PartiallyFilled)
                state_.apply_fill(order);

            auto it = orders_.find(order.order_id);
            if (it != orders_.end()) {
                auto &local_order = it->second;
                if (order.req == Status::Rejected) {
                    if (order.local_timestamp == local_order.local_timestamp) {
                        if (local_order.req == Status::New) {
                            local_order.req = Status::None;
                            local_order.status = Status::Expired;
                        } else {
                            local_order.req = Status::None;
                        }
                    }
                } else {
                    local_order.update(order);
                }
            } else {
                if (order.req != Status::Rejected) {
                    orders_.emplace(order.order_id, order);
                }
            }
        }
        return got_wait;
    }

    [[nodiscard]] int64_t earliest_recv_order_timestamp() const noexcept override {
        return order_l2e_.earliest_recv_order_timestamp().value_or(
            std::numeric_limits<int64_t>::max());
    }
    [[nodiscard]] int64_t earliest_send_order_timestamp() const noexcept override {
        return order_l2e_.earliest_send_order_timestamp().value_or(
            std::numeric_limits<int64_t>::max());
    }

    // ── Depth accessors (LocalProcessor virtuals) ────────────────────────────
    [[nodiscard]] double best_bid() const noexcept override { return depth_.best_bid(); }
    [[nodiscard]] double best_ask() const noexcept override { return depth_.best_ask(); }
    [[nodiscard]] int64_t best_bid_tick() const noexcept override { return depth_.best_bid_tick(); }
    [[nodiscard]] int64_t best_ask_tick() const noexcept override { return depth_.best_ask_tick(); }
    [[nodiscard]] double tick_size() const noexcept override { return depth_.tick_size(); }
    [[nodiscard]] double lot_size() const noexcept override { return depth_.lot_size(); }

    // ── Direct depth access (for Backtest engine) ─────────────────────────────
    [[nodiscard]] MD &depth() noexcept { return depth_; }

  private:
    MD depth_;
    State<AT, FM> state_;
    LocalToExch<LM> order_l2e_;
    std::size_t last_trades_cap_;

    std::unordered_map<OrderId, Order<>> orders_;
    std::vector<Event> trades_;
    std::optional<std::pair<int64_t, int64_t>> last_feed_latency_;
    std::optional<std::tuple<int64_t, int64_t, int64_t>> last_order_latency_;
};

} // namespace hbt
