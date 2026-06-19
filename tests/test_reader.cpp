// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "hbt/data/reader.hpp"
#include "hbt/types.hpp"

using namespace hbt;

static Event reader_event(int64_t ts) {
    Event event{};
    event.ev = LOCAL_EVENT | EXCH_EVENT | BUY_EVENT | DEPTH_EVENT;
    event.exch_ts = ts;
    event.local_ts = ts;
    event.px = 100.0;
    event.qty = 1.0;
    return event;
}

TEST(EventReaderCache, SharesAndReleasesShardsAfterBothReadersFinish) {
    auto cache =
        EventReaderCache::from_chunks({{reader_event(1)}, {reader_event(2)}, {reader_event(3)}}, 2);
    EventReader local(cache);
    EventReader exchange(cache);

    auto local_first = local.next_chunk();
    auto exchange_first = exchange.next_chunk();
    ASSERT_TRUE(local_first);
    ASSERT_TRUE(exchange_first);
    EXPECT_EQ(local_first.get(), exchange_first.get());
    EXPECT_LE(cache->resident_shards(), 2u);

    auto local_second = local.next_chunk();
    ASSERT_TRUE(local_second);
    EXPECT_EQ(cache->released_shards(), 0u);

    auto exchange_second = exchange.next_chunk();
    ASSERT_TRUE(exchange_second);
    EXPECT_EQ(cache->released_shards(), 1u);
    EXPECT_EQ(local_second.get(), exchange_second.get());
}

TEST(EventReaderCache, ResolvesOrderedShardsFromConversionManifest) {
    const auto root = std::filesystem::temp_directory_path() / "hbt-reader-manifest-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto manifest = root / "conversion.manifest.json";
    {
        std::ofstream output(manifest);
        output << R"({"source_chunks":[{"data_file":"raw.jsonl.zst"}],)"
                  R"("shards":[{"file":"events-000000.npz"},{"file":"events-000001.npz"}]})";
    }

    const auto paths = event_files_from_manifest(manifest.string());

    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(paths[0], (root / "events-000000.npz").string());
    EXPECT_EQ(paths[1], (root / "events-000001.npz").string());
    std::filesystem::remove_all(root);
}
