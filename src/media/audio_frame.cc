/*
 * src/media/audio_frame.cc
 *
 * Implementation of osw::media::AudioFrame.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/media/audio_frame.h"

#include <cstring>

#include "open_switch/media/v1/media.pb.h"

namespace osw::media {

AudioFrame::AudioFrame(std::vector<std::int16_t> samples,
                       std::uint32_t sample_rate_hz,
                       std::uint32_t channels,
                       std::uint64_t seq,
                       std::uint64_t timestamp_samples,
                       std::uint32_t channel,
                       std::string channel_uuid) noexcept
    : samples_(std::move(samples)),
      sample_rate_hz_(sample_rate_hz),
      channels_(channels),
      channel_(channel),
      channel_uuid_(std::move(channel_uuid)),
      seq_(seq),
      timestamp_samples_(timestamp_samples) {}

std::uint32_t AudioFrame::duration_ms() const noexcept {
    if (sample_rate_hz_ == 0 || channels_ == 0) {
        return 0;
    }
    const std::uint64_t dur = duration_samples();
    // avoid integer overflow: dur × 1000 / sample_rate_hz
    return static_cast<std::uint32_t>((dur * 1000u) / sample_rate_hz_);
}

// ---------------------------------------------------------------------------
// FromProto
// ---------------------------------------------------------------------------

std::optional<AudioFrame> AudioFrame::FromProto(const open_switch::media::v1::AudioFrame& proto,
                                                std::uint32_t sample_rate_hz,
                                                std::uint32_t channels) noexcept {
    // The proto carries duration_samples (number of samples per channel).
    // Expected payload length in bytes = duration_samples × channels × 2.
    const std::size_t expected_bytes = static_cast<std::size_t>(proto.duration_samples()) *
                                       static_cast<std::size_t>(channels) * sizeof(std::int16_t);

    if (proto.payload().size() != expected_bytes) {
        return std::nullopt;
    }

    const std::size_t total_samples =
        static_cast<std::size_t>(proto.duration_samples()) * static_cast<std::size_t>(channels);

    std::vector<std::int16_t> samples(total_samples);
    std::memcpy(samples.data(), proto.payload().data(), proto.payload().size());

    return AudioFrame(
        std::move(samples),
        sample_rate_hz,
        channels,
        proto.seq(),
        proto.timestamp_samples(),
        static_cast<std::uint32_t>(proto.channel()),
        proto.channel_uuid());
}

// ---------------------------------------------------------------------------
// ToProto
// ---------------------------------------------------------------------------

void AudioFrame::ToProto(open_switch::media::v1::AudioFrame* out) const noexcept {
    out->set_seq(seq_);
    out->set_timestamp_samples(timestamp_samples_);
    out->set_duration_samples(duration_samples());
    out->set_channel(static_cast<open_switch::media::v1::AudioFrame::Channel>(channel_));
    out->set_channel_uuid(channel_uuid_);

    const std::size_t byte_count = samples_.size() * sizeof(std::int16_t);
    out->set_payload(reinterpret_cast<const char*>(samples_.data()), byte_count);
}

}  // namespace osw::media
