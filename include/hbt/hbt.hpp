// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/hbt.hpp — Umbrella include for hftbacktest-cpp
// Include this single header to get everything needed for a backtest strategy.

// Core types
#include "hbt/types.hpp"

// Market depth
#include "hbt/depth/hashmap_depth.hpp"
#include "hbt/depth/market_depth.hpp"
#include "hbt/depth/roi_vector_depth.hpp"

// Models
#include "hbt/models/asset_type.hpp"
#include "hbt/models/fee.hpp"
#include "hbt/models/latency.hpp"
#include "hbt/models/queue.hpp"

// Backtest engine
#include "hbt/backtest/backtest.hpp"
#include "hbt/backtest/event_set.hpp"
#include "hbt/backtest/exchange.hpp"
#include "hbt/backtest/local.hpp"
#include "hbt/backtest/order_bus.hpp"
#include "hbt/backtest/processor.hpp"
#include "hbt/backtest/recorder.hpp"
#include "hbt/backtest/state.hpp"

// Data I/O
#include "hbt/data/npy.hpp"
#include "hbt/data/pod.hpp"
#include "hbt/data/reader.hpp"
