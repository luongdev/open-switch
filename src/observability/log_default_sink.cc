/*
 * src/observability/log_default_sink.cc
 *
 * The osw::log default sink — forwards into FreeSWITCH's
 * switch_log_printf (FF-012). Lives in its own translation unit so
 * log.cc can be compiled into unit-test binaries WITHOUT pulling in
 * <switch.h>. The test binaries do not link this file; they install
 * their own sink via SetSinkForTesting().
 *
 * Production wiring: mod_open_switch.cc calls
 * osw::log::InstallDefaultSinkForModule() during module load, which
 * passes the DefaultSink function-pointer to log.cc's internal slot.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <cstdio>
#include <string_view>

#include <switch.h>  // FF-012: switch_log_printf + log levels

#include "osw/observability/log.h"

namespace osw::log {

namespace {

// Maps our internal Level enum to FreeSWITCH's switch_log_level_t.
// FS levels are: SWITCH_LOG_DEBUG, INFO, NOTICE, WARNING, ERROR, CRIT,
// ALERT (see src/include/switch_log.h in v1.10.12). We collapse Trace
// onto DEBUG since FS lacks a separate trace level.
switch_log_level_t MapLevel(Level lvl) noexcept {
    switch (lvl) {
        case Level::kTrace:
            return SWITCH_LOG_DEBUG;
        case Level::kDebug:
            return SWITCH_LOG_DEBUG;
        case Level::kInfo:
            return SWITCH_LOG_INFO;
        case Level::kWarn:
            return SWITCH_LOG_WARNING;
        case Level::kError:
            return SWITCH_LOG_ERROR;
        case Level::kCritical:
            return SWITCH_LOG_CRIT;
    }
    return SWITCH_LOG_INFO;
}

// FF-012: switch_log_printf takes a printf format + args; the caller's
// userdata is already PII-redacted and prebuilt by log.cc::Logv, so we
// emit it via the "%s" format slot — NEVER pass user-controlled text
// as the format argument.
void DefaultSink(Level level,
                 std::string_view subsystem,
                 std::string_view traceparent,
                 std::string_view message) noexcept {
    char line[2048];
    int written;
    if (!traceparent.empty()) {
        written = std::snprintf(line,
                                sizeof(line),
                                "[osw:%.*s tp=%.*s] %.*s",
                                static_cast<int>(subsystem.size()),
                                subsystem.data(),
                                static_cast<int>(traceparent.size()),
                                traceparent.data(),
                                static_cast<int>(message.size()),
                                message.data());
    } else {
        written = std::snprintf(line,
                                sizeof(line),
                                "[osw:%.*s] %.*s",
                                static_cast<int>(subsystem.size()),
                                subsystem.data(),
                                static_cast<int>(message.size()),
                                message.data());
    }
    if (written < 0) {
        return;
    }

    // FF-012: switch_log_printf is thread-safe. We pass the module name
    // as `file`/`func` because we lack the original caller's
    // __FILE__/__LINE__ in the wrapper — a future refinement can plumb
    // those through via a macro at every osw::log::* call site.
    switch_log_printf(SWITCH_CHANNEL_LOG,
                      "mod_open_switch",
                      "osw_log_emit",
                      0,
                      nullptr,
                      MapLevel(level),
                      "%s\n",
                      line);
}

}  // namespace

// Hook implemented in log.cc; we declare it here with C linkage to
// avoid a transitive header dependency.
extern "C" void osw_log_install_default_sink(SinkFn sink) noexcept;

// Call this once during mod_open_switch_load() to wire the default
// sink into log.cc's internal slot. Idempotent: subsequent calls
// re-install the same sink.
void InstallDefaultSinkForModule() noexcept {
    osw_log_install_default_sink(&DefaultSink);
}

}  // namespace osw::log
