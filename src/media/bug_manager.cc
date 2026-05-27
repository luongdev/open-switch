/*
 * src/media/bug_manager.cc — MediaBugManager implementation.
 *
 * Referenced FACTs:
 *   FF-031 — switch_core_media_bug_add signature + user_data ownership.
 *             function = kFunctionName ("mod_open_switch").
 *             target   = PurposeName(purpose) for log clarity.
 *             user_data is caller-owned; BugCallbackContext* allocated on
 *             heap and stored in by_id_ (as void*) on success.
 *             *new_bug is FS-owned; we store a non-owning ptr.
 *   FF-032 — Channel state handler registered via
 *             switch_core_event_hook_add_state_change.
 *             CS_DESTROY fires AFTER FS-side bug chain cleanup; we erase
 *             BugRecord entries only (do NOT call
 *             switch_core_media_bug_remove_callback at this point).
 *
 * Lock discipline:
 *   mu_ is acquired for all registry reads/writes.
 *   It is released BEFORE calling switch_core_media_bug_add (FS may
 *   block briefly on its own locks; nested-lock danger).
 *   The bug trampoline callback runs on FS threads and does NOT touch
 *   mu_.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// Include FS headers (or the mock seam) BEFORE anything else so that
// the FS type definitions are visible to all subsequent includes.
#if defined(OSW_TEST_FS_MOCK)
#include "osw/raii/fs_mock.h"
#else
#include <switch_channel.h>
#include <switch_core.h>
#include <switch_core_event_hook.h>
#include <switch_types.h>

#include <switch.h>
#endif

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "osw/media/bug_handle.h"
#include "osw/media/bug_manager.h"
#include "osw/media/purpose.h"
#include "osw/media/silence_driver.h"

// ---------------------------------------------------------------------------
// FS constants and type aliases used in this TU only.
// ---------------------------------------------------------------------------

namespace {

// SMBF_FIRST value (switch_types.h:1920 in v1.10.12).
#if defined(OSW_TEST_FS_MOCK)
// fs_mock.h defines SMBF_FIRST as (1u << 26).
constexpr std::uint32_t kSmbfFirst = static_cast<std::uint32_t>(SMBF_FIRST);
constexpr std::uint32_t kSmbfWriteReplace = static_cast<std::uint32_t>(SMBF_WRITE_REPLACE);
// CS_DESTROY is defined in fs_mock.h as 12.
constexpr int kCsDestroy = static_cast<int>(CS_DESTROY);
#else
constexpr std::uint32_t kSmbfFirst = static_cast<std::uint32_t>(SMBF_FIRST);
constexpr std::uint32_t kSmbfWriteReplace = static_cast<std::uint32_t>(SMBF_WRITE_REPLACE);
constexpr int kCsDestroy = static_cast<int>(CS_DESTROY);
#endif

// ABC types (switch_types.h in v1.10.12).
#if defined(OSW_TEST_FS_MOCK)
// The mock does not define SWITCH_ABC_TYPE_* constants; provide them here.
constexpr int kAbcTypeInit = 0;   // SWITCH_ABC_TYPE_INIT
constexpr int kAbcTypeClose = 8;  // SWITCH_ABC_TYPE_CLOSE
#else
constexpr int kAbcTypeInit = static_cast<int>(SWITCH_ABC_TYPE_INIT);
constexpr int kAbcTypeClose = static_cast<int>(SWITCH_ABC_TYPE_CLOSE);
#endif

}  // namespace

// ---------------------------------------------------------------------------
// Internal data types (defined in .cc so FS types stay out of the header).
// ---------------------------------------------------------------------------

namespace osw::media {

/// Per-bug user_data passed to OswMediaBugTrampoline.
/// Track A creates it with manager + bug_id; Track C populates user_cb.
struct BugCallbackContext {
    MediaBugManager* manager;
    std::uint64_t bug_id;
    void* user_data;                      // Track C populates: StreamClient*
    switch_media_bug_callback_t user_cb;  // Track C populates: handler callback
};

/// Internal registry record.
struct BugRecord {
    std::uint64_t id;  // monotonic; unique within process
    Purpose purpose;
    std::string channel_uuid;
    switch_core_session_t* session;  // captured at Attach; needed for remove_callback
    switch_media_bug_t* fs_bug;      // FS-owned; non-owning ptr (log/debug only)
    BugConfig config;
    BugCallbackContext* ctx;  // heap-allocated; owned by this record (delete in cleanup paths)
};

}  // namespace osw::media

// ---------------------------------------------------------------------------
// FS API shim — routes through the mock seam in tests, real FS in production.
// ---------------------------------------------------------------------------

namespace {

inline switch_status_t FsMediaBugAdd(switch_core_session_t* session,
                                     const char* function_name,
                                     const char* target,
                                     switch_media_bug_callback_t callback,
                                     void* user_data,
                                     time_t stop_time,
                                     std::uint32_t flags,
                                     switch_media_bug_t** new_bug) noexcept {
#if defined(OSW_TEST_FS_MOCK)
    return osw::raii::fs::MediaBugAdd(
        session, function_name, target, callback, user_data, stop_time, flags, new_bug);
#else
    return switch_core_media_bug_add(session,
                                     function_name,
                                     target,
                                     callback,
                                     user_data,
                                     stop_time,
                                     static_cast<switch_media_bug_flag_t>(flags),
                                     new_bug);
#endif
}

// W6.5 P1-004 fix: per-bug pointer-based remove. All module-owned bugs
// share OswMediaBugTrampoline as their callback function, so the
// callback-based remove API removes ALL of them at once — wrong for
// multi-stream calls. Using switch_core_media_bug_remove(session, &bug)
// removes exactly one bug.
inline switch_status_t FsMediaBugRemove(switch_core_session_t* session,
                                        switch_media_bug_t** bug) noexcept {
#if defined(OSW_TEST_FS_MOCK)
    return osw::raii::fs::MediaBugRemove(session, bug);
#else
    return switch_core_media_bug_remove(session, bug);
#endif
}

inline const char* FsSessionGetUuid(switch_core_session_t* session) noexcept {
#if defined(OSW_TEST_FS_MOCK)
    return osw::raii::fs::SessionGetUuid(session);
#else
    return switch_core_session_get_uuid(session);
#endif
}

inline switch_channel_state_t FsChannelGetState(switch_channel_t* channel) noexcept {
#if defined(OSW_TEST_FS_MOCK)
    return osw::raii::fs::ChannelGetState(channel);
#else
    return switch_channel_get_state(channel);
#endif
}

inline switch_channel_t* FsSessionGetChannel(switch_core_session_t* session) noexcept {
#if defined(OSW_TEST_FS_MOCK)
    return osw::raii::fs::SessionGetChannel(session);
#else
    return switch_core_session_get_channel(session);
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// File-static trampolines (extern "C" for FS callback registration).
// ---------------------------------------------------------------------------

extern "C" switch_bool_t OswMediaBugTrampoline(switch_media_bug_t* bug,
                                               void* user_data,
                                               switch_abc_type_t type) noexcept {
    if (!user_data) {
        return SWITCH_TRUE;
    }
    auto* ctx = static_cast<osw::media::BugCallbackContext*>(user_data);

    const int itype = static_cast<int>(type);

    if (itype == kAbcTypeInit) {
        // W6.5 P1-001 fix: forward the real `bug` ptr (was `nullptr`).
        // Track C callbacks call switch_core_media_bug_{get,set}_*_frame(bug)
        // and would crash on a null bug ptr in production.
        if (ctx->user_cb) {
            return ctx->user_cb(bug, ctx->user_data, type);
        }
        return SWITCH_TRUE;
    }

    if (itype == kAbcTypeClose) {
        // FS is closing the bug. We MUST NOT touch ctx after asking the
        // manager to detach — DetachInternal frees the record AND ctx.
        // Capture every field we still need on the stack first.
        osw::media::MediaBugManager* const mgr = ctx->manager;
        const std::uint64_t bug_id = ctx->bug_id;
        const auto user_cb = ctx->user_cb;
        void* const user_data = ctx->user_data;

        if (mgr) {
            // call_remove_callback=false: FS is already tearing the bug
            // down; calling remove_callback from the close-callback would
            // recurse / deadlock. DetachInternal still deletes ctx for us.
            mgr->DetachInternal(bug_id, /*call_remove_callback=*/false);
        }
        // W6.5 P1-001 fix: forward the real `bug` ptr to user_cb's CLOSE
        // handler so it can run any per-bug cleanup before FS frees the bug.
        if (user_cb) {
            return user_cb(bug, user_data, type);
        }
        return SWITCH_TRUE;
    }

    // All other callback types — W6.5 P1-001 fix: forward real bug ptr.
    if (ctx->user_cb) {
        return ctx->user_cb(bug, ctx->user_data, type);
    }
    return SWITCH_TRUE;
}

