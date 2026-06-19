// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/backtest/exchange.hpp — NoPartialFillExchange<AT,LM,QM,MD,FM>
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/backtest/proc/nopartialfillexchange.rs
//
// Fill conditions (from Rust doc comment):
//   Buy order in book:
//     - price >= best_ask  → taker fill
//     - price > sell trade → maker fill
//     - front of queue && price == sell trade → maker fill
//   Sell order in book (symmetric)

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hbt/backtest/order_bus.hpp"
#include "hbt/backtest/processor.hpp"
#include "hbt/backtest/state.hpp"
#include "hbt/depth/market_depth.hpp"
#include "hbt/models/asset_type.hpp"
#include "hbt/models/fee.hpp"
#include "hbt/models/latency.hpp"
#include "hbt/models/queue.hpp"
#include "hbt/types.hpp"

namespace hbt {

template <AssetTypeC AT, typename LM, typename QM, L2MarketDepthC MD, typename FM,
          bool PartialFill = false>
class NoPartialFillExchange final : public Processor {
    using Q = typename QM::OrderState;
    using Ord = Order<Q>;

  public:
    NoPartialFillExchange(MD depth, State<AT, FM, Q> state, QM queue_model,
                          ExchToLocal<LM> order_e2l)
        : depth_(std::move(depth)), state_(std::move(state)), qm_(std::move(queue_model)),
          order_e2l_(std::move(order_e2l)) {}

    // ── Processor API ─────────────────────────────────────────────────────────
    [[nodiscard]] std::optional<int64_t>
    event_seen_timestamp(const Event &ev) const noexcept override {
        return event_is(ev, EXCH_EVENT) ? std::optional<int64_t>{ev.exch_ts} : std::nullopt;
    }

    void process(const Event &ev) override {
        if (event_is(ev, EXCH_BID_DEPTH_CLEAR_EVENT))
            depth_.clear_depth(Side::Buy, ev.px);
        else if (event_is(ev, EXCH_ASK_DEPTH_CLEAR_EVENT))
            depth_.clear_depth(Side::Sell, ev.px);
        else if (event_is(ev, EXCH_DEPTH_CLEAR_EVENT))
            depth_.clear_depth(Side::None, 0.0);
        else if (event_is(ev, EXCH_BID_DEPTH_EVENT) ||
                 event_is(ev, EXCH_BID_DEPTH_SNAPSHOT_EVENT)) {
            auto du = depth_.update_bid_depth(ev.px, ev.qty, ev.exch_ts);
            on_bid_qty_chg(du.price_tick, du.prev_qty, du.new_qty);
            if (du.new_best_tick > du.prev_best_tick)
                on_best_bid_update(du.prev_best_tick, du.new_best_tick, du.timestamp);
        } else if (event_is(ev, EXCH_ASK_DEPTH_EVENT) ||
                   event_is(ev, EXCH_ASK_DEPTH_SNAPSHOT_EVENT)) {
            auto du = depth_.update_ask_depth(ev.px, ev.qty, ev.exch_ts);
            on_ask_qty_chg(du.price_tick, du.prev_qty, du.new_qty);
            if (du.new_best_tick < du.prev_best_tick)
                on_best_ask_update(du.prev_best_tick, du.new_best_tick, du.timestamp);
        } else if (event_is(ev, EXCH_BUY_TRADE_EVENT)) {
            const int64_t price_tick = static_cast<int64_t>(std::round(ev.px / depth_.tick_size()));
            const double qty = ev.qty;
            // Check sell orders that might be filled by this buy trade
            bool scan_all =
                (depth_.best_bid_tick() == INVALID_MIN) ||
                (static_cast<int64_t>(orders_.size()) < price_tick - depth_.best_bid_tick());
            if (scan_all) {
                for (auto &[oid, order] : orders_) {
                    if (order.side == Side::Sell)
                        check_if_sell_filled(order, price_tick, qty, ev.exch_ts);
                }
            } else {
                for (int64_t t = depth_.best_bid_tick() + 1; t <= price_tick; ++t) {
                    auto it = sell_orders_.find(t);
                    if (it == sell_orders_.end())
                        continue;
                    for (OrderId oid : std::vector<OrderId>(it->second.begin(), it->second.end())) {
                        auto &order = orders_.at(oid);
                        check_if_sell_filled(order, price_tick, qty, ev.exch_ts);
                    }
                }
            }
            remove_filled_orders();
        } else if (event_is(ev, EXCH_SELL_TRADE_EVENT)) {
            const int64_t price_tick = static_cast<int64_t>(std::round(ev.px / depth_.tick_size()));
            const double qty = ev.qty;
            bool scan_all =
                (depth_.best_ask_tick() == INVALID_MAX) ||
                (static_cast<int64_t>(orders_.size()) < depth_.best_ask_tick() - price_tick);
            if (scan_all) {
                for (auto &[oid, order] : orders_) {
                    if (order.side == Side::Buy)
                        check_if_buy_filled(order, price_tick, qty, ev.exch_ts);
                }
            } else {
                for (int64_t t = depth_.best_ask_tick() - 1; t >= price_tick; --t) {
                    auto it = buy_orders_.find(t);
                    if (it == buy_orders_.end())
                        continue;
                    for (OrderId oid : std::vector<OrderId>(it->second.begin(), it->second.end())) {
                        auto &order = orders_.at(oid);
                        check_if_buy_filled(order, price_tick, qty, ev.exch_ts);
                    }
                }
            }
            remove_filled_orders();
        }
    }

