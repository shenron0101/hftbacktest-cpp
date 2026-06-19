// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>

#include "hbt/depth/hashmap_depth.hpp"

int main() {
    constexpr std::int64_t event_count = 5'000'000;
    hbt::HashMapMarketDepth depth{0.01, 0.001};
    const auto start = std::chrono::steady_clock::now();
    for (std::int64_t i = 0; i < event_count; ++i) {
        const double price = 100.0 + static_cast<double>(i % 100) * 0.01;
        if ((i & 1) == 0) {
            depth.update_bid_depth(price, static_cast<double>((i % 20) + 1), i);
        } else {
            depth.update_ask_depth(price + 1.0, static_cast<double>((i % 20) + 1), i);
        }
    }
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const auto rate = static_cast<double>(event_count) / elapsed;
    std::cout << std::fixed << std::setprecision(0) << "events=" << event_count
              << " seconds=" << std::setprecision(6) << elapsed
              << " events_per_second=" << std::setprecision(0) << rate
              << " checksum=" << std::setprecision(2) << (depth.best_bid() + depth.best_ask())
              << '\n';
}
