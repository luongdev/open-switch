/*
 * src/control/handlers/subscribe_events_stub.cc
 *
 * Minimal stub for ControlServiceSkeleton::SubscribeEvents used ONLY
 * by the W3A unit-test mock-seam library (osw_control_call_lifecycle_test_helpers).
 *
 * The W3A handler tests exercise Originate / Hangup / HangupMany and
 * need a complete vtable for ControlServiceSkeleton. SubscribeEvents
 * is provided by subscribe_events_handler.cc in the production build
 * (osw_control_fs), but that TU links the real FS library. This stub
 * provides the UNIMPLEMENTED path that satisfies the linker in mock
 * builds without pulling in FS or the event-plane subsystem.
 *
 * This file is compiled ONLY when OSW_BUILD_TESTS=ON as part of
 * osw_control_call_lifecycle_test_helpers. It MUST NOT be included in
 * osw_control_fs or osw_control.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.pb.h"
#include "open_switch/events/v1/events.pb.h"

#include "osw/control/handlers/unimplemented.h"

#include "src/control/control_service_skeleton.h"

namespace osw::control {

grpc::Status ControlServiceSkeleton::SubscribeEvents(
    grpc::ServerContext* /*ctx*/,
    const open_switch::control::v1::SubscribeEventsRequest* /*req*/,
    grpc::ServerWriter<open_switch::events::v1::EventEnvelope>* /*writer*/) {
    return handlers::Unimplemented("SubscribeEvents", "W2 (stub for W3A tests)");
}

}  // namespace osw::control
