// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/backtest/backtest.hpp — Backtest engine: event loop + Bot API
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/backtest/mod.rs
//
// Backtest<MD> owns:
//   - N local  processors (LocalProcessor*)
//   - N exchange processors (Processor*)
//   - N per-asset event data vectors (one Reader/vector per asset)
//   - EventSet scheduler
//
// The goto() dispatch loop mirrors the Rust implementation exactly.

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include "hbt/backtest/event_set.hpp"
#include "hbt/backtest/processor.hpp"
#include "hbt/data/reader.hpp"
#include "hbt/types.hpp"

namespace hbt {

// ── ProcessorState ────────────────────────────────────────────────────────────
// Wraps a Processor with its event data source (a snapshot of loaded Events)
// and a cursor into that data.
struct ProcessorState {
    std::unique_ptr<Processor> processor;
    EventReader reader;
    std::shared_ptr<const std::vector<Event>> data;
    std::optional<std::size_t> row;

    ProcessorState(std::unique_ptr<Processor> p, std::shared_ptr<EventReaderCache> cache)
        : processor(std::move(p)), reader(std::move(cache)) {}

    /// Returns the timestamp at which the processor will see row `rn`.
    [[nodiscard]] std::optional<int64_t> event_seen_timestamp(std::size_t rn) const {
        if (!data || rn >= data->size())
            return std::nullopt;
        return processor->event_seen_timestamp((*data)[rn]);
    }

    [[nodiscard]] const Event &event(std::size_t rn) const { return data->at(rn); }

    /// Advance past current row to find the next valid row.
    /// Returns the timestamp of the next row, or std::nullopt if end of data.
    std::optional<int64_t> advance() {
        const std::size_t start = row ? *row + 1 : 0;
        // Scan rest of current chunk
        if (data) {
            for (std::size_t rn = start; rn < data->size(); ++rn) {
                auto ts = processor->event_seen_timestamp((*data)[rn]);
                if (ts) {
                    row = rn;
                    return ts;
                }
            }
        }
        while ((data = reader.next_chunk())) {
            row = std::nullopt;
            for (std::size_t rn = 0; rn < data->size(); ++rn) {
                auto ts = processor->event_seen_timestamp((*data)[rn]);
                if (ts) {
                    row = rn;
                    return ts;
                }
            }
        }
        row = std::nullopt;
        return std::nullopt;
    }

    std::optional<std::size_t> next_row() {
        if (!row)
            advance();
        return row;
    }
};

// ── Backtest ──────────────────────────────────────────────────────────────────
class Backtest {
  public:
    Backtest() = default;

    /// Add one asset from in-memory chunks.
    void add_asset(std::unique_ptr<LocalProcessor> local, std::unique_ptr<Processor> exch,
                   std::vector<std::vector<Event>> local_data,
                   std::vector<std::vector<Event>> exch_data) {
        auto local_reader = EventReaderCache::from_chunks(std::move(local_data), 1);
        auto exch_reader = EventReaderCache::from_chunks(std::move(exch_data), 1);
        local_.push_back(
            std::make_unique<ProcessorState>(std::move(local), std::move(local_reader)));
        exch_.push_back(std::make_unique<ProcessorState>(std::move(exch), std::move(exch_reader)));
    }

    /// Add one asset backed by an ordered, shared shard reader.
    void add_asset(std::unique_ptr<LocalProcessor> local, std::unique_ptr<Processor> exch,
                   std::shared_ptr<EventReaderCache> reader) {
        local_.push_back(std::make_unique<ProcessorState>(std::move(local), reader));
        exch_.push_back(std::make_unique<ProcessorState>(std::move(exch), std::move(reader)));
    }

    [[nodiscard]] std::size_t num_assets() const noexcept { return local_.size(); }

    // ── Bot API ───────────────────────────────────────────────────────────────
    [[nodiscard]] int64_t current_timestamp() const noexcept { return cur_ts_; }

    [[nodiscard]] double position(std::size_t asset_no) const {
        return local_proc(asset_no).position();
    }

    [[nodiscard]] const StateValues &state_values(std::size_t asset_no) const {
        return local_proc(asset_no).state_values();
    }

    [[nodiscard]] const std::unordered_map<OrderId, Order<>> &orders(std::size_t asset_no) const {
        return local_proc(asset_no).orders();
    }

    [[nodiscard]] const std::vector<Event> &last_trades(std::size_t asset_no) const {
        return local_proc(asset_no).last_trades();
    }

    void clear_last_trades(std::optional<std::size_t> asset_no = std::nullopt) {
        if (asset_no) {
            local_proc_mut(*asset_no).clear_last_trades();
        } else {
            for (auto &lp : local_)
                static_cast<LocalProcessor *>(lp->processor.get())->clear_last_trades();
        }
    }

