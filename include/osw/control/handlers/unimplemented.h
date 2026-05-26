/*
 * src/control/handlers/unimplemented.h
 *
 * Shared helper used by ControlServiceSkeleton's stub methods to
 * return a uniform UNIMPLEMENTED status with a "wave WN coming"
 * message. Defined in unimplemented.cc.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_HANDLERS_UNIMPLEMENTED_H_
#define OSW_CONTROL_HANDLERS_UNIMPLEMENTED_H_

#include <grpcpp/grpcpp.h>
#include <string_view>

namespace osw::control::handlers {

/// Returns a grpc::Status with code UNIMPLEMENTED and the message
///   "not yet implemented (wave W<wave> coming)"
/// `method` and `wave` are baked into the message for log-side
/// diagnostics.
grpc::Status Unimplemented(std::string_view method, std::string_view wave);

}  // namespace osw::control::handlers

#endif  // OSW_CONTROL_HANDLERS_UNIMPLEMENTED_H_
