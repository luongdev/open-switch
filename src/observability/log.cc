/*
 * src/observability/log.cc
 *
 * Subsystem-neutral implementation of osw::log::* — Logv / Logf,
 * SetSinkForTesting, SetRedactionPatterns, TraceScope.
 *
 * The default sink (which forwards to switch_log_printf) lives in a
 * separate translation unit, log_default_sink.cc, so that this file can
 * be compiled into the unit-test binary WITHOUT pulling in <switch.h>.
 * Tests install their own sink via SetSinkForTesting before any log
 * emission, so the default sink's symbol is never reached.
 *
 * Threading:
 *   - Sink installation (SetSinkForTesting) is test-only and assumed
 *     single-threaded.
 *   - Redaction-pattern publication uses
 *     std::atomic<std::shared_ptr<const Patterns>>. Lock-free on the
 *     hot read path.
 *   - Thread-local traceparent: pure thread-local, no synchronisation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/observability/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace osw::log {

namespace {

// --- Sink slot --------------------------------------------------------
//
// The slot is initialised to a benign do-nothing sink at static init
// time. The DefaultSink that actually calls switch_log_printf is
// installed by InstallDefaultSink() at module load (called from
// log_default_sink.cc's module-init helper).

void NullSink(Level /*level*/, std::string_view /*subsystem*/,
              std::string_view /*traceparent*/, std::string_view /*message*/) noexcept {
    // Intentional no-op. Production code overrides via
    // InstallDefaultSinkForModule() (log_default_sink.cc) early in
    // mod_open_switch_load.
}

std::atomic<SinkFn> g_sink{&NullSink};

// --- Redaction patterns ----------------------------------------------

using PatternList = std::vector<std::regex>;

// Redaction-pattern publication uses the C++20 partial specialisation
// std::atomic<std::shared_ptr<T>>. libstdc++ in GCC 13+ (Debian trixie)
// and libc++ in LLVM 16+ both ship it. The deprecated free-function
// overloads std::atomic_{load,store}_explicit(std::shared_ptr<T>*, ...)
// from C++11 were [[deprecated]]-tagged in C++20 (LWG 3766) and removed
// in C++26; with -Werror they fail to compile, so we standardise on
// the partial specialisation here.
//
// Writers (config load, SIGHUP reload) are rare; readers fire on every
// log line. The partial specialisation is wait-free on platforms that
// have a lock-free std::atomic<std::shared_ptr> implementation
// (x86_64 libstdc++ uses a fine-grained spinlock internally — still
// fast enough for our throughput).
std::atomic<std::shared_ptr<const PatternList>>& PatternsSlot() noexcept {
    static std::atomic<std::shared_ptr<const PatternList>> slot{
        std::make_shared<const PatternList>()};
    return slot;
}

std::shared_ptr<const PatternList> LoadPatterns() noexcept {
    return PatternsSlot().load(std::memory_order_acquire);
}

void StorePatterns(std::shared_ptr<const PatternList> snap) noexcept {
    PatternsSlot().store(std::move(snap), std::memory_order_release);
}

std::string ApplyPatterns(std::string_view in) {
    auto patterns = LoadPatterns();
    if (!patterns || patterns->empty()) {
        return std::string(in);
    }
    std::string out(in);
    for (const auto& re : *patterns) {
        out = std::regex_replace(out, re, std::string("[REDACTED]"));
    }
    return out;
}

// --- Thread-local traceparent ----------------------------------------

thread_local std::string tls_traceparent;

}  // namespace

// --- Public API implementations --------------------------------------

SinkFn SetSinkForTesting(SinkFn new_sink) noexcept {
    SinkFn previous = g_sink.exchange(new_sink ? new_sink : &NullSink,
                                      std::memory_order_acq_rel);
    return previous;
}

void SetRedactionPatterns(std::vector<std::regex> patterns) {
    auto snap = std::make_shared<const PatternList>(std::move(patterns));
    StorePatterns(std::move(snap));
}

std::size_t RedactionPatternCountForTesting() {
    auto snap = LoadPatterns();
    return snap ? snap->size() : 0;
}

std::string ApplyRedactionForTesting(std::string_view message) {
    return ApplyPatterns(message);
}

std::string_view CurrentTraceparent() noexcept {
    return tls_traceparent;
}

TraceScope::TraceScope(std::string traceparent) : previous_(tls_traceparent) {
    tls_traceparent = std::move(traceparent);
}

TraceScope::~TraceScope() noexcept {
    tls_traceparent = std::move(previous_);
}

// --- Emit functions --------------------------------------------------

void Logv(Level level, std::string_view subsystem,
          const char* fmt, std::va_list ap) noexcept {
    // Format into a stack buffer first; on overflow, heap retry.
    char stack_buf[1024];
    std::va_list ap_copy;
    va_copy(ap_copy, ap);
    int n = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap_copy);
    va_end(ap_copy);

    if (n < 0) {
        return;  // encoding error
    }

    std::string formatted;
    if (static_cast<std::size_t>(n) < sizeof(stack_buf)) {
        formatted.assign(stack_buf, static_cast<std::size_t>(n));
    } else {
        formatted.resize(static_cast<std::size_t>(n));
        std::va_list ap_retry;
        va_copy(ap_retry, ap);
        const int n2 = std::vsnprintf(formatted.data(),
                                      formatted.size() + 1, fmt, ap_retry);
        va_end(ap_retry);
        if (n2 < 0) {
            return;
        }
    }

    std::string redacted = ApplyPatterns(formatted);

    SinkFn sink = g_sink.load(std::memory_order_acquire);
    if (sink) {
        sink(level, subsystem, tls_traceparent, redacted);
    }
}

void Logf(Level level, std::string_view subsystem,
          const char* fmt, ...) noexcept {
    std::va_list ap;
    va_start(ap, fmt);
    Logv(level, subsystem, fmt, ap);
    va_end(ap);
}

// --- Internal hook used by log_default_sink.cc -----------------------
//
// log_default_sink.cc calls this from the module's load entry point to
// swap the NullSink out for the switch_log_printf-backed DefaultSink.
// We expose it via a function rather than direct slot manipulation so
// log_default_sink.cc has no header dependency on the internal namespace.

extern "C" void osw_log_install_default_sink(SinkFn sink) noexcept {
    if (sink) {
        g_sink.store(sink, std::memory_order_release);
    }
}

}  // namespace osw::log