// W6.5 P1-003 fix: file-static pointers wired by RegisterStateHandlers.
// The CS_DESTROY hook is a C-callable extern function; it cannot capture
// `this`.  Storing the module-owned manager + opaque ActiveMediaStreams
// pointer here lets the hook route through the actual module instances.
namespace {
osw::media::MediaBugManager* g_state_hook_bug_manager_ = nullptr;
void* g_state_hook_active_streams_opaque_ = nullptr;
osw::media::MediaBugManager::ChannelCleanupFn g_state_hook_cleanup_fn_ = nullptr;
}  // namespace

extern "C" switch_status_t OswMediaChannelDestroy(switch_core_session_t* session) noexcept {
    if (!session) {
        return SWITCH_STATUS_SUCCESS;
    }

    // Filter: only act on CS_DESTROY.
    switch_channel_t* channel = FsSessionGetChannel(session);
    if (channel) {
        const int state = static_cast<int>(FsChannelGetState(channel));
        if (state != kCsDestroy) {
            return SWITCH_STATUS_SUCCESS;
        }
    }

    const char* uuid_cstr = FsSessionGetUuid(session);
    if (!uuid_cstr || uuid_cstr[0] == '\0') {
        return SWITCH_STATUS_SUCCESS;
    }

    // W6.5 P1-003 fix: route through the MODULE-OWNED registries (set by
    // RegisterStateHandlers), not the Instance() singleton.  Module::Load
    // creates a std::unique_ptr<MediaBugManager> which is distinct from
    // the singleton; the hook had been calling Instance().DetachAll
    // against the wrong object, so bug records leaked on channel destroy.
    //
    // Order matters: drain ActiveMediaStreams FIRST so its TearDown can
    // detach bugs via BugHandle dtors before MediaBugManager::DetachAll
    // sweeps any survivors.
    if (g_state_hook_cleanup_fn_ && g_state_hook_active_streams_opaque_) {
        g_state_hook_cleanup_fn_(g_state_hook_active_streams_opaque_, uuid_cstr);
    }

    osw::media::MediaBugManager* mgr = g_state_hook_bug_manager_;
    if (!mgr) {
        // Defensive fall-back to the singleton for legacy code paths
        // (e.g., tests that may exercise the hook before RegisterStateHandlers
        // ran).  Logs a warning in production via the calling site of
        // RegisterStateHandlers if the registration was skipped.
        mgr = &osw::media::MediaBugManager::Instance();
    }
    // FF-032: at CS_DESTROY the FS-side bug chain is already cleaned up.
    // DetachAll must NOT call switch_core_media_bug_remove.
    mgr->DetachAll(uuid_cstr);
    mgr->RemoveSilenceDriverForChannel(uuid_cstr);
    return SWITCH_STATUS_SUCCESS;
}

