// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/data/pod.hpp — POD marker + alignment utilities
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/backtest/data/mod.rs
// (POD marker trait)

#include <concepts>
#include <cstddef>
#include <type_traits>

namespace hbt {

/// Types that satisfy POD<T> can be safely memcpy'd to/from raw byte buffers.
/// Specialize this for Event and OrderLatencyRow (both already trivially copyable).
template <typename T>
concept POD = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

} // namespace hbt
