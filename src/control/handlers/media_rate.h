/*
 * src/control/handlers/media_rate.h
 *
 * Handler-private helpers for validating FreeSWITCH media rates before
 * attaching bugs. If the channel rate differs from the stream rate, V1 only
 * supports 8 kHz <-> 16 kHz resampling.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_HANDLERS_MEDIA_RATE_H_
#define OSW_CONTROL_HANDLERS_MEDIA_RATE_H_

#include <cstdint>
#include <string>

#include <grpcpp/grpcpp.h>

#include "osw/media/resampler.h"
#include "osw/raii/fs_api.h"

namespace osw::control::handlers {

struct SessionMediaRate {
    std::uint32_t sample_rate_hz = 0;
    std::uint32_t channels = 1;
};

inline SessionMediaRate ReadMediaRate(switch_core_session_t* session) noexcept {
    switch_codec_implementation_t impl{};
    if (::osw::raii::fs::SessionGetReadImpl(session, &impl) != SWITCH_STATUS_SUCCESS) {
        return {};
    }
    return {impl.actual_samples_per_second != 0 ? impl.actual_samples_per_second
                                                : impl.samples_per_second,
            impl.number_of_channels != 0 ? impl.number_of_channels : 1u};
}

inline SessionMediaRate WriteMediaRate(switch_core_session_t* session) noexcept {
    switch_codec_implementation_t impl{};
    if (::osw::raii::fs::SessionGetWriteImpl(session, &impl) != SWITCH_STATUS_SUCCESS) {
        return {};
    }
    return {impl.actual_samples_per_second != 0 ? impl.actual_samples_per_second
                                                : impl.samples_per_second,
            impl.number_of_channels != 0 ? impl.number_of_channels : 1u};
}

inline grpc::Status ValidateRatePair(const char* label,
                                     std::uint32_t from_hz,
                                     std::uint32_t to_hz) {
    if (from_hz == 0 || to_hz == 0) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            std::string(label) + " media rate unavailable");
    }
    if (!::osw::media::Resampler::Supports(static_cast<int>(from_hz), static_cast<int>(to_hz))) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            std::string(label) + " unsupported media rate pair " +
                                std::to_string(from_hz) + "->" + std::to_string(to_hz));
    }
    return grpc::Status::OK;
}

inline grpc::Status ValidateReadStreamRate(switch_core_session_t* session,
                                           std::uint32_t stream_rate_hz,
                                           const char* label) {
    return ValidateRatePair(label, ReadMediaRate(session).sample_rate_hz, stream_rate_hz);
}

inline grpc::Status ValidateWriteStreamRate(switch_core_session_t* session,
                                            std::uint32_t stream_rate_hz,
                                            const char* label) {
    return ValidateRatePair(label, stream_rate_hz, WriteMediaRate(session).sample_rate_hz);
}

inline grpc::Status ValidateRecordingWriteRate(switch_core_session_t* session,
                                               std::uint32_t stream_rate_hz,
                                               const char* label) {
    return ValidateRatePair(label, WriteMediaRate(session).sample_rate_hz, stream_rate_hz);
}

}  // namespace osw::control::handlers

#endif  // OSW_CONTROL_HANDLERS_MEDIA_RATE_H_