    bool process_recv_order(int64_t timestamp, std::optional<OrderId> /*wait_id*/) override {
        while (auto opt = order_e2l_.receive(timestamp)) {
            Order<> plain = std::move(*opt); // bus carries Order<> (monostate)
            Ord order(plain);                // promote to Order<Q> for queue model
            if (order.req == Status::New)
                ack_new(order, timestamp);
            else if (order.req == Status::Canceled)
                ack_cancel(order, timestamp);
            else if (order.req == Status::Replaced)
                ack_modify(order, timestamp);
            order_e2l_.respond(order.to_plain()); // respond with Order<>
        }
        return false; // exchange side never waits for a specific order
    }

    [[nodiscard]] int64_t earliest_recv_order_timestamp() const noexcept override {
        return order_e2l_.earliest_recv_order_timestamp().value_or(
            std::numeric_limits<int64_t>::max());
    }
    [[nodiscard]] int64_t earliest_send_order_timestamp() const noexcept override {
        return order_e2l_.earliest_send_order_timestamp().value_or(
            std::numeric_limits<int64_t>::max());
    }

    [[nodiscard]] MD &depth() noexcept { return depth_; }

  private:
    // ── Internal helpers ─────────────────────────────────────────────────────

    void check_if_sell_filled(Ord &order, int64_t price_tick, double qty, int64_t ts) {
        if (order.price_tick < price_tick) {
            filled_orders_.push_back(order.order_id);
            fill_maker(order, ts, order.price_tick);
        } else if (order.price_tick == price_tick) {
            qm_.trade(order, qty, depth_);
            const double executable = qm_.is_filled(order, depth_);
            if (executable > 0.0) {
                if constexpr (PartialFill) {
                    const double exec_qty = std::min(executable, order.leaves_qty);
                    fill<true>(order, ts, true, order.price_tick, exec_qty);
                    if (order.status == Status::Filled)
                        filled_orders_.push_back(order.order_id);
                } else {
                    filled_orders_.push_back(order.order_id);
                    fill_maker(order, ts, order.price_tick);
                }
            }
        }
    }

    void check_if_buy_filled(Ord &order, int64_t price_tick, double qty, int64_t ts) {
        if (order.price_tick > price_tick) {
            filled_orders_.push_back(order.order_id);
            fill_maker(order, ts, order.price_tick);
        } else if (order.price_tick == price_tick) {
            qm_.trade(order, qty, depth_);
            const double executable = qm_.is_filled(order, depth_);
            if (executable > 0.0) {
                if constexpr (PartialFill) {
                    const double exec_qty = std::min(executable, order.leaves_qty);
                    fill<true>(order, ts, true, order.price_tick, exec_qty);
                    if (order.status == Status::Filled)
                        filled_orders_.push_back(order.order_id);
                } else {
                    filled_orders_.push_back(order.order_id);
                    fill_maker(order, ts, order.price_tick);
                }
            }
        }
    }