// W6.5 P1-003 fix: switch_state_handler_table_t for switch_core_add_state_handler.
// The table is module-lifetime (static); FS holds a reference until
// switch_core_remove_state_handler is called from UnregisterStateHandlers.
//
// FS struct (FF-035 candidate — switch_module_interfaces.h):
//   12 callbacks (on_init .. on_destroy) + int flags + void* padding[10].
// We only set on_destroy; everything else value-initialises to zero.
#if !defined(OSW_TEST_FS_MOCK)
namespace {
switch_state_handler_table_t MakeOswStateHandlerTable() noexcept {
    switch_state_handler_table_t t{};
    t.on_destroy = OswMediaChannelDestroy;
    return t;
}
switch_state_handler_table_t kOswStateHandlerTable = MakeOswStateHandlerTable();
}  // namespace
#endif  // !OSW_TEST_FS_MOCK

// ---------------------------------------------------------------------------
// MediaBugManager — implementation.
// ---------------------------------------------------------------------------

namespace osw::media {

// Convenience: cast void* back to BugRecord* safely.
static BugRecord* ToRecord(void* p) noexcept {
    return static_cast<BugRecord*>(p);
}

// ---------------------------------------------------------------------------
// Singleton.
// ---------------------------------------------------------------------------

MediaBugManager& MediaBugManager::Instance() noexcept {
    static MediaBugManager instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Constructor / destructor.
// ---------------------------------------------------------------------------

MediaBugManager::MediaBugManager() noexcept = default;

MediaBugManager::~MediaBugManager() noexcept {
    // Defensive: drain all records.  Normally Module::Shutdown has already
    // called DetachAll per channel; this handles abnormal teardown.
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [id, raw_ptr] : by_id_) {
        delete ToRecord(raw_ptr);
    }
    by_id_.clear();
    by_channel_.clear();
}

std::shared_ptr<std::mutex> MediaBugManager::ChannelLockFor(const std::string& uuid) noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);

        auto it = channel_locks_.find(uuid);
        if (it != channel_locks_.end()) {
            auto lock = it->second.lock();
            if (lock) {
                return lock;
            }
            channel_locks_.erase(it);
        }

        auto& weak_lock = channel_locks_[uuid];
        auto lock = std::shared_ptr<std::mutex>{};
        lock = std::make_shared<std::mutex>();
        weak_lock = lock;
        return lock;
    } catch (...) {
        return nullptr;
    }
}

