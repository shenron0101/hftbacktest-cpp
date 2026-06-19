// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/backtest/state.hpp — State<AT,FM>: position tracking + fill accounting
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/backtest/state.rs

#include "hbt/models/asset_type.hpp"
#include "hbt/models/fee.hpp"
#include "hbt/types.hpp"

namespace hbt {

/// Holds the running StateValues and delegates amount/equity math to AT, fees to FM.
/// AT = AssetType (LinearAsset / InverseAsset)
/// FM = FeeModel  (TradingValueFeeModel / TradingQtyFeeModel / FlatPerTradeFeeModel)
/// Q  = queue scratch type embedded in Order (only needed so FM::amount's Order<Q> resolves)
template <AssetTypeC AT, typename FM, typename Q = std::monostate> class State {
  public:
    State(AT asset_type, FM fee_model)
        : asset_type_(std::move(asset_type)), fee_model_(std::move(fee_model)) {}

    /// Apply a fill to accumulate position, balance, fee, trade stats.
    /// order.exec_qty and order.exec_price_tick must already be set.
    void apply_fill(const Order<Q> &order) noexcept {
        const double exec_price = order.exec_price();
        const double amount = asset_type_.amount(exec_price, order.exec_qty);
        const double sign = side_sign(order.side);

        sv_.position += order.exec_qty * sign;
        sv_.balance -= amount * sign;
        sv_.fee += fee_model_.amount(order, amount);
        sv_.num_trades += 1;
        sv_.trading_volume += order.exec_qty;
        sv_.trading_value += amount;
    }

    /// Equity at the given mid-price.
    [[nodiscard]] double equity(double mid) const noexcept {
        return asset_type_.equity(mid, sv_.balance, sv_.position, sv_.fee);
    }

    [[nodiscard]] const StateValues &values() const noexcept { return sv_; }
    [[nodiscard]] StateValues &values() noexcept { return sv_; }

    [[nodiscard]] const AT &asset_type() const noexcept { return asset_type_; }
    [[nodiscard]] const FM &fee_model() const noexcept { return fee_model_; }

  private:
    StateValues sv_{};
    AT asset_type_;
    FM fee_model_;
};

} // namespace hbt
