/*
 * src/control/originate_options.cc
 *
 * osw::control::OriginateOptions builder implementation.
 *
 * Dial-string assembly rules (V1, synchronous unattended originate):
 *
 *   Single endpoint   → dial string = endpoint as-is.
 *   Multiple endpoints:
 *     SIMULTANEOUS    → comma-separated  (e.g. "A,B")
 *     FAILOVER        → pipe-separated   (e.g. "A|B")
 *     MULTIPLE        → colon+underscore (e.g. "A:_:B")
 *     UNSPECIFIED     → treated as FAILOVER (safe default)
 *
 * Channel variables are pushed into a SWITCH_EVENT_GENERAL event (the
 * ovars mechanism — FF-021). switch_ivr_originate iterates the headers
 * of the supplied ovars event and sets them as channel variables on
 * the new outbound leg.
 *
 * Cited FACTs: FF-021.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/control/originate_options.h"

#include <cstdint>
#include <string>
#include <utility>

#include "osw/observability/log.h"

namespace osw::control {

namespace {

constexpr uint32_t kDefaultTimeoutSec = 60;
constexpr const char* kSubsystem = "control.originate_options";

// Build the dial string from the endpoint list + strategy.
[[nodiscard]] std::string BuildDialString(const open_switch::control::v1::OriginateRequest& req) {
    if (req.endpoints_size() == 0) {
        return {};
    }
    if (req.endpoints_size() == 1) {
        return req.endpoints(0);
    }

    const char* sep = nullptr;
    using Req = open_switch::control::v1::OriginateRequest;
    switch (req.strategy()) {
        case Req::SIMULTANEOUS:
            sep = ",";
            break;
        case Req::MULTIPLE:
            sep = ":_:";
            break;
        case Req::FAILOVER:
        case Req::STRATEGY_UNSPECIFIED:
        default:
            sep = "|";
            break;
    }

    std::string ds;
    ds.reserve(256);
    for (int i = 0; i < req.endpoints_size(); ++i) {
        if (i > 0) {
            ds += sep;
        }
        ds += req.endpoints(i);
    }
    return ds;
}

}  // namespace

// static
OriginateOptions OriginateOptions::Build(
    const open_switch::control::v1::OriginateRequest& req) noexcept {
    OriginateOptions opts;

    // Validate endpoints.
    if (req.endpoints_size() == 0) {
        opts.error_ = "at least one endpoint is required";
        osw::log::Debug(kSubsystem, "Build failed: %s", opts.error_.c_str());
        return opts;
    }
    for (int i = 0; i < req.endpoints_size(); ++i) {
        if (req.endpoints(i).empty()) {
            opts.error_ = "endpoint at index " + std::to_string(i) + " is empty";
            osw::log::Debug(kSubsystem, "Build failed: %s", opts.error_.c_str());
            return opts;
        }
    }

    // Validate timeout.
    uint32_t timeout_sec = kDefaultTimeoutSec;
    if (req.has_timeout()) {
        const auto& dur = req.timeout();
        // Reject zero or negative.
        if (dur.seconds() <= 0 && dur.nanos() <= 0) {
            opts.error_ = "timeout must be > 0";
            osw::log::Debug(kSubsystem, "Build failed: %s", opts.error_.c_str());
            return opts;
        }
        timeout_sec = (dur.seconds() > 0) ? static_cast<uint32_t>(dur.seconds()) : 1u;
    }

    opts.dial_string_ = BuildDialString(req);
    opts.cid_name_ = req.caller_id_name();
    opts.cid_num_ = req.caller_id_number();
    opts.timeout_sec_ = timeout_sec;

    // Build ovars event if the request carries channel variables.
    if (!req.variables().empty()) {
        switch_event_t* ev = nullptr;
        if (osw::raii::fs::EventCreate(&ev, SWITCH_EVENT_GENERAL) != SWITCH_STATUS_SUCCESS ||
            ev == nullptr) {
            opts.error_ = "failed to allocate channel variables event";
            osw::log::Warn(kSubsystem, "Build failed: %s", opts.error_.c_str());
            return opts;
        }
        for (const auto& [k, v] : req.variables()) {
            osw::raii::fs::EventAddHeaderString(ev, SWITCH_STACK_BOTTOM, k.c_str(), v.c_str());
        }
        opts.ovars_ = ev;
    }

    opts.valid_ = true;
    return opts;
}

OriginateOptions::~OriginateOptions() noexcept {
    if (ovars_ != nullptr) {
        osw::raii::fs::EventDestroy(&ovars_);
    }
}

OriginateOptions::OriginateOptions(OriginateOptions&& other) noexcept
    : valid_(other.valid_),
      error_(std::move(other.error_)),
      dial_string_(std::move(other.dial_string_)),
      cid_name_(std::move(other.cid_name_)),
      cid_num_(std::move(other.cid_num_)),
      timeout_sec_(other.timeout_sec_),
      ovars_(other.ovars_) {
    other.valid_ = false;
    other.ovars_ = nullptr;
    other.timeout_sec_ = 0;
}

OriginateOptions& OriginateOptions::operator=(OriginateOptions&& other) noexcept {
    if (this != &other) {
        if (ovars_ != nullptr) {
            osw::raii::fs::EventDestroy(&ovars_);
        }
        valid_ = other.valid_;
        error_ = std::move(other.error_);
        dial_string_ = std::move(other.dial_string_);
        cid_name_ = std::move(other.cid_name_);
        cid_num_ = std::move(other.cid_num_);
        timeout_sec_ = other.timeout_sec_;
        ovars_ = other.ovars_;
        other.valid_ = false;
        other.ovars_ = nullptr;
        other.timeout_sec_ = 0;
    }
    return *this;
}

}  // namespace osw::control