void MediaBugManager::PruneExpiredChannelLock(const std::string& uuid) noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = channel_locks_.find(uuid);
        if (it != channel_locks_.end() && it->second.expired()) {
            channel_locks_.erase(it);
        }
    } catch (...) {
        // Best-effort cleanup only; lifecycle correctness does not depend on
        // pruning an expired weak lock entry.
    }
}

// ---------------------------------------------------------------------------
// Attach.
// ---------------------------------------------------------------------------

MediaBugManager::AttachResult MediaBugManager::Attach(switch_core_session_t* session,
                                                      BugConfig cfg) noexcept {
    const char* uuid_cstr = FsSessionGetUuid(session);
    if (!uuid_cstr || uuid_cstr[0] == '\0') {
        return AttachResult{
            false, grpc::StatusCode::INVALID_ARGUMENT, "session has no UUID", BugHandle{}};
    }
    const std::string uuid(uuid_cstr);

    auto channel_lock = ChannelLockFor(uuid);
    if (!channel_lock) {
        return AttachResult{false,
                            grpc::StatusCode::RESOURCE_EXHAUSTED,
                            "failed to allocate channel lifecycle lock",
                            BugHandle{}};
    }

    // Serialize lifecycle mutations per channel, not globally. This keeps
    // the per-channel uniqueness/rank invariants stable while FsMediaBugAdd
    // runs, without blocking attaches on unrelated channels.
    std::unique_lock<std::mutex> channel_lk(*channel_lock);
    auto cleanup_channel_lock = [&]() noexcept {
        if (channel_lk.owns_lock()) {
            channel_lk.unlock();
        }
        channel_lock.reset();
        PruneExpiredChannelLock(uuid);
    };

    std::uint64_t assigned_id = 0;
    std::uint32_t effective_flags = cfg.fs_flags;
    const Purpose purpose_copy = cfg.purpose;
    AttachResult validation_failure{
        false, grpc::StatusCode::UNKNOWN, "validation not run", BugHandle{}};
    bool validation_failed = false;

    {
        std::lock_guard<std::mutex> lk(mu_);

        // Duplicate purpose check.
        if (HasPurpose(uuid, cfg.purpose)) {
            validation_failure = AttachResult{
                false,
                grpc::StatusCode::ALREADY_EXISTS,
                std::string("purpose already attached: ") + std::string(PurposeName(cfg.purpose)),
                BugHandle{}};
            validation_failed = true;
        } else {
            // Stage-rank enforcement.
            const std::uint32_t this_rank = StageRank(cfg.purpose);
            const std::uint32_t max_rank = MaxRankForChannel(uuid);

            if (cfg.purpose == Purpose::kVadBargeIn) {
                // VAD always gets SMBF_FIRST regardless of attach order.
                effective_flags |= kSmbfFirst;
            } else if (this_rank < max_rank) {
                // Out-of-order attach.  Allowed only if caller already set
                // SMBF_FIRST in cfg.fs_flags.
                if ((cfg.fs_flags & kSmbfFirst) == 0) {
                    validation_failure = AttachResult{
                        false,
                        grpc::StatusCode::FAILED_PRECONDITION,
                        std::string("out-of-order attach: purpose=") +
                            std::string(PurposeName(cfg.purpose)) +
                            " rank=" + std::to_string(this_rank) +
                            " already-attached max-rank=" + std::to_string(max_rank),
                        BugHandle{}};
                    validation_failed = true;
                }
            }

            if (!validation_failed) {
                assigned_id = next_id_++;
            }
        }
    }

    if (validation_failed) {
        cleanup_channel_lock();
        return validation_failure;
    }

    // Allocate callback context before calling FS.
    auto* ctx = new (std::nothrow) BugCallbackContext{this, assigned_id, nullptr, nullptr};
    if (!ctx) {
        cleanup_channel_lock();
        return AttachResult{false,
                            grpc::StatusCode::RESOURCE_EXHAUSTED,
                            "failed to allocate BugCallbackContext",
                            BugHandle{}};
    }

    // Call switch_core_media_bug_add.
    switch_media_bug_t* fs_bug = nullptr;
    const auto rc = FsMediaBugAdd(session,
                                  kFunctionName,
                                  std::string(PurposeName(purpose_copy)).c_str(),
                                  OswMediaBugTrampoline,
                                  ctx,
                                  0,
                                  effective_flags,
                                  &fs_bug);

    if (rc != SWITCH_STATUS_SUCCESS) {
        delete ctx;
        cleanup_channel_lock();
        return AttachResult{
            false,
            grpc::StatusCode::INTERNAL,
            std::string("switch_core_media_bug_add failed: rc=") + std::to_string(rc),
            BugHandle{}};
    }

    // Register in maps.
    auto* rec = new (std::nothrow) BugRecord{};
    if (!rec) {
        // W6.5 P2-002 fix: FS already holds a bug with `ctx` as its
        // user_data.  If we delete ctx without removing the FS bug,
        // the next callback fires with a dangling user_data → UAF.
        // Remove the bug from FS BEFORE freeing ctx.
        FsMediaBugRemove(session, &fs_bug);
        delete ctx;
        cleanup_channel_lock();
        return AttachResult{false,
                            grpc::StatusCode::RESOURCE_EXHAUSTED,
                            "failed to allocate BugRecord",
                            BugHandle{}};
    }

    rec->id = assigned_id;
    rec->purpose = purpose_copy;
    rec->channel_uuid = uuid;
    rec->session = session;  // captured for Detach → remove
    rec->fs_bug = fs_bug;
    rec->config = std::move(cfg);
    rec->config.fs_flags = effective_flags;
    rec->ctx = ctx;

    SilenceDriverRegistry* silence_registry = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        by_id_.emplace(assigned_id, static_cast<void*>(rec));
        by_channel_[uuid].push_back(assigned_id);
        if ((effective_flags & kSmbfWriteReplace) != 0) {
            silence_registry = silence_driver_registry_;
        }
    }

    if (silence_registry) {
        silence_registry->AttachOpportunistic(session);
    }

    BugHandle handle(this, assigned_id, purpose_copy, uuid);
    cleanup_channel_lock();
    return AttachResult{true, grpc::StatusCode::OK, "", std::move(handle)};
}

