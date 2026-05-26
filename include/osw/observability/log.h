/*
 * include/osw/observability/log.h
 *
 * osw::log — structured logging wrapper around switch_log_printf.
 *
 * Design (W1):
 *   - Five level functions: trace, debug, info, warn, error, critical.
 *   - Each accepts a printf-style format string. Bare user strings
 *     MUST go through "%s" — the caller's responsibility.
 *   - PII redaction: a configurable list of std::regex patterns is
 *     applied to the formatted message before it leaves our wrapper.
 *     Default list is empty. The list is set once at module load via
 *     osw::log::SetRedactionPatterns and is treated as effectively
 *     read-only thereafter (synchronisation is via a single
 *     atomic-snapshot std::shared_ptr).
 *   - Trace correlation: a thread-local std::string holds the current
 *     W3C traceparent. osw::log::TraceScope sets it for the duration
 *     of a scope; the formatted log line prefixes it when non-empty.
 *
 * Why thread_local: the FS event-dispatch pool has up to 64 threads
 * (FF-004). A thread can be handling events for many channels in
 * rotation. We do NOT want a global current_traceparent — that's
 * racy. We do NOT want a per-channel map — that's lookup cost on
 * every log line. Thread-local works because TraceScope is RAII-
 * scoped to the work-unit (one event, one RPC handler).
 *
 * Why an atomic-snapshot for redaction patterns rather than a
 * std::shared_mutex: redaction is on the hot path of every log
 * line; std::regex_search itself takes a const std::regex&, so we
 * can publish a shared_ptr<const vector<regex>> once at config
 * load and replace it atomically on SIGHUP reload (W4). Readers
 * load the shared_ptr with atomic semantics and iterate. Zero
 * lock contention on log emission.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_OBSERVABILITY_LOG_H_
#define OSW_OBSERVABILITY_LOG_H_

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace osw::log {

/// Log severity. Matches FreeSWITCH log levels by intent; the underlying
/// mapping to switch_log_level_t is in log.cc.
enum class Level : int {
    kTrace = 0,
    kDebug = 1,
    kInfo = 2,
    kWarn = 3,
    kError = 4,
    kCritical = 5,
};

/// The function the wrapper forwards to. Default implementation in
/// log.cc emits via switch_log_printf. Tests install their own sink
/// via `SetSinkForTesting`.
///
/// `subsystem` is a short tag like "core", "control", "events". Each
/// subsystem passes a stable string at its log call sites; in W1 we
/// pass it explicitly rather than building per-subsystem objects.
///
/// `traceparent` is the current TraceScope value or "" if none set.
///
/// `message` is the fully-formatted, PII-redacted message.
using SinkFn = void (*)(Level level,
                        std::string_view subsystem,
                        std::string_view traceparent,
                        std::string_view message) noexcept;

/// Replace the global sink for testing. Returns the previous sink so
/// tests can restore at tear-down. NOT thread-safe — tests are
/// single-threaded around this. Production code never calls this.
SinkFn SetSinkForTesting(SinkFn new_sink) noexcept;

/// Wires the default sink (the one that forwards to switch_log_printf)
/// into the log slot. Called exactly once by mod_open_switch_load.
/// Defined in log_default_sink.cc which includes <switch.h>; not linked
/// into unit-test binaries.
void InstallDefaultSinkForModule() noexcept;

/// Sets the PII redaction patterns. Empty list disables redaction.
/// May be called from any thread; the new list is published via
/// std::atomic<std::shared_ptr<>> so concurrent readers see either
/// the old or the new list, never a torn snapshot.
void SetRedactionPatterns(std::vector<std::regex> patterns);

/// Returns the count of compiled redaction patterns currently active.
/// Test helper.
std::size_t RedactionPatternCountForTesting();

/// Apply the current redaction patterns to `message` in-place,
/// replacing each match with "[REDACTED]". Exposed for testing the
/// redaction logic directly without going through the sink.
std::string ApplyRedactionForTesting(std::string_view message);

/// Thread-local W3C traceparent. Returns "" when no TraceScope is
/// active.
std::string_view CurrentTraceparent() noexcept;

/// RAII helper that sets the thread-local current traceparent for
/// the duration of its scope. Pushes/pops in stack order; nested
/// scopes are supported.
class TraceScope {
  public:
    explicit TraceScope(std::string traceparent);
    ~TraceScope() noexcept;
    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;
    TraceScope(TraceScope&&) = delete;
    TraceScope& operator=(TraceScope&&) = delete;

  private:
    std::string previous_;
};

// --- Emit functions (printf-style) ----------------------------------
//
// Each one:
//   1. vsnprintf the user fmt+args into a heap-allocated std::string.
//   2. Apply PII redaction.
//   3. Forward to the current sink with level, subsystem,
//      thread-local traceparent, and the redacted message.
//
// Bare user data MUST go through "%s" — these are printf-style and
// take any format string.

void Logv(Level level, std::string_view subsystem, const char* fmt, std::va_list ap) noexcept;

void Logf(Level level, std::string_view subsystem, const char* fmt, ...) noexcept
    __attribute__((format(printf, 3, 4)));

// Convenience wrappers — fixed level.

#define OSW_LOG_DEFINE_LEVEL_FN(name, level_enum)                                 \
    inline void name(std::string_view subsystem, const char* fmt, ...) noexcept   \
        __attribute__((format(printf, 2, 3)));                                    \
    inline void name(std::string_view subsystem, const char* fmt, ...) noexcept { \
        std::va_list ap;                                                          \
        va_start(ap, fmt);                                                        \
        Logv(level_enum, subsystem, fmt, ap);                                     \
        va_end(ap);                                                               \
    }

OSW_LOG_DEFINE_LEVEL_FN(Trace, Level::kTrace)
OSW_LOG_DEFINE_LEVEL_FN(Debug, Level::kDebug)
OSW_LOG_DEFINE_LEVEL_FN(Info, Level::kInfo)
OSW_LOG_DEFINE_LEVEL_FN(Warn, Level::kWarn)
OSW_LOG_DEFINE_LEVEL_FN(Error, Level::kError)
OSW_LOG_DEFINE_LEVEL_FN(Critical, Level::kCritical)

#undef OSW_LOG_DEFINE_LEVEL_FN

}  // namespace osw::log

#endif  // OSW_OBSERVABILITY_LOG_H_
