/*
 * include/osw/media/bug_fanout.h
 *
 * osw::media::BugFanout - pure C++ fan-out router for W7 multi-target
 * bot WRITE_REPLACE playout. One inbound AudioFrame stream is copied into
 * per-target bounded queues. FreeSWITCH-facing callbacks pop from their
 * target queue and passthrough when Pop() returns std::nullopt.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_MEDIA_BUG_FANOUT_H_
#define OSW_MEDIA_BUG_FANOUT_H_

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "osw/media/audio_frame.h"

namespace osw::media {

/// Per-target queue guarded for one producer (gRPC reader) and one consumer
/// (that target's WRITE_REPLACE media thread).
struct TargetQueue {
    std::string channel_uuid;
    std::deque<AudioFrame> queue;
    mutable std::mutex mu_;
};

/// Routes one inbound stream of AudioFrames to N per-target queues.
/// Broadcast sends every frame to every target; whisper sends only to the
/// configured write subset.
class BugFanout {
  public:
    enum class Mode { kBroadcast, kWhisper };

    struct Config {
        Mode mode = Mode::kBroadcast;
        std::vector<std::string> target_uuids;
        std::vector<std::string> write_subset_uuids;
        std::uint32_t capacity_frames = 0;
    };

    explicit BugFanout(Config cfg);
    ~BugFanout() noexcept;

    BugFanout(const BugFanout&) = delete;
    BugFanout& operator=(const BugFanout&) = delete;
    BugFanout(BugFanout&&) = delete;
    BugFanout& operator=(BugFanout&&) = delete;

    /// Push one PCM frame from the gRPC reader. The frame is copied into each
    /// routed target queue. If a target queue exceeds capacity, oldest frames
    /// are dropped until the queue is back within bounds. Returns the number
    /// of per-target frame drops caused by this push.
    std::uint64_t Push(AudioFrame frame) noexcept;

    /// Pop one frame from a target queue. Returns std::nullopt when the target
    /// is unknown or its queue is empty.
    [[nodiscard]] std::optional<AudioFrame> Pop(std::string_view channel_uuid) noexcept;

    /// Stop accepting new pushes. Existing queued frames remain drainable.
    void HalfClose() noexcept;

    [[nodiscard]] std::uint64_t TotalDropped() const noexcept;
    [[nodiscard]] std::uint64_t QueueDepth(std::string_view channel_uuid) const noexcept;

  private:
    [[nodiscard]] TargetQueue* FindQueue(std::string_view channel_uuid) const noexcept;

    Config cfg_;
    std::vector<std::unique_ptr<TargetQueue>> queues_;
    std::unordered_map<std::string, TargetQueue*> by_uuid_;
    std::vector<TargetQueue*> write_queues_;
    std::atomic<bool> half_closed_{false};
    std::atomic<std::uint64_t> total_dropped_{0};
};

}  // namespace osw::media

#endif  // OSW_MEDIA_BUG_FANOUT_H_
