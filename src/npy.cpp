// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

// hbt/src/npy.cpp — .npy / .npz I/O implementation
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/backtest/data/npy/

#include "hbt/data/npy.hpp"

#include <cassert>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>

namespace hbt {

// ── .npy magic + version ─────────────────────────────────────────────────────
// Format: https://numpy.org/doc/stable/reference/generated/numpy.lib.format.html
static constexpr uint8_t NPY_MAGIC[6] = {0x93, 'N', 'U', 'M', 'P', 'Y'};

NpyHeader parse_npy_header(const uint8_t *buf, std::size_t buflen) {
    if (buflen < 10)
        throw std::runtime_error("npy buffer too small");
    if (std::memcmp(buf, NPY_MAGIC, 6) != 0)
        throw std::runtime_error("npy magic bytes mismatch");

    const uint8_t major = buf[6];
    const uint8_t minor = buf[7];
    (void)minor;

    uint32_t header_len = 0;
    if (major == 1) {
        // 2-byte little-endian header length
        header_len = static_cast<uint32_t>(buf[8]) | (static_cast<uint32_t>(buf[9]) << 8);
        if (buflen < 10 + header_len)
            throw std::runtime_error("npy header truncated (v1)");
        buf += 10;
    } else if (major == 2 || major == 3) {
        // 4-byte little-endian header length
        if (buflen < 12)
            throw std::runtime_error("npy buffer too small (v2/3)");
        header_len = static_cast<uint32_t>(buf[8]) | (static_cast<uint32_t>(buf[9]) << 8) |
                     (static_cast<uint32_t>(buf[10]) << 16) |
                     (static_cast<uint32_t>(buf[11]) << 24);
        if (buflen < 12 + header_len)
            throw std::runtime_error("npy header truncated (v2/3)");
        buf += 12;
    } else {
        throw std::runtime_error("unsupported npy major version");
    }

    // The header is a Python dict literal, e.g.:
    // {'descr': '...', 'fortran_order': False, 'shape': (N,), }
    std::string hdr(reinterpret_cast<const char *>(buf), header_len);

    NpyHeader result;

    // ── descr ─────────────────────────────────────────────────────────────────
    auto descr_pos = hdr.find("'descr'");
    if (descr_pos == std::string::npos)
        descr_pos = hdr.find("\"descr\"");
    if (descr_pos == std::string::npos)
        throw std::runtime_error("npy header: no 'descr'");
    auto colon = hdr.find(':', descr_pos);
    auto q1 = hdr.find_first_of("'\"", colon + 1);
    if (q1 == std::string::npos)
        throw std::runtime_error("npy header: bad descr value");
    char delim = hdr[q1];
    auto q2 = hdr.find(delim, q1 + 1);
    if (q2 == std::string::npos)
        throw std::runtime_error("npy header: unterminated descr");
    result.descr = hdr.substr(q1 + 1, q2 - q1 - 1);

    // ── fortran_order ─────────────────────────────────────────────────────────
    auto fo_pos = hdr.find("'fortran_order'");
    if (fo_pos == std::string::npos)
        fo_pos = hdr.find("\"fortran_order\"");
    if (fo_pos != std::string::npos) {
        auto fc = hdr.find(':', fo_pos);
        auto fv = hdr.find_first_not_of(" \t\r\n", fc + 1);
        result.fortran_order = (hdr.substr(fv, 4) == "True");
    }

    // ── shape ─────────────────────────────────────────────────────────────────
    auto sh_pos = hdr.find("'shape'");
    if (sh_pos == std::string::npos)
        sh_pos = hdr.find("\"shape\"");
    if (sh_pos != std::string::npos) {
        auto open = hdr.find('(', sh_pos);
        auto close = hdr.find(')', open);
        if (open != std::string::npos && close != std::string::npos) {
            std::string shape_str = hdr.substr(open + 1, close - open - 1);
            std::size_t p = 0;
            while (p < shape_str.size()) {
                while (p < shape_str.size() && (shape_str[p] == ' ' || shape_str[p] == ','))
                    ++p;
                if (p >= shape_str.size())
                    break;
                char *end;
                int64_t v = std::strtoll(shape_str.c_str() + p, &end, 10);
                if (end == shape_str.c_str() + p)
                    break;
                result.shape.push_back(v);
                p = static_cast<std::size_t>(end - shape_str.c_str());
            }
        }
    }

    // data_offset = header preamble + header_len
    result.data_offset = (major == 1 ? 10u : 12u) + header_len;
    return result;
}

// ── .npy file reader ──────────────────────────────────────────────────────────
std::vector<uint8_t> read_npy_payload(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("cannot open npy file: " + path);

    auto fsize = static_cast<std::size_t>(f.tellg());
    std::vector<uint8_t> buf(fsize);
    f.seekg(0);
    f.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(fsize));
    if (!f)
        throw std::runtime_error("read error: " + path);

