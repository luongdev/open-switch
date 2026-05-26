/*
 * include/osw/observability/health.h
 *
 * osw::Health — module health aggregator surfaced via the Health gRPC.
 *
 * Per W1 contract §"src/observability/", the Health aggregator exposes:
 *
 *   - module_version (from a compile-time constant)
 *   - freeswitch_version (looked up at module load via switch_version_*)
 *   - status (SERVING / DRAINING). DRAINING is set by the lifecycle
 *     orchestrator on SIGTERM (see designs/architecture.md §"Graceful
 *     drain").
 *   - placeholder counter slots — all zero in W1 because the owning
 *     subsystems (event ring, gRPC server tracker, media bug manager)
 *     are not yet implemented. Each owning subsystem will update its
 *     slot atomically as it comes online in W2/W3/W4.
 *
 * Threading: counters are std::atomic. Status is std::atomic<Status>.
 * The Snapshot() method takes a wait-free copy under the assumption
 * that callers tolerate a tiny window of skew between counter loads
 * (acceptable for a health endpoint).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_OBSERVABILITY_HEALTH_H_
#define OSW_OBSERVABILITY_HEALTH_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace osw {

class Health {
 public:
    enum class Status : int {
        kUnspecified = 0,
        kServing     = 1,
        kNotServing  = 2,
        kDraining    = 3,
    };

    /// A wait-free point-in-time snapshot. Field semantics mirror
    /// open_switch.control.v1.HealthResponse.
    struct Snapshot {
        Status      status                 = Status::kServing;
        std::string module_version;
        std::string freeswitch_version;
        std::uint64_t active_channels      = 0;
        std::uint64_t active_media_bugs    = 0;
        std::uint64_t events_emitted_total = 0;
        std::uint32_t subscriber_count     = 0;
        std::uint32_t tier1_ring_fill_pct  = 0;
        std::uint32_t tier2_ring_fill_pct  = 0;
        std::uint32_t tier3_ring_fill_pct  = 0;
        std::uint64_t tier1_dropped_total  = 0;
        std::uint64_t tier2_dropped_total  = 0;
        std::uint64_t tier3_dropped_total  = 0;
    };

    Health() = default;

    /// Sets the two string fields. Called once at module load.
    void SetVersions(std::string module_version, std::string freeswitch_version);

    /// Returns the current Status atomically.
    [[nodiscard]] Status GetStatus() const noexcept {
        return status_.load(std::memory_order_acquire);
    }

    /// Flips status to the new value. Idempotent.
    void SetStatus(Status s) noexcept {
        status_.store(s, std::memory_order_release);
    }

    // --- Counter setters (owning subsystems call these) ---------------
    //
    // W1 ships no callers; the slots are exposed so W2/W3/W4 wire their
    // owning subsystems without touching the Health header.

    void SetActiveChannels(std::uint64_t v) noexcept       { active_channels_.store(v, std::memory_order_relaxed); }
    void SetActiveMediaBugs(std::uint64_t v) noexcept      { active_media_bugs_.store(v, std::memory_order_relaxed); }
    void SetEventsEmittedTotal(std::uint64_t v) noexcept   { events_emitted_total_.store(v, std::memory_order_relaxed); }
    void SetSubscriberCount(std::uint32_t v) noexcept      { subscriber_count_.store(v, std::memory_order_relaxed); }
    void SetTier1RingFillPct(std::uint32_t v) noexcept     { tier1_ring_fill_pct_.store(v, std::memory_order_relaxed); }
    void SetTier2RingFillPct(std::uint32_t v) noexcept     { tier2_ring_fill_pct_.store(v, std::memory_order_relaxed); }
    void SetTier3RingFillPct(std::uint32_t v) noexcept     { tier3_ring_fill_pct_.store(v, std::memory_order_relaxed); }
    void SetTier1DroppedTotal(std::uint64_t v) noexcept    { tier1_dropped_total_.store(v, std::memory_order_relaxed); }
    void SetTier2DroppedTotal(std::uint64_t v) noexcept    { tier2_dropped_total_.store(v, std::memory_order_relaxed); }
    void SetTier3DroppedTotal(std::uint64_t v) noexcept    { tier3_dropped_total_.store(v, std::memory_order_relaxed); }

    /// Wait-free point-in-time snapshot. The fields are loaded under
    /// std::memory_order_acquire individually — callers tolerate a
    /// tiny window of skew (acceptable for a health endpoint).
    [[nodiscard]] Snapshot GetSnapshot() const;

 private:
    // Versions are immutable after SetVersions; we still wrap them in
    // a small mutex-protected struct to avoid std::string thread-safety
    // assumptions. Reads happen during Snapshot().
    mutable std::atomic<Status> status_{Status::kServing};

    // Atomic counters owned by future subsystems.
    std::atomic<std::uint64_t> active_channels_{0};
    std::atomic<std::uint64_t> active_media_bugs_{0};
    std::atomic<std::uint64_t> events_emitted_total_{0};
    std::atomic<std::uint32_t> subscriber_count_{0};
    std::atomic<std::uint32_t> tier1_ring_fill_pct_{0};
    std::atomic<std::uint32_t> tier2_ring_fill_pct_{0};
    std::atomic<std::uint32_t> tier3_ring_fill_pct_{0};
    std::atomic<std::uint64_t> tier1_dropped_total_{0};
    std::atomic<std::uint64_t> tier2_dropped_total_{0};
    std::atomic<std::uint64_t> tier3_dropped_total_{0};

    // Versions accessed via Snapshot(); reads are infrequent.
    mutable std::string module_version_;
    mutable std::string freeswitch_version_;
};

}  // namespace osw

#endif  // OSW_OBSERVABILITY_HEALTH_H_
