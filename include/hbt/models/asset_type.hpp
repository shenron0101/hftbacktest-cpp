// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/models/asset_type.hpp — Linear and Inverse asset type models
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/backtest/assettype.rs

#include <concepts>

namespace hbt {

// ── AssetType concept ─────────────────────────────────────────────────────────
template <typename AT>
concept AssetTypeC =
    requires(const AT &at, double price, double qty, double balance, double position, double fee) {
        { at.amount(price, qty) } -> std::convertible_to<double>;
        { at.equity(price, balance, position, fee) } -> std::convertible_to<double>;
    };

// ── LinearAsset ───────────────────────────────────────────────────────────────
// USDT-margined perpetuals: notional = contract_size × price × qty
struct LinearAsset {
    explicit constexpr LinearAsset(double contract_size = 1.0) : contract_size_(contract_size) {}

    [[nodiscard]] constexpr double amount(double exec_price, double qty) const noexcept {
        return contract_size_ * exec_price * qty;
    }

    [[nodiscard]] constexpr double equity(double price, double balance, double position,
                                          double fee) const noexcept {
        return balance + contract_size_ * position * price - fee;
    }

  private:
    double contract_size_;
};

static_assert(AssetTypeC<LinearAsset>);

// ── InverseAsset ──────────────────────────────────────────────────────────────
// Coin-margined perpetuals: notional = contract_size × qty / price
struct InverseAsset {
    explicit constexpr InverseAsset(double contract_size = 1.0) : contract_size_(contract_size) {}

    [[nodiscard]] constexpr double amount(double exec_price, double qty) const noexcept {
        return contract_size_ * qty / exec_price;
    }

    [[nodiscard]] constexpr double equity(double price, double balance, double position,
                                          double fee) const noexcept {
        return -balance - contract_size_ * position / price - fee;
    }

  private:
    double contract_size_;
};

static_assert(AssetTypeC<InverseAsset>);

} // namespace hbt