// ---------------------------------------------------------------------------
// SetBugCallback — Track C wires user_cb + user_data after Attach.
// ---------------------------------------------------------------------------

bool MediaBugManager::SetBugCallback(std::uint64_t bug_id,
                                     void* user_cb_vp,
                                     void* user_data) noexcept {
    // Cast back to the concrete FS callback type. The public API uses void*
    // to keep bug_manager.h free of <switch_types.h> dependencies so that
    // FS-agnostic TUs can include it (e.g. bug_handle.cc in osw_media).
    auto user_cb = reinterpret_cast<switch_media_bug_callback_t>(user_cb_vp);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_id_.find(bug_id);
    if (it == by_id_.end()) {
        return false;
    }
    auto* rec = ToRecord(it->second);
    if (!rec || !rec->ctx) {
        return false;
    }
    rec->ctx->user_cb = user_cb;
    rec->ctx->user_data = user_data;
    return true;
}

// ---------------------------------------------------------------------------
// Detach.
// ---------------------------------------------------------------------------

// Internal: erase the record + delete ctx + (optionally) ask FS to detach
// the bug. mu_ is acquired/released inside. Caller-side bool decides whether
// to invoke remove_callback:
//
//   call_remove_callback=true  (default): BugHandle dtor / explicit Detach
//     — session is still alive (FS guarantees session_t valid until our
//       CS_DESTROY hook returns), and we must stop FS callbacks before
//       freeing ctx (which is used as user_data by FS).
//
//   call_remove_callback=false: invoked by the trampoline's CLOSE branch
//     (FS is already tearing down the bug — calling remove_callback here
//     would be redundant or unsafe), and by DetachAll on CS_DESTROY
//     (FS has already closed all bugs for the dying channel; bug_t* is
//     stale at this point — see FF-032).
void MediaBugManager::DetachInternalLocked(std::uint64_t bug_id,
                                           bool call_remove_callback) noexcept {
    // Capture what we need under mu_, then release before calling FS.
    switch_core_session_t* session_to_remove = nullptr;
    switch_media_bug_t* fs_bug_to_remove = nullptr;
    BugCallbackContext* ctx_to_delete = nullptr;
    BugRecord* rec_to_delete = nullptr;
    SilenceDriverRegistry* silence_registry = nullptr;
    std::string removed_channel_uuid;
    bool stop_silence_driver = false;

    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = by_id_.find(bug_id);
        if (it == by_id_.end()) {
            return;  // already gone or unknown — idempotent
        }
        BugRecord* rec = ToRecord(it->second);

        // Drop from by_channel_ index.
        auto cit = by_channel_.find(rec->channel_uuid);
        if (cit != by_channel_.end()) {
            auto& vec = cit->second;
            vec.erase(std::remove(vec.begin(), vec.end(), bug_id), vec.end());
            if (vec.empty()) {
                by_channel_.erase(cit);
            }
        }

        // Take ownership for cleanup outside the lock.
        // W6.5 P1-004 fix: capture the EXACT bug pointer so we can detach
        // only this one bug (not every bug that shares the trampoline
        // callback). The codex review found that switch_core_media_bug_
        // remove_callback() removes ALL bugs with the matching callback,
        // which breaks multi-stream calls where N module-owned bugs all
        // share OswMediaBugTrampoline as their callback function.
        session_to_remove = call_remove_callback ? rec->session : nullptr;
        fs_bug_to_remove = call_remove_callback ? rec->fs_bug : nullptr;
        ctx_to_delete = rec->ctx;
        rec_to_delete = rec;
        removed_channel_uuid = rec->channel_uuid;
        if ((rec->config.fs_flags & kSmbfWriteReplace) != 0 &&
            !HasWriteReplaceBug(removed_channel_uuid)) {
            stop_silence_driver = true;
            silence_registry = silence_driver_registry_;
        }
        by_id_.erase(it);
    }

    // mu_ released. Ask FS to detach the bug if requested. FS guarantees
    // that after switch_core_media_bug_remove() returns, no more callback
    // invocations will fire for this bug — so freeing ctx below is safe.
    if (session_to_remove && fs_bug_to_remove) {
        // W6.5 P1-004 fix: remove the specific bug by pointer, not by
        // callback function (which would remove every module-owned bug).
        FsMediaBugRemove(session_to_remove, &fs_bug_to_remove);
    }

    delete ctx_to_delete;
    delete rec_to_delete;

    if (stop_silence_driver && silence_registry) {
        silence_registry->DetachIfOrphan(removed_channel_uuid);
    }
}

