/*
 * include/osw/media/recording_relay.h
 *
 * W7 Track B module-owned recording relay callbacks.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_MEDIA_RECORDING_RELAY_H_
#define OSW_MEDIA_RECORDING_RELAY_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "osw/media/resampler.h"
#include "osw/media/stereo_pairer.h"
#include "osw/media/stream_client.h"
#include "osw/raii/fs_api.h"

namespace osw::media {

struct RecordingRelayConfig {
    std::string channel_uuid;
    std::string tenant_id;
    std::string stream_id;
    bool stereo = false;
    std::uint32_t sample_rate_hz = 8000;
    std::uint32_t read_fs_rate_hz = 0;
    std::uint32_t write_fs_rate_hz = 0;
    std::uint32_t desync_warn_ms = 5;
    std::uint32_t desync_timeout_ms = 25;
};

class RecordingRelay {
  public:
    RecordingRelay(StreamClient* client, RecordingRelayConfig config);
    ~RecordingRelay() noexcept;

    RecordingRelay(const RecordingRelay&) = delete;
    RecordingRelay& operator=(const RecordingRelay&) = delete;
    RecordingRelay(RecordingRelay&&) = delete;
    RecordingRelay& operator=(RecordingRelay&&) = delete;

    void Start() noexcept;
    void Stop() noexcept;
    void EmitStopped() noexcept;
    [[nodiscard]] bool Stopped() const noexcept;

    void PushReadFrame(std::uint64_t fs_timestamp_samples,
                       std::uint32_t sample_rate_hz,
                       std::span<const std::int16_t> samples) noexcept;
    void PushWriteFrame(std::uint64_t fs_timestamp_samples,
                        std::uint32_t sample_rate_hz,
                        std::span<const std::int16_t> samples) noexcept;

  private:
    void PushSide(bool left,
                  std::uint64_t fs_timestamp_samples,
                  std::uint32_t sample_rate_hz,
                  std::span<const std::int16_t> samples) noexcept;
    void FlushMonoFrame(std::uint64_t fs_timestamp_samples,
                        std::uint32_t sample_rate_hz,
                        std::span<const std::int16_t> samples) noexcept;
    void TickLoop() noexcept;
    void FlushPairedFrame(PairedFrame paired) noexcept;
    [[nodiscard]] bool ResampleIfNeeded(bool left,
                                        std::uint32_t from_hz,
                                        std::span<const std::int16_t> in,
                                        std::vector<std::int16_t>* out) noexcept;
    void EmitRateLimited(const char* subclass,
                         std::chrono::steady_clock::time_point* last_emit) noexcept;

    StreamClient* client_ = nullptr;  // non-owning; ActiveMediaStream owns it.
    RecordingRelayConfig config_;
    StereoFramePairer pairer_;
    std::atomic<bool> stopped_{false};
    std::thread tick_thread_;
    std::atomic<bool> stopped_audit_emitted_{false};
    std::atomic<bool> desync_episode_active_{false};
    std::mutex emit_mu_;
    std::mutex resampler_mu_;
    std::unique_ptr<Resampler> read_resampler_;
    std::unique_ptr<Resampler> write_resampler_;
    bool read_resampler_error_logged_ = false;
    bool write_resampler_error_logged_ = false;
    std::chrono::steady_clock::time_point last_overflow_emit_{};
    std::chrono::steady_clock::time_point last_desync_emit_{};
};

}  // namespace osw::media

extern "C" {

switch_bool_t OswRecordingReadTap(switch_media_bug_t* bug,
                                  void* user_data,
                                  switch_abc_type_t type) noexcept;

switch_bool_t OswRecordingWriteTap(switch_media_bug_t* bug,
                                   void* user_data,
                                   switch_abc_type_t type) noexcept;

}  // extern "C"

#endif  // OSW_MEDIA_RECORDING_RELAY_H_
