/*
 * include/osw/media/bug_manager.h
 *
 * MediaBugManager — per-channel media bug registry with stage-rank
 * enforcement and RAII lifecycle.
 *
 * Owns the mapping from (channel_uuid, Purpose) → BugRecord and
 * calls switch_core_media_bug_add / _remove_callback on attach /
 * detach.  All public methods are thread-safe (internal mutex).
 *
 * Referenced FACTs:
 *   FF-031 — switch_core_media_bug_add signature + user_data ownership.
 *   FF-032 — CS_DESTROY state handler registration + timing.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_MEDIA_BUG_MANAGER_H_
#define OSW_MEDIA_BUG_MANAGER_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <grpcpp/support/status_code_enum.h>

#include "osw/media/bug_handle.h"
#include "osw/media/purpose.h"

// Forward-declare the FS opaque types so this header compiles without
// including <switch.h>.  Both production and mock TUs provide these;
// the mock (fs_mock.h) declares them first in its own TU so we must
// guard against redeclaration.
#if !defined(OSW_TEST_FS_MOCK)
struct switch_core_session;
using switch_core_session_t = switch_core_session;
#endif  // !OSW_TEST_FS_MOCK

namespace osw::media {

class SilenceDriverRegistry;

/// Configuration passed to Attach.
struct BugConfig {
    Purpose purpose;
    std::uint32_t fs_flags;        // SMBF_* bitmask; manager may OR in SMBF_FIRST
    std::uint32_t target_rate_hz;  // 8000 or 16000 in V1 (Track B resampler)
    std::string tenant_id;
    std::string stream_endpoint;  // gRPC endpoint of the upstream service
                                  // (stored here; consumed by Track C)
};

class MediaBugManager {
  public:
    MediaBugManager() noexcept;
    ~MediaBugManager() noexcept;

    MediaBugManager(const MediaBugManager&) = delete;
    MediaBugManager& operator=(const MediaBugManager&) = delete;
    MediaBugManager(MediaBugManager&&) = delete;
    MediaBugManager& operator=(MediaBugManager&&) = delete;

    struct AttachResult {
        bool ok;
        grpc::StatusCode status_code;  // OK on success
        std::string error;             // populated when !ok
        BugHandle handle;              // valid when ok
    };

    /// Attach a media bug to `session`.  Enforces:
    ///   1. Per-purpose uniqueness per channel (kAlreadyExists on dup).
    ///   2. Stage-rank ordering:
    ///      - If this_rank < max_attached_rank AND purpose is NOT
    ///        kVadBargeIn AND caller did NOT set SMBF_FIRST in fs_flags
    ///        → kFailedPrecondition.
    ///      - kVadBargeIn always gets SMBF_FIRST OR'd in.
    ///      - Explicit SMBF_FIRST in cfg.fs_flags also bypasses the check.
    ///   3. switch_core_media_bug_add → kInternal on FS failure.
    ///   4. Returns kOk + fresh BugHandle on success.
    ///
    /// Lock discipline: mu_ is acquired to read/update the registry;
    /// released BEFORE calling switch_core_media_bug_add (FS may block).
    AttachResult Attach(switch_core_session_t* session, BugConfig cfg) noexcept;

    /// Detach by id.  Idempotent — unknown id returns silently.
    /// Called by BugHandle destructor; also exposed for manual ownership.
    void Detach(std::uint64_t bug_id) noexcept;

    /// Detach every bug for the given channel.  Called from
    /// OswMediaChannelDestroy (CS_DESTROY hook).  Idempotent.
    ///
    /// FF-032: at CS_DESTROY the FS-side bug chain has already been
    /// cleaned up by hangup processing.  Do NOT call
    /// switch_core_media_bug_remove_callback from this path — only
    /// erase the BugRecord entries.
    void DetachAll(std::string_view channel_uuid) noexcept;

    /// Snapshot counters for tests / Health.
    [[nodiscard]] std::size_t ActiveBugCount(std::string_view channel_uuid) const noexcept;
    [[nodiscard]] std::size_t TotalActiveBugCount() const noexcept;

    /// Function-pointer typedef used by the CS_DESTROY state hook to
    /// route the per-channel cleanup back into ActiveMediaStreams from
    /// inside MediaBugManager TU.  Kept as a raw C-style function ptr +
    /// opaque data pointer so this header has no dependency on
    /// osw/control/active_media_streams.h (which itself depends on
    /// bug_handle.h — a cycle if we included it here).
    using ChannelCleanupFn = void (*)(void* active_streams_opaque, std::string_view channel_uuid);

    /// Register the CS_DESTROY state handler once at module load.
    /// Called from Module::Load AFTER `active_streams_opaque` is
    /// constructed so the CS_DESTROY hook can clean up both registries
    /// on hangup.
    ///
    /// W6.5 P1-003 fix: the previous stub did not actually call
    /// switch_core_add_state_handler — CS_DESTROY never fired, leaking
    /// every bug + stream on channel destroy.  The current impl wires
    /// a switch_state_handler_table_t with on_destroy=OswMediaChannelDestroy
    /// and stores `this` + the opaque registry + the cleanup-fn in
    /// file-static pointers so the C-callable hook can route through
    /// the module-owned objects.
    ///
    /// `active_streams_opaque` + `cleanup_fn` may both be nullptr in
    /// legacy unit tests that don't exercise the ActiveMediaStreams
    /// registry; the hook will skip the cleanup call in that case.
    void RegisterStateHandlers(void* active_streams_opaque, ChannelCleanupFn cleanup_fn) noexcept;

    /// Tear down the CS_DESTROY state handler at module shutdown.
    /// MUST be called before `~MediaBugManager` runs (otherwise the
    /// hook may dereference the destroyed manager).
    void UnregisterStateHandlers() noexcept;

    /// Wire the optional W6.6 silence driver registry. The registry is
    /// module-owned and must outlive this manager until shutdown unregisters
    /// state handlers and drains media streams.
    void SetSilenceDriverRegistry(SilenceDriverRegistry* registry) noexcept;

    /// Stops any silence driver for this channel. Used by the CS_DESTROY
    /// trampoline as a final sweep in case no WRITE_REPLACE bug record
    /// remains by the time channel cleanup runs.
    void RemoveSilenceDriverForChannel(std::string_view channel_uuid) noexcept;

    /// Wire user_cb + user_data onto an existing bug's BugCallbackContext.
    /// Called by Track C handlers after Attach to connect the media bug
    /// callback to the StreamClient / TtsPlayoutBuffer.
    /// `bug_id` comes from the BugHandle returned by Attach.
    /// `user_cb` is the extern "C" callback (OswStreamingReadTap /
    /// OswStreamingWriteReplace) passed as void* to keep this header
    /// free of FS-type dependencies; the .cc implementation casts it
    /// to switch_media_bug_callback_t internally.
    /// `user_data` is a non-owning pointer to handler-owned state.
    /// Returns false if bug_id is not found.
    bool SetBugCallback(std::uint64_t bug_id, void* user_cb, void* user_data) noexcept;

    /// Expose the bug_id from a BugHandle (needed by Track C to call
    /// SetBugCallback without exposing bug_id as a public BugHandle field).
    static std::uint64_t BugId(const BugHandle& h) noexcept { return h.bug_id_; }

    /// Function-name constant used in switch_core_media_bug_add
    /// (FF-031 §"function" field).  Also used as the filter in
    /// switch_core_media_bug_remove_callback.
    static constexpr const char* kFunctionName = "mod_open_switch";

    /// Singleton accessor.  The instance is owned lazily (Meyers singleton)
    /// until Module is updated to hold it as a member (Track C work).
    /// OswMediaChannelDestroy uses this to reach the manager.
    static MediaBugManager& Instance() noexcept;

    /// Shared cleanup core. `call_remove_callback=true` asks FS to detach
    /// the bug (used from BugHandle dtor / explicit Detach); `false` skips
    /// that call (used from the trampoline's CLOSE branch, where FS is
    /// already tearing the bug down, and from DetachAll on CS_DESTROY
    /// where the bug chain has already been freed — FF-032). The
    /// trampoline calls this directly so the friend declaration below is
    /// not enough — keep it public-but-internal.
    void DetachInternal(std::uint64_t bug_id, bool call_remove_callback) noexcept;

  private:
    friend class BugHandle;

    // Pimpl-like: internal data is all in the .cc so the header doesn't
    // need to include FS types.  We declare the data members here because
    // MediaBugManager is a concrete class (not a pure interface), but
    // the FS-type-using internals (BugRecord, BugCallbackContext) are
    // defined only in bug_manager.cc.
    mutable std::mutex mu_;
    std::uint64_t next_id_ = 1;

    // We use type-erased storage for BugRecord to keep FS types out of
    // this header.  by_id_ stores BugRecord* (heap-allocated in .cc)
    // cast to void*; by_channel_ holds bug_ids.
    // Map: bug_id → void* (really BugRecord*)
    std::unordered_map<std::uint64_t, void*> by_id_;
    // Map: channel_uuid → [bug_id, ...]
    std::unordered_map<std::string, std::vector<std::uint64_t>> by_channel_;
    // Map: channel_uuid → per-channel lifecycle lock.  The weak value lets
    // inactive lock objects disappear after the last holder releases them.
    std::unordered_map<std::string, std::weak_ptr<std::mutex>> channel_locks_;
    SilenceDriverRegistry* silence_driver_registry_ = nullptr;

    // Registry helpers — called with mu_ held.
    std::uint32_t MaxRankForChannel(const std::string& uuid) const noexcept;
    bool HasPurpose(const std::string& uuid, Purpose p) const noexcept;
    bool HasWriteReplaceBug(const std::string& uuid) const noexcept;
    // Lifecycle helpers — acquire/use the per-channel lock internally.
    std::shared_ptr<std::mutex> ChannelLockFor(const std::string& uuid) noexcept;
    void PruneExpiredChannelLock(const std::string& uuid) noexcept;
    void DetachInternalLocked(std::uint64_t bug_id, bool call_remove_callback) noexcept;
};

}  // namespace osw::media

#endif  // OSW_MEDIA_BUG_MANAGER_H_