void MediaBugManager::DetachInternal(std::uint64_t bug_id, bool call_remove_callback) noexcept {
    std::string channel_uuid;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = by_id_.find(bug_id);
        if (it == by_id_.end()) {
            return;
        }
        const BugRecord* rec = ToRecord(it->second);
        if (!rec) {
            return;
        }
        channel_uuid = rec->channel_uuid;
    }

    auto channel_lock = ChannelLockFor(channel_uuid);
    if (channel_lock) {
        {
            std::unique_lock<std::mutex> channel_lk(*channel_lock);
            DetachInternalLocked(bug_id, call_remove_callback);
        }
        channel_lock.reset();
        PruneExpiredChannelLock(channel_uuid);
        return;
    }

    // Best-effort cleanup under memory pressure. ChannelLockFor only fails
    // when allocating a new lock object fails; do not leak a known bug record.
    DetachInternalLocked(bug_id, call_remove_callback);
}

void MediaBugManager::Detach(std::uint64_t bug_id) noexcept {
    DetachInternal(bug_id, /*call_remove_callback=*/true);
}

void MediaBugManager::DetachAll(std::string_view channel_uuid) noexcept {
    const std::string uuid(channel_uuid);
    std::vector<std::uint64_t> ids;

    auto channel_lock = ChannelLockFor(uuid);
    std::unique_lock<std::mutex> channel_lk;
    if (channel_lock) {
        channel_lk = std::unique_lock<std::mutex>(*channel_lock);
    }

    bool already_clean = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto cit = by_channel_.find(uuid);
        if (cit == by_channel_.end()) {
            already_clean = true;
        } else {
            ids = cit->second;  // copy the id list
        }
    }
    if (already_clean) {
        if (channel_lk.owns_lock()) {
            channel_lk.unlock();
        }
        channel_lock.reset();
        PruneExpiredChannelLock(uuid);
        return;  // already clean — idempotent
    }

    // FF-032: during CS_DESTROY do NOT call remove_callback (FS already
    // closed our bugs). Still must free ctx + rec for every record.
    for (std::uint64_t id : ids) {
        DetachInternalLocked(id, /*call_remove_callback=*/false);
    }
    if (channel_lk.owns_lock()) {
        channel_lk.unlock();
    }
    channel_lock.reset();
    PruneExpiredChannelLock(uuid);
}

