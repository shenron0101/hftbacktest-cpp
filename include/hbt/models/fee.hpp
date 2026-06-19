// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/models/fee.hpp — Fee models
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/backtest/models/fee.rs

#include <concepts>
#include <variant>

#include "hbt/types.hpp"

namespace hbt {

// ── FeeModel concept ──────────────────────────────────────────────────────────
template <typename FM, typename Q = std::monostate>
concept FeeModelC = requires(const FM &fm, const Order<Q> &order, double amount) {
    { fm.amount(order, amount) } -> std::convertible_to<double>;
};

// ── Fee structs ───────────────────────────────────────────────────────────────

struct CommonFees {
    double maker_fee = 0.0;
    double taker_fee = 0.0;

    constexpr CommonFees() = default;
    constexpr CommonFees(double maker, double taker) : maker_fee(maker), taker_fee(taker) {}
};

struct DirectionalFees {
    CommonFees common_fees;
    double buyer_fee = 0.0; ///< additional fee charged to the buyer (e.g. stamp duty)
    double seller_fee = 0.0;

    constexpr DirectionalFees(CommonFees common, double buyer, double seller)
        : common_fees(common), buyer_fee(buyer), seller_fee(seller) {}
};

// ── TradingValueFeeModel ──────────────────────────────────────────────────────
// Fee = rate × notional_amount  (amount passed in by apply_fill)

template <typename Fees> struct TradingValueFeeModel {
    explicit TradingValueFeeModel(Fees fees) : fees_(std::move(fees)) {}

    template <typename Q>
    [[nodiscard]] double amount(const Order<Q> &order, double notional) const noexcept;

  private:
    Fees fees_;
};

template <>
template <typename Q>
inline double TradingValueFeeModel<CommonFees>::amount(const Order<Q> &order,
                                                       double notional) const noexcept {
    return (order.maker ? fees_.maker_fee : fees_.taker_fee) * notional;
}

template <>
template <typename Q>
inline double TradingValueFeeModel<DirectionalFees>::amount(const Order<Q> &order,
                                                            double notional) const noexcept {
    const double base = order.maker ? fees_.common_fees.maker_fee : fees_.common_fees.taker_fee;
    const double dir = (order.side == Side::Buy) ? fees_.buyer_fee : fees_.seller_fee;
    return (base + dir) * notional;
}

// ── TradingQtyFeeModel ────────────────────────────────────────────────────────
// Fee = rate × exec_qty  (ignores notional for common; adds notional for directional)

template <typename Fees> struct TradingQtyFeeModel {
    explicit TradingQtyFeeModel(Fees fees) : fees_(std::move(fees)) {}

    template <typename Q>
    [[nodiscard]] double amount(const Order<Q> &order, double notional) const noexcept;

  private:
    Fees fees_;
};

template <>
template <typename Q>
inline double TradingQtyFeeModel<CommonFees>::amount(const Order<Q> &order,
                                                     double /*notional*/) const noexcept {
    return (order.maker ? fees_.maker_fee : fees_.taker_fee) * order.exec_qty;
}

template <>
template <typename Q>
inline double TradingQtyFeeModel<DirectionalFees>::amount(const Order<Q> &order,
                                                          double notional) const noexcept {
    const double base_rate =
        order.maker ? fees_.common_fees.maker_fee : fees_.common_fees.taker_fee;
    const double dir = (order.side == Side::Buy) ? fees_.buyer_fee : fees_.seller_fee;
    return base_rate * order.exec_qty + dir * notional;
}

// ── FlatPerTradeFeeModel ──────────────────────────────────────────────────────
// Fee = flat amount per fill (ignores qty and notional for common fees)

template <typename Fees> struct FlatPerTradeFeeModel {
    explicit FlatPerTradeFeeModel(Fees fees) : fees_(std::move(fees)) {}

    template <typename Q>
    [[nodiscard]] double amount(const Order<Q> &order, double /*notional*/) const noexcept;

  private:
    Fees fees_;
};

template <>
template <typename Q>
inline double FlatPerTradeFeeModel<CommonFees>::amount(const Order<Q> &order,
                                                       double) const noexcept {
    return order.maker ? fees_.maker_fee : fees_.taker_fee;
}

} // namespace hbt