    void clear_inactive_orders(std::optional<std::size_t> asset_no = std::nullopt) {
        if (asset_no) {
            local_proc_mut(*asset_no).clear_inactive_orders();
        } else {
            for (auto &lp : local_)
                static_cast<LocalProcessor *>(lp->processor.get())->clear_inactive_orders();
        }
    }

    std::optional<std::pair<int64_t, int64_t>> feed_latency(std::size_t asset_no) const {
        return local_proc(asset_no).feed_latency();
    }
    std::optional<std::tuple<int64_t, int64_t, int64_t>> order_latency(std::size_t asset_no) const {
        return local_proc(asset_no).order_latency();
    }

    // ── Depth / BBO ───────────────────────────────────────────────────────────
    [[nodiscard]] double best_bid(std::size_t asset_no) const {
        return local_proc(asset_no).best_bid();
    }
    [[nodiscard]] double best_ask(std::size_t asset_no) const {
        return local_proc(asset_no).best_ask();
    }
    [[nodiscard]] int64_t best_bid_tick(std::size_t asset_no) const {
        return local_proc(asset_no).best_bid_tick();
    }
    [[nodiscard]] int64_t best_ask_tick(std::size_t asset_no) const {
        return local_proc(asset_no).best_ask_tick();
    }
    [[nodiscard]] double tick_size(std::size_t asset_no) const {
        return local_proc(asset_no).tick_size();
    }
    [[nodiscard]] double lot_size(std::size_t asset_no) const {
        return local_proc(asset_no).lot_size();
    }
    [[nodiscard]] double mid(std::size_t asset_no) const {
        const double b = local_proc(asset_no).best_bid();
        const double a = local_proc(asset_no).best_ask();
        return (std::isnan(b) || std::isnan(a)) ? std::numeric_limits<double>::quiet_NaN()
                                                : (b + a) * 0.5;
    }

    // ── elapse / elapse_bt / wait_* ───────────────────────────────────────────
    ElapseResult elapse(int64_t duration) {
        ensure_initialized();
        return do_goto<false>(cur_ts_ + duration, WaitOrderResponse::none());
    }

    ElapseResult elapse_bt(int64_t duration) {
        ensure_initialized();
        return do_goto<false>(cur_ts_ + duration, WaitOrderResponse::none());
    }

    ElapseResult wait_next_feed(bool any_data, bool any_order) {
        ensure_initialized();
        if (any_data)
            return do_goto<true>(UNTIL_END_OF_DATA, WaitOrderResponse::none());
        if (any_order)
            return do_goto<false>(UNTIL_END_OF_DATA, WaitOrderResponse::any());
        return do_goto<false>(UNTIL_END_OF_DATA, WaitOrderResponse::none());
    }

    ElapseResult wait_order_response(std::size_t asset_no, OrderId order_id) {
        ensure_initialized();
        return do_goto<false>(UNTIL_END_OF_DATA, WaitOrderResponse::specified(asset_no, order_id));
    }

    ElapseResult close() {
        ensure_initialized();
        return do_goto<false>(UNTIL_END_OF_DATA, WaitOrderResponse::none());
    }

    // ── Order submission ──────────────────────────────────────────────────────
    ElapseResult submit_buy_order(std::size_t asset_no, OrderId order_id, double price, double qty,
                                  TimeInForce tif, OrdType ord_type, bool wait) {
        ensure_initialized();
        local_proc_mut(asset_no).submit_order(order_id, Side::Buy, price, qty, ord_type, tif,
                                              cur_ts_);
        if (wait)
            return do_goto<false>(UNTIL_END_OF_DATA,
                                  WaitOrderResponse::specified(asset_no, order_id));
        return ElapseResult::Ok;
    }

    ElapseResult submit_sell_order(std::size_t asset_no, OrderId order_id, double price, double qty,
                                   TimeInForce tif, OrdType ord_type, bool wait) {
        ensure_initialized();
        local_proc_mut(asset_no).submit_order(order_id, Side::Sell, price, qty, ord_type, tif,
                                              cur_ts_);
        if (wait)
            return do_goto<false>(UNTIL_END_OF_DATA,
                                  WaitOrderResponse::specified(asset_no, order_id));
        return ElapseResult::Ok;
    }

    void modify(std::size_t asset_no, OrderId order_id, double price, double qty) {
        ensure_initialized();
        local_proc_mut(asset_no).modify(order_id, price, qty, cur_ts_);
    }

    void cancel(std::size_t asset_no, OrderId order_id) {
        ensure_initialized();
        local_proc_mut(asset_no).cancel(order_id, cur_ts_);
    }

