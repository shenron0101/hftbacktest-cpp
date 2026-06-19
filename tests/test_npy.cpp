// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

// tests/test_npy.cpp — Phase 4: npy/npz I/O tests
// These tests use small in-memory buffers rather than real files.

#include "hbt/data/npy.hpp"
#include "hbt/types.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace hbt;

// ── parse_npy_header ──────────────────────────────────────────────────────────
// Construct a minimal .npy v1 header in memory and parse it.
static std::vector<uint8_t> make_npy_v1(const std::string &descr, std::size_t nrows) {
    std::string dict = "{'descr': '" + descr + "', 'fortran_order': False, 'shape': (" +
                       std::to_string(nrows) + ",), }";
    // Pad to 64-byte alignment
    while ((10 + dict.size()) % 64 != 0)
        dict += ' ';

    std::vector<uint8_t> buf;
    // Magic + version
    buf.push_back(0x93);
    buf.push_back('N');
    buf.push_back('U');
    buf.push_back('M');
    buf.push_back('P');
    buf.push_back('Y');
    buf.push_back(1);
    buf.push_back(0); // v1.0
    uint16_t hlen = static_cast<uint16_t>(dict.size());
    buf.push_back(static_cast<uint8_t>(hlen & 0xff));
    buf.push_back(static_cast<uint8_t>((hlen >> 8) & 0xff));
    for (char c : dict)
        buf.push_back(static_cast<uint8_t>(c));
    return buf;
}

TEST(NpyHeader, ParseV1) {
    auto buf = make_npy_v1("|u1", 42);
    auto hdr = parse_npy_header(buf.data(), buf.size());
    EXPECT_EQ(hdr.descr, "|u1");
    EXPECT_FALSE(hdr.fortran_order);
    ASSERT_EQ(hdr.shape.size(), 1u);
    EXPECT_EQ(hdr.shape[0], 42);
}

TEST(NpyHeader, BadMagic) {
    std::vector<uint8_t> buf(20, 0);
    EXPECT_THROW(parse_npy_header(buf.data(), buf.size()), std::runtime_error);
}

static void append_u16(std::vector<uint8_t> &bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8U));
}

static void append_u32(std::vector<uint8_t> &bytes, uint32_t value) {
    append_u16(bytes, static_cast<uint16_t>(value));
    append_u16(bytes, static_cast<uint16_t>(value >> 16U));
}

TEST(NpzReader, ReadsStoredNpyMember) {
    auto npy = make_npy_v1("|u1", 3);
    npy.insert(npy.end(), {1, 2, 3});
    const std::string name = "data.npy";

    std::vector<uint8_t> zip;
    append_u32(zip, 0x04034b50U);
    append_u16(zip, 20);
    append_u16(zip, 0);
    append_u16(zip, 0);
    append_u16(zip, 0);
    append_u16(zip, 0);
    append_u32(zip, 0);
    append_u32(zip, static_cast<uint32_t>(npy.size()));
    append_u32(zip, static_cast<uint32_t>(npy.size()));
    append_u16(zip, static_cast<uint16_t>(name.size()));
    append_u16(zip, 0);
    zip.insert(zip.end(), name.begin(), name.end());
    zip.insert(zip.end(), npy.begin(), npy.end());

    const auto central_offset = static_cast<uint32_t>(zip.size());
    append_u32(zip, 0x02014b50U);
    append_u16(zip, 20);
    append_u16(zip, 20);
    append_u16(zip, 0);
    append_u16(zip, 0);
    append_u16(zip, 0);
    append_u16(zip, 0);
    append_u32(zip, 0);
    append_u32(zip, static_cast<uint32_t>(npy.size()));
    append_u32(zip, static_cast<uint32_t>(npy.size()));
    append_u16(zip, static_cast<uint16_t>(name.size()));
    append_u16(zip, 0);
    append_u16(zip, 0);
    append_u16(zip, 0);
    append_u16(zip, 0);
    append_u32(zip, 0);
    append_u32(zip, 0);
    zip.insert(zip.end(), name.begin(), name.end());
    const auto central_size = static_cast<uint32_t>(zip.size()) - central_offset;

    append_u32(zip, 0x06054b50U);
    append_u16(zip, 0);
    append_u16(zip, 0);
    append_u16(zip, 1);
    append_u16(zip, 1);
    append_u32(zip, central_size);
    append_u32(zip, central_offset);
    append_u16(zip, 0);

    const auto path = std::filesystem::temp_directory_path() / "hbt-npz-reader-test.npz";
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char *>(zip.data()),
                     static_cast<std::streamsize>(zip.size()));
    }
    const auto payload = read_npz_member_payload(path.string(), name);
    std::filesystem::remove(path);
    EXPECT_EQ(payload, (std::vector<uint8_t>{1, 2, 3}));
}

// ── Event struct size/layout sanity ──────────────────────────────────────────
TEST(EventLayout, MatchesNumpyDtype) {
    // numpy event_dtype: 8 fields × 8 bytes = 64 bytes (aligned)
    EXPECT_EQ(sizeof(Event), 64u);
    // Check field offsets via pointer arithmetic
    Event e{};
    const uint8_t *base = reinterpret_cast<const uint8_t *>(&e);
    EXPECT_EQ(reinterpret_cast<const uint8_t *>(&e.ev) - base, 0);
    EXPECT_EQ(reinterpret_cast<const uint8_t *>(&e.exch_ts) - base, 8);
    EXPECT_EQ(reinterpret_cast<const uint8_t *>(&e.local_ts) - base, 16);
    EXPECT_EQ(reinterpret_cast<const uint8_t *>(&e.px) - base, 24);
    EXPECT_EQ(reinterpret_cast<const uint8_t *>(&e.qty) - base, 32);
    EXPECT_EQ(reinterpret_cast<const uint8_t *>(&e.order_id) - base, 40);
    EXPECT_EQ(reinterpret_cast<const uint8_t *>(&e.ival) - base, 48);
    EXPECT_EQ(reinterpret_cast<const uint8_t *>(&e.fval) - base, 56);
}