// ---------------------------------------------------------------------------
// Counters.
// ---------------------------------------------------------------------------

std::size_t MediaBugManager::ActiveBugCount(std::string_view channel_uuid) const noexcept {
    const std::string uuid(channel_uuid);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_channel_.find(uuid);
    if (it == by_channel_.end()) {
        return 0;
    }
    return it->second.size();
}

std::size_t MediaBugManager::TotalActiveBugCount() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return by_id_.size();
}

// ---------------------------------------------------------------------------
// State handler registration.
// ---------------------------------------------------------------------------

void MediaBugManager::RegisterStateHandlers(void* active_streams_opaque,
                                            MediaBugManager::ChannelCleanupFn cleanup_fn) noexcept {
    // W6.5 P1-003 fix: wire the file-static pointers used by
    // OswMediaChannelDestroy + register the FS state-handler table.
    g_state_hook_bug_manager_ = this;
    g_state_hook_active_streams_opaque_ = active_streams_opaque;
    g_state_hook_cleanup_fn_ = cleanup_fn;

#if !defined(OSW_TEST_FS_MOCK)
    // Production: hand FS a pointer to the static handler table; FS
    // will invoke on_destroy=OswMediaChannelDestroy at CS_DESTROY for
    // every channel.  The table is global static — no ownership.
    switch_core_add_state_handler(&kOswStateHandlerTable);
#endif
    // In tests with OSW_TEST_FS_MOCK, the existing bug_manager_test
    // bypasses the hook entirely by calling mgr_.DetachAll directly.
    // No additional mock plumbing is required.
}

