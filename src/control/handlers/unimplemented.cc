/*
 * src/control/handlers/unimplemented.cc
 *
 * Two responsibilities in one TU:
 *   1. `osw::control::handlers::Unimplemented(method, wave)` —
 *      uniform UNIMPLEMENTED grpc::Status builder used by every stub
 *      RPC and reusable from any future handler that wants the same
 *      shape.
 *   2. Method-override bodies for every RPC of ControlServiceSkeleton
 *      EXCEPT:
 *        - Health (real impl lives in health_handler.cc)
 *        - SubscribeEvents (real impl lives in subscribe_events_handler.cc)
 *        - Originate (real impl lives in originate_handler.cc)             [W3A]
 *        - Hangup (real impl lives in hangup_handler.cc)                   [W3A]
 *        - HangupMany (real impl lives in hangup_many_handler.cc)          [W3A]
 *        - Bridge (real impl lives in bridge_handler.cc)                   [W3B]
 *        - Execute (real impl lives in execute_handler.cc)                 [W3B]
 *        - BlindTransfer (real impl lives in blind_transfer_handler.cc)    [W3B]
 *        - SetVariables (real impl lives in set_variables_handler.cc)      [W3C]
 *        - Hold (real impl lives in hold_handler.cc)                       [W3C]
 *        - Unhold (real impl lives in unhold_handler.cc)                   [W3C]
 *        - StartTts (real impl lives in start_tts_handler.cc)              [W6C]
 *        - StartStt (real impl lives in start_stt_handler.cc)              [W6C]
 *        - StartVoicebot (real impl lives in start_voicebot_handler.cc)    [W6C]
 *        - StopMediaStream (real impl lives in stop_media_stream_handler.cc) [W6C]
 *
 * After W6C lands, this TU carries ONLY the Unimplemented() helper.
 * No stub method bodies remain. Future RPCs added to ControlServiceSkeleton
 * (V2 node lifecycle, etc.) can be stubbed here until their real handler
 * lands.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/handlers/unimplemented.h"

#include <sstream>
#include <string>

#include "osw/observability/log.h"

#include "src/control/control_service_skeleton.h"

namespace osw::control::handlers {

grpc::Status Unimplemented(std::string_view method, std::string_view wave) {
    std::ostringstream oss;
    oss << "not yet implemented (wave " << wave << " coming)";
    const std::string msg = oss.str();
    osw::log::Debug("control",
                    "%.*s: UNIMPLEMENTED (wave %.*s)",
                    static_cast<int>(method.size()),
                    method.data(),
                    static_cast<int>(wave.size()),
                    wave.data());
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, msg);
}

}  // namespace osw::control::handlers

// No stub method bodies remain after W6C. StartTts/StartStt/StartVoicebot/
// StopMediaStream (W6C) moved to their dedicated handler TUs;
// Bridge/Execute/BlindTransfer (W3B) and SetVariables/Hold/Unhold (W3C)
// moved earlier; SubscribeEvents (W2) lives in subscribe_events_handler.cc;
// Originate/Hangup/HangupMany (W3A) in their dedicated TUs; Health (W1)
// in health_handler.cc.