    void fill_maker(Ord &order, int64_t ts, int64_t exec_price_tick) {
        fill<true>(order, ts,
                   /*maker=*/true, exec_price_tick, order.leaves_qty);
    }

    template <bool MakeResponse>
    void fill(Ord &order, int64_t ts, bool maker, int64_t exec_price_tick, double exec_qty) {
        if (order.status == Status::Expired || order.status == Status::Canceled ||
            order.status == Status::Filled)
            return;

        order.maker = maker;
        order.exec_price_tick = maker ? order.price_tick : exec_price_tick;
        order.exec_qty = exec_qty;
        order.leaves_qty = std::max(0.0, order.leaves_qty - exec_qty);
        order.status = static_cast<int64_t>(std::round(order.leaves_qty / depth_.lot_size())) > 0
                           ? Status::PartiallyFilled
                           : Status::Filled;
        order.exch_timestamp = ts;

        state_.apply_fill(order);

        if constexpr (MakeResponse) {
            order_e2l_.respond(order.to_plain()); // bus carries Order<>
        }
    }

    void remove_filled_orders() {
        for (OrderId oid : filled_orders_) {
            auto it = orders_.find(oid);
            if (it == orders_.end())
                continue;
            if (it->second.side == Side::Buy) {
                auto &s = buy_orders_[it->second.price_tick];
                s.erase(oid);
            } else {
                auto &s = sell_orders_[it->second.price_tick];
                s.erase(oid);
            }
            orders_.erase(it);
        }
        filled_orders_.clear();
    }

    void on_bid_qty_chg(int64_t price_tick, double prev_qty, double new_qty) {
        auto it = buy_orders_.find(price_tick);
        if (it == buy_orders_.end())
            return;
        for (OrderId oid : it->second) {
            auto &order = orders_.at(oid);
            qm_.depth(order, prev_qty, new_qty, depth_);
        }
    }

    void on_ask_qty_chg(int64_t price_tick, double prev_qty, double new_qty) {
        auto it = sell_orders_.find(price_tick);
        if (it == sell_orders_.end())
            return;
        for (OrderId oid : it->second) {
            auto &order = orders_.at(oid);
            qm_.depth(order, prev_qty, new_qty, depth_);
        }
    }

    void on_best_bid_update(int64_t prev_best, int64_t new_best, int64_t ts) {
        bool scan_all = (prev_best == INVALID_MIN) ||
                        (static_cast<int64_t>(orders_.size()) < new_best - prev_best);
        std::vector<OrderId> to_fill;
        if (scan_all) {
            for (auto &[oid, order] : orders_) {
                if (order.side == Side::Sell && order.price_tick <= new_best)
                    to_fill.push_back(oid);
            }
        } else {
            for (int64_t t = prev_best + 1; t <= new_best; ++t) {
                auto it = sell_orders_.find(t);
                if (it != sell_orders_.end())
                    to_fill.insert(to_fill.end(), it->second.begin(), it->second.end());
            }
        }
        for (OrderId oid : to_fill) {
            filled_orders_.push_back(oid);
            fill_maker(orders_.at(oid), ts, orders_.at(oid).price_tick);
        }
        remove_filled_orders();
    }

    void on_best_ask_update(int64_t prev_best, int64_t new_best, int64_t ts) {
        bool scan_all = (prev_best == INVALID_MAX) ||
                        (static_cast<int64_t>(orders_.size()) < prev_best - new_best);
        std::vector<OrderId> to_fill;
        if (scan_all) {
            for (auto &[oid, order] : orders_) {
                if (order.side == Side::Buy && order.price_tick >= new_best)
                    to_fill.push_back(oid);
            }
        } else {
            for (int64_t t = new_best; t < prev_best; ++t) {
                auto it = buy_orders_.find(t);
                if (it != buy_orders_.end())
                    to_fill.insert(to_fill.end(), it->second.begin(), it->second.end());
            }
        }
        for (OrderId oid : to_fill) {
            filled_orders_.push_back(oid);
            fill_maker(orders_.at(oid), ts, orders_.at(oid).price_tick);
        }
        remove_filled_orders();
    }

