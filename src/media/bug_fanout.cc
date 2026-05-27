/*
 * src/media/bug_fanout.cc
 *
 * osw::media::BugFanout implementation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/media/bug_fanout.h"

#include <cstddef>
#include <utility>

namespace osw::media {

BugFanout::BugFanout(Config cfg) : cfg_(std::move(cfg)) {
    queues_.reserve(cfg_.target_uuids.size());
    by_uuid_.reserve(cfg_.target_uuids.size());

    for (const auto& channel_uuid : cfg_.target_uuids) {
        auto queue = std::make_unique<TargetQueue>();
        queue->channel_uuid = channel_uuid;
        TargetQueue* ptr = queue.get();
        queues_.push_back(std::move(queue));
        by_uuid_[ptr->channel_uuid] = ptr;
    }

    if (cfg_.mode == Mode::kBroadcast) {
        write_queues_.reserve(queues_.size());
        for (const auto& queue : queues_) {
            write_queues_.push_back(queue.get());
        }
        return;
    }

    write_queues_.reserve(cfg_.write_subset_uuids.size());
    for (const auto& channel_uuid : cfg_.write_subset_uuids) {
        TargetQueue* queue = FindQueue(channel_uuid);
        if (queue != nullptr) {
            write_queues_.push_back(queue);
        }
    }
}

BugFanout::~BugFanout() noexcept = default;

std::uint64_t BugFanout::Push(AudioFrame frame) noexcept {
    if (half_closed_.load(std::memory_order_acquire)) {
        return 0;
    }

    std::size_t last_target_index = write_queues_.size();
    for (std::size_t i = write_queues_.size(); i > 0; --i) {
        if (write_queues_[i - 1] != nullptr) {
            last_target_index = i - 1;
            break;
        }
    }
    if (last_target_index == write_queues_.size()) {
        return 0;
    }

    std::uint64_t dropped = 0;
    for (std::size_t i = 0; i < write_queues_.size(); ++i) {
        TargetQueue* target = write_queues_[i];
        if (target == nullptr) {
            continue;
        }

        std::lock_guard<std::mutex> lock(target->mu_);
        if (i == last_target_index) {
            target->queue.push_back(std::move(frame));
        } else {
            target->queue.push_back(frame);
        }
        while (target->queue.size() > cfg_.capacity_frames) {
            target->queue.pop_front();
            ++dropped;
        }
    }

    if (dropped > 0) {
        total_dropped_.fetch_add(dropped, std::memory_order_relaxed);
    }

    return dropped;
}

std::optional<AudioFrame> BugFanout::Pop(std::string_view channel_uuid) noexcept {
    TargetQueue* target = FindQueue(channel_uuid);
    if (target == nullptr) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(target->mu_);
    if (target->queue.empty()) {
        return std::nullopt;
    }

    AudioFrame frame = std::move(target->queue.front());
    target->queue.pop_front();
    return frame;
}

void BugFanout::HalfClose() noexcept {
    half_closed_.store(true, std::memory_order_release);
}

std::uint64_t BugFanout::TotalDropped() const noexcept {
    return total_dropped_.load(std::memory_order_relaxed);
}

std::uint64_t BugFanout::QueueDepth(std::string_view channel_uuid) const noexcept {
    TargetQueue* target = FindQueue(channel_uuid);
    if (target == nullptr) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(target->mu_);
    return target->queue.size();
}

TargetQueue* BugFanout::FindQueue(std::string_view channel_uuid) const noexcept {
    for (const auto& queue : queues_) {
        if (std::string_view(queue->channel_uuid) == channel_uuid) {
            return queue.get();
        }
    }
    return nullptr;
}

}  // namespace osw::media
