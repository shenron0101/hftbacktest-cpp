// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hbt/data/npy.hpp"
#include "hbt/types.hpp"

namespace hbt {

inline std::vector<std::string> event_files_from_manifest(const std::string &manifest_path) {
    std::ifstream input(manifest_path);
    if (!input) {
        throw std::runtime_error("cannot open conversion manifest: " + manifest_path);
    }
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    const auto shards_position = content.find("\"shards\"");
    if (shards_position == std::string::npos) {
        throw std::runtime_error("conversion manifest has no shards array");
    }

    const std::regex file_pattern(R"re("file"\s*:\s*"([^"]+\.npz)")re");
    const std::string shard_section = content.substr(shards_position);
    const std::filesystem::path root = std::filesystem::path(manifest_path).parent_path();
    std::vector<std::string> paths;
    for (std::sregex_iterator it(shard_section.begin(), shard_section.end(), file_pattern);
         it != std::sregex_iterator(); ++it) {
        paths.push_back((root / (*it)[1].str()).string());
    }
    if (paths.empty()) {
        throw std::runtime_error("conversion manifest contains no NPZ shards");
    }
    return paths;
}

class EventReaderCache {
  public:
    explicit EventReaderCache(std::vector<std::string> paths, std::size_t consumer_count = 2)
        : paths_(std::move(paths)), consumer_count_(consumer_count) {
        if (consumer_count_ == 0) {
            throw std::invalid_argument("consumer_count must be positive");
        }
    }

    static std::shared_ptr<EventReaderCache> from_chunks(std::vector<std::vector<Event>> chunks,
                                                         std::size_t consumer_count = 2) {
        auto cache = std::shared_ptr<EventReaderCache>(new EventReaderCache({}, consumer_count));
        cache->memory_chunks_ = std::move(chunks);
        return cache;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return memory_chunks_.empty() ? paths_.size() : memory_chunks_.size();
    }

    std::shared_ptr<const std::vector<Event>> acquire(std::size_t index) {
        if (index >= size())
            return {};
        ensure_loaded(index);
        if (index + 1 < size())
            ensure_loaded(index + 1);
        return cache_.at(index).data;
    }

    void release(std::size_t index) {
        auto found = cache_.find(index);
        if (found == cache_.end())
            return;
        ++found->second.finished_consumers;
        if (found->second.finished_consumers >= consumer_count_) {
            cache_.erase(found);
            ++released_shards_;
        }
    }

    [[nodiscard]] std::size_t resident_shards() const noexcept { return cache_.size(); }

    [[nodiscard]] std::size_t released_shards() const noexcept { return released_shards_; }

    [[nodiscard]] std::size_t load_count() const noexcept { return load_count_; }

  private:
    struct CachedShard {
        std::shared_ptr<const std::vector<Event>> data;
        std::size_t finished_consumers = 0;
    };

    void ensure_loaded(std::size_t index) {
        if (cache_.contains(index))
            return;
        std::vector<Event> events;
        if (!memory_chunks_.empty()) {
            events = memory_chunks_.at(index);
        } else {
            events = load_events(paths_.at(index));
        }
        cache_.emplace(index, CachedShard{
                                  std::make_shared<const std::vector<Event>>(std::move(events)),
                                  0,
                              });
        ++load_count_;
    }

    std::vector<std::string> paths_;
    std::vector<std::vector<Event>> memory_chunks_;
    std::size_t consumer_count_;
    std::unordered_map<std::size_t, CachedShard> cache_;
    std::size_t released_shards_ = 0;
    std::size_t load_count_ = 0;
};

class EventReader {
  public:
    explicit EventReader(std::shared_ptr<EventReaderCache> cache) : cache_(std::move(cache)) {
        if (!cache_)
            throw std::invalid_argument("event reader cache is null");
    }

    EventReader(const EventReader &) = delete;
    EventReader &operator=(const EventReader &) = delete;
    EventReader(EventReader &&) = delete;
    EventReader &operator=(EventReader &&) = delete;

    ~EventReader() { release_current(); }

    std::shared_ptr<const std::vector<Event>> next_chunk() {
        release_current();
        if (next_index_ >= cache_->size())
            return {};
        current_index_ = next_index_++;
        current_ = cache_->acquire(*current_index_);
        return current_;
    }

  private:
    void release_current() {
        if (current_index_) {
            cache_->release(*current_index_);
            current_index_.reset();
            current_.reset();
        }
    }

    std::shared_ptr<EventReaderCache> cache_;
    std::shared_ptr<const std::vector<Event>> current_;
    std::optional<std::size_t> current_index_;
    std::size_t next_index_ = 0;
};

inline std::shared_ptr<EventReaderCache> event_reader_from_files(std::vector<std::string> paths) {
    if (paths.empty())
        throw std::invalid_argument("event file list is empty");
    return std::make_shared<EventReaderCache>(std::move(paths), 2);
}

} // namespace hbt