    NpyHeader hdr = parse_npy_header(buf.data(), buf.size());
    std::vector<uint8_t> payload(buf.begin() + static_cast<std::ptrdiff_t>(hdr.data_offset),
                                 buf.end());
    return payload;
}

namespace {

uint16_t read_u16(const std::vector<uint8_t> &bytes, std::size_t offset) {
    if (offset + 2 > bytes.size())
        throw std::runtime_error("truncated zip field");
    return static_cast<uint16_t>(bytes[offset]) | static_cast<uint16_t>(bytes[offset + 1] << 8U);
}

uint32_t read_u32(const std::vector<uint8_t> &bytes, std::size_t offset) {
    if (offset + 4 > bytes.size())
        throw std::runtime_error("truncated zip field");
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24U);
}

std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("cannot open npz file: " + path);
    const auto size = static_cast<std::size_t>(file.tellg());
    std::vector<uint8_t> bytes(size);
    file.seekg(0);
    file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
    if (!file)
        throw std::runtime_error("read error: " + path);
    return bytes;
}

std::vector<uint8_t> inflate_raw(const uint8_t *input, std::size_t input_size,
                                 std::size_t output_size) {
    std::vector<uint8_t> output(output_size);
    z_stream stream{};
    stream.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(input));
    stream.avail_in = static_cast<uInt>(input_size);
    stream.next_out = reinterpret_cast<Bytef *>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw std::runtime_error("cannot initialize deflate decoder");
    }
    const int result = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (result != Z_STREAM_END || stream.total_out != output_size) {
        throw std::runtime_error("invalid deflated npz member");
    }
    return output;
}

} // namespace

// Reads a classic ZIP32 member using the central directory. NumPy's .npz output
// uses stored or raw-DEFLATE members; ZIP64 and encrypted archives are rejected.
std::vector<uint8_t> read_npz_member_payload(const std::string &npz_path,
                                             const std::string &member_name) {
    const auto zip = read_file(npz_path);
    constexpr uint32_t eocd_signature = 0x06054b50U;
    constexpr uint32_t central_signature = 0x02014b50U;
    constexpr uint32_t local_signature = 0x04034b50U;

    const std::size_t search_start = zip.size() > 65'557 ? zip.size() - 65'557 : 0;
    std::size_t eocd = zip.size();
    if (zip.size() >= 22) {
        for (std::size_t pos = zip.size() - 22;; --pos) {
            if (read_u32(zip, pos) == eocd_signature) {
                eocd = pos;
                break;
            }
            if (pos == search_start)
                break;
        }
    }
    if (eocd == zip.size())
        throw std::runtime_error("npz central directory not found");

    const auto entry_count = read_u16(zip, eocd + 10);
    std::size_t cursor = read_u32(zip, eocd + 16);
    for (uint16_t entry = 0; entry < entry_count; ++entry) {
        if (read_u32(zip, cursor) != central_signature) {
            throw std::runtime_error("invalid npz central directory");
        }
        const auto flags = read_u16(zip, cursor + 8);
        const auto method = read_u16(zip, cursor + 10);
        const auto compressed_size = read_u32(zip, cursor + 20);
        const auto uncompressed_size = read_u32(zip, cursor + 24);
        const auto name_size = read_u16(zip, cursor + 28);
        const auto extra_size = read_u16(zip, cursor + 30);
        const auto comment_size = read_u16(zip, cursor + 32);
        const auto local_offset = read_u32(zip, cursor + 42);
        if (cursor + 46 + name_size > zip.size()) {
            throw std::runtime_error("truncated npz central directory entry");
        }
        const std::string name(reinterpret_cast<const char *>(zip.data() + cursor + 46), name_size);
        if (name == member_name) {
            if ((flags & 0x1U) != 0)
                throw std::runtime_error("encrypted npz is unsupported");
            if (read_u32(zip, local_offset) != local_signature) {
                throw std::runtime_error("invalid npz local header");
            }
            const auto local_name_size = read_u16(zip, local_offset + 26);
            const auto local_extra_size = read_u16(zip, local_offset + 28);
            const std::size_t data_offset = local_offset + 30 + local_name_size + local_extra_size;
            if (data_offset + compressed_size > zip.size()) {
                throw std::runtime_error("truncated npz member");
            }
            std::vector<uint8_t> raw;
            if (method == 0) {
                raw.assign(zip.begin() + static_cast<std::ptrdiff_t>(data_offset),
                           zip.begin() +
                               static_cast<std::ptrdiff_t>(data_offset + compressed_size));
            } else if (method == 8) {
                raw = inflate_raw(zip.data() + data_offset, compressed_size, uncompressed_size);
            } else {
                throw std::runtime_error("unsupported npz compression method");
            }
            const auto header = parse_npy_header(raw.data(), raw.size());
            return {raw.begin() + static_cast<std::ptrdiff_t>(header.data_offset), raw.end()};
        }
        cursor += 46 + name_size + extra_size + comment_size;
    }
    throw std::runtime_error("member not found in npz: " + member_name);
}

} // namespace hbt