void MediaBugManager::UnregisterStateHandlers() noexcept {
#if !defined(OSW_TEST_FS_MOCK)
    switch_core_remove_state_handler(&kOswStateHandlerTable);
#endif
    g_state_hook_bug_manager_ = nullptr;
    g_state_hook_active_streams_opaque_ = nullptr;
    g_state_hook_cleanup_fn_ = nullptr;
}

void MediaBugManager::SetSilenceDriverRegistry(SilenceDriverRegistry* registry) noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    silence_driver_registry_ = registry;
}

void MediaBugManager::RemoveSilenceDriverForChannel(std::string_view channel_uuid) noexcept {
    SilenceDriverRegistry* registry = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        registry = silence_driver_registry_;
    }
    if (registry) {
        registry->RemoveForChannel(channel_uuid);
    }
}

// ---------------------------------------------------------------------------
// Internal helpers (called with mu_ held).
// ---------------------------------------------------------------------------

std::uint32_t MediaBugManager::MaxRankForChannel(const std::string& uuid) const noexcept {
    auto cit = by_channel_.find(uuid);
    if (cit == by_channel_.end()) {
        return 0;
    }
    std::uint32_t max_rank = 0;
    for (std::uint64_t id : cit->second) {
        auto it = by_id_.find(id);
        if (it != by_id_.end()) {
            const BugRecord* rec = static_cast<const BugRecord*>(it->second);
            const std::uint32_t r = StageRank(rec->purpose);
            if (r > max_rank) {
                max_rank = r;
            }
        }
    }
    return max_rank;
}

bool MediaBugManager::HasPurpose(const std::string& uuid, Purpose p) const noexcept {
    auto cit = by_channel_.find(uuid);
    if (cit == by_channel_.end()) {
        return false;
    }
    for (std::uint64_t id : cit->second) {
        auto it = by_id_.find(id);
        if (it != by_id_.end()) {
            const BugRecord* rec = static_cast<const BugRecord*>(it->second);
            if (rec->purpose == p) {
                return true;
            }
        }
    }
    return false;
}

bool MediaBugManager::HasWriteReplaceBug(const std::string& uuid) const noexcept {
    auto cit = by_channel_.find(uuid);
    if (cit == by_channel_.end()) {
        return false;
    }
    for (std::uint64_t id : cit->second) {
        auto it = by_id_.find(id);
        if (it != by_id_.end()) {
            const BugRecord* rec = static_cast<const BugRecord*>(it->second);
            if ((rec->config.fs_flags & kSmbfWriteReplace) != 0) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace osw::media
