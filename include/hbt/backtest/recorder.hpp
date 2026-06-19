// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/backtest/recorder.hpp — RecordRow + Recorder
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/backtest/recorder.rs
//
// Appends RecordRow entries on each call to record() and flushes to a binary
// .npy-compatible file (raw array of RecordRow, no zip).  The Python stats
// module can read it as numpy with the record_dtype defined in types.py.

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "hbt/types.hpp"

namespace hbt {

/// One recorded snapshot — binary-identical to record_dtype from types.py.
struct RecordRow {
    int64_t timestamp = 0;
    double price = 0.0;
    double position = 0.0;
    double balance = 0.0;
    double fee = 0.0;
    int64_t num_trades = 0;
    double trading_volume = 0.0;
    double trading_value = 0.0;
};
static_assert(sizeof(RecordRow) == 64);
static_assert(std::is_trivially_copyable_v<RecordRow>);

/// Recorder: accumulates RecordRow in memory and writes to a binary file.
class Recorder {
  public:
    explicit Recorder(std::size_t reserve = 0) { rows_.reserve(reserve); }

    /// Record a snapshot from the Backtest engine.
    /// `asset_no` — which asset's values to record
    /// `mid_price` — mid-price for the row's price field
    template <typename Bot> void record(const Bot &bot, std::size_t asset_no, double mid_price) {
        const auto &sv = bot.state_values(asset_no);
        rows_.push_back(RecordRow{
            bot.current_timestamp(),
            mid_price,
            sv.position,
            sv.balance,
            sv.fee,
            sv.num_trades,
            sv.trading_volume,
            sv.trading_value,
        });
    }

    /// Manual row append.
    void push(RecordRow row) { rows_.push_back(row); }

    [[nodiscard]] const std::vector<RecordRow> &rows() const noexcept { return rows_; }
    [[nodiscard]] std::size_t size() const noexcept { return rows_.size(); }

    /// Write rows to a flat binary file (raw RecordRow array, no npy header).
    /// Load in Python with: np.frombuffer(open(path,'rb').read(), dtype=record_dtype)
    void save_raw(const std::string &path) const {
        std::ofstream f(path, std::ios::binary);
        if (!f)
            throw std::runtime_error("Recorder::save_raw: cannot open " + path);
        f.write(reinterpret_cast<const char *>(rows_.data()),
                static_cast<std::streamsize>(rows_.size() * sizeof(RecordRow)));
        if (!f)
            throw std::runtime_error("Recorder::save_raw: write failed");
    }

    void clear() noexcept { rows_.clear(); }

  private:
    std::vector<RecordRow> rows_;
};

} // namespace hbt
