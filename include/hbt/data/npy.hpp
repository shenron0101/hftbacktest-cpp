// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/data/npy.hpp — .npy header parser + .npz (zip) reader
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/backtest/data/npy/parser.rs
//            https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/backtest/data/mod.rs
//
// Provides:
//   - parse_npy_header(buf, len)  → NpyHeader{descr, fortran_order, shape}
//   - read_npy(path)              → std::vector<uint8_t> (payload only)
//   - read_npz_member(path, key)  → std::vector<uint8_t> (payload for .npz member)
//   - load_events(path)           → std::vector<Event>
//   - load_latency_rows(path)     → std::vector<OrderLatencyRow>
//
// .npz = ZIP file where each member is a .npy file. The implementation supports
// classic ZIP32 archives with stored or DEFLATE members and uses zlib to inflate.

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "hbt/models/latency.hpp"
#include "hbt/types.hpp"

namespace hbt {

// ── NpyHeader ─────────────────────────────────────────────────────────────────
struct NpyHeader {
    std::string descr; ///< dtype string, e.g. "[('ev','<u8'),...]"
    bool fortran_order = false;
    std::vector<int64_t> shape;
    std::size_t data_offset = 0; ///< byte offset of data payload in file/buffer
};

/// Parse the numpy 1.x/2.x binary header from a buffer.
/// Throws std::runtime_error on malformed input.
NpyHeader parse_npy_header(const uint8_t *buf, std::size_t buflen);

/// Read a .npy file and return the raw payload bytes (without header).
/// Throws on file I/O or parse errors.
std::vector<uint8_t> read_npy_payload(const std::string &path);

/// Read the payload of a named member from a .npz (zip) archive.
std::vector<uint8_t> read_npz_member_payload(const std::string &npz_path,
                                             const std::string &member_name);

// ── Typed loaders ─────────────────────────────────────────────────────────────

/// Load an array of POD records from a .npy file.
/// T must be trivially copyable.  sizeof(T) must divide the payload size exactly.
template <typename T> std::vector<T> load_npy(const std::string &path) {
    static_assert(std::is_trivially_copyable_v<T>);
    auto payload = read_npy_payload(path);
    if (payload.size() % sizeof(T) != 0) {
        throw std::runtime_error("npy payload size not a multiple of record size");
    }
    std::vector<T> result(payload.size() / sizeof(T));
    std::memcpy(result.data(), payload.data(), payload.size());
    return result;
}

/// Load an array of POD records from a named member of a .npz file.
template <typename T>
std::vector<T> load_npz(const std::string &path, const std::string &member = "data") {
    static_assert(std::is_trivially_copyable_v<T>);
    auto payload = read_npz_member_payload(path, member + ".npy");
    if (payload.size() % sizeof(T) != 0) {
        throw std::runtime_error("npz member payload size not a multiple of record size");
    }
    std::vector<T> result(payload.size() / sizeof(T));
    std::memcpy(result.data(), payload.data(), payload.size());
    return result;
}

/// Convenience: load events from a .npz produced by hftbacktest / our pipeline.
inline std::vector<Event> load_events(const std::string &path, const std::string &member = "data") {
    return load_npz<Event>(path, member);
}

/// Convenience: load OrderLatencyRow data from a .npz.
inline std::vector<OrderLatencyRow> load_latency_rows(const std::string &path,
                                                      const std::string &member = "data") {
    return load_npz<OrderLatencyRow>(path, member);
}

} // namespace hbt
