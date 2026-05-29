/*
 * include/osw/media/audio_frame.h
 *
 * osw::media::AudioFrame — owning, move-friendly value type for L16
 * PCM audio exchanged between FreeSWITCH media bugs and the gRPC media
 * plane. No FreeSWITCH dependency; this lives in osw_media (FS-agnostic).
 *
 * Design (W6 Track B):
 *   - Owns the sample buffer as std::vector<int16_t>. Cheap to move;
 *     copying duplicates the buffer (callers should move when possible).
 *   - ToProto / FromProto perform the wire ↔ in-memory conversion.
 *     FromProto validates payload size against channels × duration_samples
 *     and returns nullopt on mismatch.
 *   - Codec is always PCM_S16LE in V1. The AudioCodec field in the proto
 *     is set by ToProto and asserted by the caller on FromProto.
 *
 * See FREESWITCH-FACTS FF-034 for the gRPC bidi-stream lifetime contract.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_MEDIA_AUDIO_FRAME_H_
#define OSW_MEDIA_AUDIO_FRAME_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Forward-declare the proto type so callers that only need the in-process
// type can include this header without pulling in the generated pb headers.
namespace open_switch::media::v1 {
class AudioFrame;
}  // namespace open_switch::media::v1

namespace osw::media {

/// Owning view over L16 PCM samples + metadata. Cheap to move; copying
/// duplicates the sample buffer (callers should move when possible).
class AudioFrame {
  public:
    AudioFrame() noexcept = default;
    AudioFrame(std::vector<std::int16_t> samples,
               std::uint32_t sample_rate_hz,
               std::uint32_t channels,
               std::uint64_t seq,
               std::uint64_t timestamp_samples,
               std::uint32_t channel = 0,
               std::string channel_uuid = {}) noexcept;

    AudioFrame(const AudioFrame&) = default;
    AudioFrame(AudioFrame&&) noexcept = default;
    AudioFrame& operator=(const AudioFrame&) = default;
    AudioFrame& operator=(AudioFrame&&) noexcept = default;

    [[nodiscard]] const std::int16_t* data() const noexcept { return samples_.data(); }
    [[nodiscard]] std::int16_t* data() noexcept { return samples_.data(); }
    [[nodiscard]] std::size_t sample_count() const noexcept { return samples_.size(); }
    [[nodiscard]] std::uint32_t sample_rate_hz() const noexcept { return sample_rate_hz_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::uint32_t channel() const noexcept { return channel_; }
    [[nodiscard]] const std::string& channel_uuid() const noexcept { return channel_uuid_; }
    [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }
    [[nodiscard]] std::uint64_t timestamp_samples() const noexcept { return timestamp_samples_; }

    /// Number of samples per channel.  For a 160-sample mono frame: 160.
    /// For a 320-sample stereo frame (channels=2): 160.
    [[nodiscard]] std::uint32_t duration_samples() const noexcept {
        return channels_ > 0 ? static_cast<std::uint32_t>(samples_.size() / channels_) : 0;
    }

    /// Frame duration in milliseconds: duration_samples / sample_rate_hz * 1000.
    /// Returns 0 when sample_rate_hz is 0.
    [[nodiscard]] std::uint32_t duration_ms() const noexcept;

    /// Construct from a wire AudioFrame proto.
    /// Returns std::nullopt if the payload size does not equal
    ///   duration_samples_from_proto × channels × sizeof(int16_t).
    /// `sample_rate_hz` and `channels` are caller-supplied (they come from
    /// StreamStart, not from each AudioFrame message).
    static std::optional<AudioFrame> FromProto(const open_switch::media::v1::AudioFrame& proto,
                                               std::uint32_t sample_rate_hz,
                                               std::uint32_t channels) noexcept;

    /// Serialize into a wire proto. Codec is always PCM_S16LE in V1.
    /// The caller is responsible for the lifetime of `out`.
    void ToProto(open_switch::media::v1::AudioFrame* out) const noexcept;

  private:
    std::vector<std::int16_t> samples_;
    std::uint32_t sample_rate_hz_ = 0;
    std::uint32_t channels_ = 1;
    std::uint32_t channel_ = 0;
    std::string channel_uuid_;
    std::uint64_t seq_ = 0;
    std::uint64_t timestamp_samples_ = 0;
};

}  // namespace osw::media

#endif  // OSW_MEDIA_AUDIO_FRAME_H_
