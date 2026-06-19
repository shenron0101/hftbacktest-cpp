// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/models/queue.hpp — Queue position models
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/backtest/models/queue.rs

#include <algorithm>
#include <cmath>
#include <variant>

#include "hbt/depth/market_depth.hpp"
#include "hbt/types.hpp"

namespace hbt {

// ── QueueModel concept ────────────────────────────────────────────────────────
// QM must:
//   · declare `using OrderState = <some type>` — stored inline in Order<OrderState>
//   · implement new_order, trade, depth, is_filled
// We verify the concept against Order<QM::OrderState>.

template <typename QM, typename MD>
concept QueueModelC =
    MarketDepthC<MD> && requires(QM &qm, const QM &cqm, Order<typename QM::OrderState> &o,
                                 double qty, double prev_qty, double new_qty, const MD &depth) {
        { qm.new_order(o, depth) } -> std::same_as<void>;
        { qm.trade(o, qty, depth) } -> std::same_as<void>;
        { qm.depth(o, prev_qty, new_qty, depth) } -> std::same_as<void>;
        { qm.is_filled(o, depth) } -> std::convertible_to<double>;
    };

// ── RiskAdverseQueueModel ─────────────────────────────────────────────────────
// Per-order scratch: a single f64 (front_q_qty).
// Conservative: queue advances only when trades occur at the same level.
struct RiskAdverseQueueModel {
    using OrderState = double; ///< front_q_qty

    template <MarketDepthC MD>
    void new_order(Order<double> &order, const MD &depth) const noexcept {
        order.q = (order.side == Side::Buy) ? depth.bid_qty_at_tick(order.price_tick)
                                            : depth.ask_qty_at_tick(order.price_tick);
    }

    template <MarketDepthC MD>
    void trade(Order<double> &order, double qty, const MD & /*depth*/) const noexcept {
        order.q -= qty;
    }

    template <MarketDepthC MD>
    void depth(Order<double> &order, double /*prev_qty*/, double new_qty,
               const MD & /*depth*/) const noexcept {
        order.q = std::min(order.q, new_qty);
    }

    template <MarketDepthC MD>
    [[nodiscard]] double is_filled(Order<double> &order, const MD &depth) const noexcept {
        const int64_t exec = static_cast<int64_t>(std::round(-order.q / depth.lot_size()));
        if (exec > 0) {
            order.q = 0.0;
            return exec * depth.lot_size();
        }
        return 0.0;
    }
};

// ── ProbQueueModel ────────────────────────────────────────────────────────────
// Per-order scratch: {front_q_qty, cum_trade_qty}
struct QueuePos {
    double front_q_qty = 0.0;
    double cum_trade_qty = 0.0;
};

// Probability concept: a callable with (front, back) → double
template <typename P>
concept ProbabilityC = requires(const P &p, double front, double back) {
    { p.prob(front, back) } -> std::convertible_to<double>;
};

template <ProbabilityC P> struct ProbQueueModel {
    using OrderState = QueuePos;

    explicit ProbQueueModel(P prob) : prob_(std::move(prob)) {}

    template <MarketDepthC MD>
    void new_order(Order<QueuePos> &order, const MD &depth) const noexcept {
        order.q.front_q_qty = (order.side == Side::Buy) ? depth.bid_qty_at_tick(order.price_tick)
                                                        : depth.ask_qty_at_tick(order.price_tick);
        order.q.cum_trade_qty = 0.0;
    }

    template <MarketDepthC MD>
    void trade(Order<QueuePos> &order, double qty, const MD & /*depth*/) const noexcept {
        order.q.front_q_qty -= qty;
        order.q.cum_trade_qty += qty;
    }

    template <MarketDepthC MD>
    void depth(Order<QueuePos> &order, double prev_qty, double new_qty,
               const MD & /*depth*/) const noexcept {
        double chg = prev_qty - new_qty;
        // Subtract quantity already moved by trades to avoid double counting.
        chg -= order.q.cum_trade_qty;
        order.q.cum_trade_qty = 0.0;

        if (chg < 0.0) {
            // Depth increased — front queue can only shrink to new_qty.
            order.q.front_q_qty = std::min(order.q.front_q_qty, new_qty);
            return;
        }

        const double front = order.q.front_q_qty;
        const double back = prev_qty - front;

        double p = prob_.prob(front, back);
        if (std::isinf(p))
            p = 1.0;

        const double back_after = back - p * chg;
        const double est_front = front - (1.0 - p) * chg + std::min(back_after, 0.0);
        order.q.front_q_qty = std::min(est_front, new_qty);
    }

    template <MarketDepthC MD>
    [[nodiscard]] double is_filled(Order<QueuePos> &order, const MD &depth) const noexcept {
        const int64_t exec =
            static_cast<int64_t>(std::round(-order.q.front_q_qty / depth.lot_size()));
        if (exec > 0) {
            order.q.front_q_qty = 0.0;
            return exec * depth.lot_size();
        }
        return 0.0;
    }

  private:
    P prob_;
};

// ── Probability functors ──────────────────────────────────────────────────────

/// f(x) = x^n;  p = f(back) / (f(back) + f(front))
struct PowerProbQueueFunc {
    explicit PowerProbQueueFunc(double n) : n_(n) {}
    [[nodiscard]] double prob(double front, double back) const noexcept {
        const double fb = std::pow(back, n_);
        const double ff = std::pow(front, n_);
        return fb / (fb + ff);
    }

  private:
    double n_;
};

/// f(x) = x^n;  p = f(back) / f(back + front)
struct PowerProbQueueFunc2 {
    explicit PowerProbQueueFunc2(double n) : n_(n) {}
    [[nodiscard]] double prob(double front, double back) const noexcept {
        return std::pow(back, n_) / std::pow(back + front, n_);
    }

  private:
    double n_;
};

/// f(x) = x^n;  p = 1 - f(front / (front + back))
struct PowerProbQueueFunc3 {
    explicit PowerProbQueueFunc3(double n) : n_(n) {}
    [[nodiscard]] double prob(double front, double back) const noexcept {
        return 1.0 - std::pow(front / (front + back), n_);
    }

  private:
    double n_;
};

/// f(x) = ln(1 + x);  p = f(back) / (f(back) + f(front))
struct LogProbQueueFunc {
    [[nodiscard]] double prob(double front, double back) const noexcept {
        const double fb = std::log1p(back);
        const double ff = std::log1p(front);
        return fb / (fb + ff);
    }
};

/// f(x) = ln(1 + x);  p = f(back) / f(back + front)
struct LogProbQueueFunc2 {
    [[nodiscard]] double prob(double front, double back) const noexcept {
        return std::log1p(back) / std::log1p(back + front);
    }
};

} // namespace hbt