    void ack_new(Ord &order, int64_t ts) {
        if (orders_.count(order.order_id)) {
            order.req = Status::Rejected;
            order.exch_timestamp = ts;
            return;
        }

        order.req = Status::None; // will be overridden to Rejected only if ack fails

        if (order.side == Side::Buy) {
            if (order.order_type == OrdType::Market) {
                fill<false>(order, ts, false, depth_.best_ask_tick(), order.leaves_qty);
                return;
            }
            if (order.price_tick >= depth_.best_ask_tick()) {
                if (order.time_in_force == TimeInForce::GTX) {
                    order.status = Status::Expired;
                    order.exch_timestamp = ts;
                    return;
                }
                fill<false>(order, ts, false, depth_.best_ask_tick(), order.leaves_qty);
            } else if (order.time_in_force == TimeInForce::GTC ||
                       order.time_in_force == TimeInForce::GTX) {
                qm_.new_order(order, depth_);
                order.status = Status::New;
                order.exch_timestamp = ts;
                buy_orders_[order.price_tick].insert(order.order_id);
                orders_.emplace(order.order_id, order);
            } else {
                order.status = Status::Expired;
                order.exch_timestamp = ts;
            }
        } else {
            if (order.order_type == OrdType::Market) {
                fill<false>(order, ts, false, depth_.best_bid_tick(), order.leaves_qty);
                return;
            }
            if (order.price_tick <= depth_.best_bid_tick()) {
                if (order.time_in_force == TimeInForce::GTX) {
                    order.status = Status::Expired;
                    order.exch_timestamp = ts;
                    return;
                }
                fill<false>(order, ts, false, depth_.best_bid_tick(), order.leaves_qty);
            } else if (order.time_in_force == TimeInForce::GTC ||
                       order.time_in_force == TimeInForce::GTX) {
                qm_.new_order(order, depth_);
                order.status = Status::New;
                order.exch_timestamp = ts;
                sell_orders_[order.price_tick].insert(order.order_id);
                orders_.emplace(order.order_id, order);
            } else {
                order.status = Status::Expired;
                order.exch_timestamp = ts;
            }
        }
    }

    void ack_cancel(Ord &order, int64_t ts) {
        auto it = orders_.find(order.order_id);
        if (it == orders_.end()) {
            order.req = Status::Rejected;
            order.exch_timestamp = ts;
            return;
        }
        order = it->second; // replace with exchange view
        if (order.side == Side::Buy)
            buy_orders_[order.price_tick].erase(order.order_id);
        else
            sell_orders_[order.price_tick].erase(order.order_id);
        orders_.erase(it);
        order.req = Status::None;
        order.status = Status::Canceled;
        order.exch_timestamp = ts;
    }

    void ack_modify(Ord &order, int64_t ts) {
        auto it = orders_.find(order.order_id);
        if (it == orders_.end()) {
            order.req = Status::Rejected;
            order.exch_timestamp = ts;
            return;
        }
        const int64_t prev_price_tick = it->second.price_tick;
        const double prev_leaves_qty = it->second.leaves_qty;

        if (prev_price_tick != order.price_tick || order.qty > prev_leaves_qty) {
            // Cancel + re-submit: resets queue position
            Ord cancel_order = order;
            ack_cancel(cancel_order, ts);
            ack_new(order, ts);
        } else {
            it->second.qty = order.qty;
            it->second.leaves_qty = order.qty;
            it->second.exch_timestamp = ts;
            it->second.status = Status::New;
            order.leaves_qty = order.qty;
            order.exch_timestamp = ts;
            order.status = Status::New;
        }
    }

    // ── Data members ──────────────────────────────────────────────────────────
    MD depth_;
    State<AT, FM, Q> state_;
    QM qm_;
    ExchToLocal<LM>
        order_e2l_; // bus carries Order<> (monostate); exchange promotes to Order<Q> internally

    std::unordered_map<OrderId, Ord> orders_;
    std::unordered_map<int64_t, std::unordered_set<OrderId>> buy_orders_;
    std::unordered_map<int64_t, std::unordered_set<OrderId>> sell_orders_;
    std::vector<OrderId> filled_orders_;
};

template <AssetTypeC AT, typename LM, typename QM, L2MarketDepthC MD, typename FM>
using PartialFillExchange = NoPartialFillExchange<AT, LM, QM, MD, FM, true>;

} // namespace hbt