  private:
    // ── Helpers ───────────────────────────────────────────────────────────────
    [[nodiscard]] LocalProcessor &local_proc(std::size_t n) const {
        return *static_cast<LocalProcessor *>(local_[n]->processor.get());
    }
    [[nodiscard]] LocalProcessor &local_proc_mut(std::size_t n) {
        return *static_cast<LocalProcessor *>(local_[n]->processor.get());
    }

    void ensure_initialized() {
        if (initialized_)
            return;
        assert(!local_.empty());
        evs_ = std::make_unique<EventSet>(local_.size());
        initialize_evs();
        // Set cur_ts to the first event
        if (auto nxt = evs_->next()) {
            cur_ts_ = nxt->timestamp;
        }
        initialized_ = true;
    }

    void initialize_evs() {
        for (std::size_t i = 0; i < local_.size(); ++i) {
            if (auto ts = local_[i]->advance())
                evs_->update_local_data(i, *ts);
            else
                evs_->invalidate_local_data(i);

            if (auto ts = exch_[i]->advance())
                evs_->update_exch_data(i, *ts);
            else
                evs_->invalidate_exch_data(i);
        }
    }

    /// Core dispatch loop — mirrors Rust's `goto<WAIT_NEXT_FEED>`.
    template <bool WAIT_NEXT_FEED>
    ElapseResult do_goto(int64_t target_ts, WaitOrderResponse wait_resp) {
        using K = EventIntentKind;
        ElapseResult result = ElapseResult::Ok;

        // Pre-load order bus timestamps into EventSet
        for (std::size_t i = 0; i < local_.size(); ++i) {
            evs_->update_exch_order(i, local_[i]->processor->earliest_send_order_timestamp());
            evs_->update_local_order(i, local_[i]->processor->earliest_recv_order_timestamp());
        }

        for (;;) {
            auto opt = evs_->next();
            if (!opt) {
                // Return the most recent meaningful result (e.g., MarketFeed) rather
                // than EndOfData if we already processed a relevant event this call.
                return (result != ElapseResult::Ok) ? result : ElapseResult::EndOfData;
            }

            const auto &ev = *opt;
            if (ev.timestamp > target_ts) {
                cur_ts_ = target_ts;
                return result;
            }

            cur_ts_ = ev.timestamp;

            switch (ev.kind) {
            case K::LocalData: {
                auto &ls = *local_[ev.asset_no];
                if (auto rn = ls.next_row()) {
                    ls.processor->process(ls.event(*rn));
                }
                if (auto ts = ls.advance())
                    evs_->update_local_data(ev.asset_no, *ts);
                else
                    evs_->invalidate_local_data(ev.asset_no);

                if constexpr (WAIT_NEXT_FEED) {
                    target_ts = ev.timestamp;
                    result = ElapseResult::MarketFeed;
                }
                break;
            }
            case K::LocalOrder: {
                auto &ls = *local_[ev.asset_no];
                std::optional<OrderId> wait_oid;
                if (wait_resp.kind == WaitOrderResponse::Kind::Specified &&
                    wait_resp.asset_no == ev.asset_no)
                    wait_oid = wait_resp.order_id;

                bool got = ls.processor->process_recv_order(ev.timestamp, wait_oid);
                if (got || wait_resp.kind == WaitOrderResponse::Kind::Any) {
                    target_ts = ev.timestamp;
                    if constexpr (WAIT_NEXT_FEED)
                        result = ElapseResult::OrderResponse;
                }
                evs_->update_local_order(ev.asset_no,
                                         ls.processor->earliest_recv_order_timestamp());
                break;
            }
            case K::ExchData: {
                auto &es = *exch_[ev.asset_no];
                if (auto rn = es.next_row()) {
                    es.processor->process(es.event(*rn));
                }
                if (auto ts = es.advance())
                    evs_->update_exch_data(ev.asset_no, *ts);
                else
                    evs_->invalidate_exch_data(ev.asset_no);

                // Exch→Local responses may now be scheduled
                evs_->update_local_order(ev.asset_no,
                                         es.processor->earliest_send_order_timestamp());
                break;
            }
            case K::ExchOrder: {
                auto &es = *exch_[ev.asset_no];
                es.processor->process_recv_order(ev.timestamp, std::nullopt);
                evs_->update_exch_order(ev.asset_no, es.processor->earliest_recv_order_timestamp());
                evs_->update_local_order(ev.asset_no,
                                         es.processor->earliest_send_order_timestamp());
                break;
            }
            }
        }
    }

    // ── Data members ──────────────────────────────────────────────────────────
    std::vector<std::unique_ptr<ProcessorState>> local_;
    std::vector<std::unique_ptr<ProcessorState>> exch_;
    std::unique_ptr<EventSet> evs_;
    int64_t cur_ts_ = std::numeric_limits<int64_t>::max();
    bool initialized_ = false;
};

} // namespace hbt
