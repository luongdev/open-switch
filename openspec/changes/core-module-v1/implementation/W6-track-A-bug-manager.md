# W6 Track A — MediaBugManager + lifecycle + stage rank

**Wave.** [W6 Media plane V1](W6-media-plane.md).
**Owner.** Sonnet sub-agent (claude-sonnet).
**Branch.** `implementation/wave6-track-a-bug-manager` (off `main`).
**Phase.** 1 (parallel with Track B). Must merge before Track C starts.

Track A lands the foundational `MediaBugManager` that owns the per-channel
bug registry and enforces the canonical stage-rank ordering documented in
`designs/media-bridge.md`. Tracks B + C consume the `BugHandle` API.

---

## Files in scope

**Create.**
- `include/osw/media/purpose.h` — `enum class Purpose` + free helpers
- `include/osw/media/bug_handle.h` — RAII lease returned by `Attach`
- `include/osw/media/bug_manager.h` — public API
- `src/media/bug_manager.cc`
- `src/media/bug_handle.cc`
- `src/media/CMakeLists.txt` (new subdir)
- `tests/unit/media/CMakeLists.txt` (new subdir)
- `tests/unit/media/bug_manager_test.cc`

**Modify.**
- `src/CMakeLists.txt` — `add_subdirectory(media)` after `observability`
- `tests/unit/CMakeLists.txt` — `add_subdirectory(media)`
- `include/osw/raii/fs_mock.h` — extend mock seam to capture
  `switch_core_media_bug_add` / `_remove_callback` invocations
  (function-name + flags + user_data pointer)
- `openspec/changes/core-module-v1/FREESWITCH-FACTS.md` — append
  - `FF-031` — `switch_core_media_bug_add` signature, callback type
    (`switch_bool_t cb(switch_media_bug_t*, void*, switch_abc_type_t)`),
    `user_data` ownership (caller-owned; must outlive bug)
  - `FF-032` — channel state handler registration via
    `switch_core_event_hook_add_state_change` and `CS_DESTROY` timing

---

## API contract

```cpp
namespace osw::media {

enum class Purpose : std::uint8_t {
    kUnspecified = 0,
    kTtsPlayback,
    kSttTranscribe,
    kVoicebotDuplexRead,
    kVoicebotDuplexWrite,
    kAmdDetect,        // proto-reserved; not used by W6 (W7 or V2)
    kRecordingRelay,   // proto-reserved; W7
    kVadBargeIn,       // proto-reserved; V2
    kTest,             // internal smoke
};

/// Maps Purpose → numeric stage rank used by the order check. Free helper
/// so tests can assert ordering without poking BugManager internals.
[[nodiscard]] std::uint32_t StageRank(Purpose p) noexcept;

/// String form for log + audit. Matches the snake-case Purpose enum
/// names in proto/open_switch/media/v1/media.proto.
[[nodiscard]] std::string_view PurposeName(Purpose p) noexcept;

struct BugConfig {
    Purpose purpose;
    std::uint32_t fs_flags;        // SMBF_* bitmask (manager may OR in SMBF_FIRST)
    std::uint32_t target_rate_hz;  // 8000 or 16000 only in V1 (Track B resampler)
    std::string tenant_id;
    std::string stream_endpoint;   // gRPC endpoint of the upstream service
                                   // (handled by Track C — manager just stores it)
};

/// Opaque handle. Destruction triggers Detach. Move-only.
class BugHandle {
  public:
    BugHandle() noexcept = default;
    ~BugHandle() noexcept;  // calls manager_->Detach if attached
    BugHandle(BugHandle&&) noexcept;
    BugHandle& operator=(BugHandle&&) noexcept;
    BugHandle(const BugHandle&) = delete;
    BugHandle& operator=(const BugHandle&) = delete;

    [[nodiscard]] bool attached() const noexcept;
    [[nodiscard]] Purpose purpose() const noexcept;
    [[nodiscard]] std::string channel_uuid() const noexcept;

    /// Release ownership without detaching (rare — handler hands the bug
    /// over to a long-lived owner). Caller becomes responsible for
    /// calling MediaBugManager::Detach manually.
    void release() noexcept;

  private:
    friend class MediaBugManager;
    BugHandle(MediaBugManager* mgr, std::uint64_t bug_id) noexcept;
    MediaBugManager* manager_ = nullptr;
    std::uint64_t bug_id_ = 0;
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

    /// Attach a media bug to `session`. Enforces stage-rank ordering and
    /// per-purpose uniqueness per channel. Returns:
    ///   - kAlreadyExists if a bug for `cfg.purpose` already exists
    ///   - kFailedPrecondition if cfg.purpose's rank < max attached rank
    ///     and SMBF_FIRST is not set (VAD always gets SMBF_FIRST OR'd in)
    ///   - kInternal if switch_core_media_bug_add returns non-success
    ///   - kOk + a fresh BugHandle on success
    AttachResult Attach(switch_core_session_t* session, BugConfig cfg) noexcept;

    /// Detach by id. Idempotent — unknown id returns silently. Called by
    /// BugHandle destructor; also exposed for manual ownership cases.
    void Detach(std::uint64_t bug_id) noexcept;

    /// Detach every bug for the given channel. Called from the
    /// CS_DESTROY state handler. Idempotent.
    void DetachAll(std::string_view channel_uuid) noexcept;

    /// Snapshot for tests / Health: number of active bugs by channel +
    /// purpose. Cheap lock + copy.
    [[nodiscard]] std::size_t ActiveBugCount(std::string_view channel_uuid) const noexcept;
    [[nodiscard]] std::size_t TotalActiveBugCount() const noexcept;
};

}  // namespace osw::media
```

### Stage rank table

Mirrors `designs/media-bridge.md` §"Stage rank":

| Rank | Value | Purpose | Default flags |
|---|---|---|---|
| EARLY | 1 | `kVadBargeIn` | `SMBF_READ_STREAM \| SMBF_FIRST` (always OR'd in) |
| MID_READ | 2 | `kSttTranscribe`, `kVoicebotDuplexRead`, `kAmdDetect` | `SMBF_READ_STREAM` |
| INJECT | 3 | `kTtsPlayback`, `kVoicebotDuplexWrite` | `SMBF_WRITE_REPLACE` |
| LATE | 4 | `kRecordingRelay` (W7), `kTest` | varies |

Order check on `Attach`:
1. If purpose already attached on this channel → `kAlreadyExists`.
2. Compute `this_rank = StageRank(cfg.purpose)`,
   `max_rank = max(StageRank(p) for p in attached_purposes_on_channel)`.
3. If `this_rank < max_rank`:
   - If `cfg.purpose == kVadBargeIn` (or caller passed `SMBF_FIRST`) →
     OR in `SMBF_FIRST` and proceed (prepends to FS bug chain head).
   - Else → return `kFailedPrecondition` with message
     `"out-of-order attach: purpose=<X> rank=<n> already-attached max-rank=<m>"`.

---

## Internal data structures

```cpp
struct BugRecord {
    std::uint64_t id;            // unique within process, monotonic
    Purpose purpose;
    std::string channel_uuid;
    switch_media_bug_t* fs_bug;  // owned by FS; we hold a non-owning ptr
    BugConfig config;
};

// All access serialised by mu_. Reads are cheap (in-memory map + vector).
std::mutex mu_;
std::uint64_t next_id_ = 1;
std::unordered_map<std::uint64_t, BugRecord> by_id_;
std::unordered_map<std::string, std::vector<std::uint64_t>> by_channel_;
```

Lock discipline: `mu_` is held during `Attach` / `Detach` / `DetachAll`.
Releases the lock BEFORE calling `switch_core_media_bug_add` (FS may
block briefly on its own locks; nested-lock danger). The bug callback
itself runs on FS threads and does NOT touch `mu_` — it dispatches to
the user-data pointer (a Track C handler installs its own callback
context).

---

## Bug callback signature (Track A only defines the trampoline)

```cpp
// File-static (per FF-003); installed as user_data for every bug.
struct BugCallbackContext {
    MediaBugManager* manager;
    std::uint64_t bug_id;
    void* user_data;             // Track C populates: pointer to StreamClient
    BugCallback user_cb;         // Track C populates: function pointer
};

extern "C" switch_bool_t OswMediaBugTrampoline(
    switch_media_bug_t* bug,
    void* user_data,
    switch_abc_type_t type) noexcept;
```

Track A implements `OswMediaBugTrampoline` as a thin dispatch:
- `type == SWITCH_ABC_TYPE_INIT` → no-op (return TRUE)
- `type == SWITCH_ABC_TYPE_CLOSE` → call `manager->Detach(ctx->bug_id)`,
  return TRUE
- otherwise → if `ctx->user_cb` is set, invoke it; else return TRUE

Track C populates `ctx->user_cb` per-handler. Track A's tests use a
test-only callback that counts invocations.

---

## Channel state handler — DetachAll on CS_DESTROY

```cpp
// In bug_manager.cc, file-static (FF-003):
extern "C" switch_status_t OswMediaChannelDestroy(
    switch_core_session_t* session) noexcept;

// Registered once at module load:
void MediaBugManager::RegisterStateHandlers() noexcept;
```

`OswMediaChannelDestroy` recovers the channel UUID via
`switch_core_session_get_uuid(session)` (FF-031 / new FF-032 to be added)
and calls `MediaBugManager::Instance().DetachAll(uuid)`. The instance is
obtained via the same singleton pattern as `osw::Module::Instance()` —
make the manager a member of `Module` and expose a global accessor for
the C trampoline.

---

## Acceptance criteria

| # | Scenario | Expected |
|---|---|---|
| A1 | Attach STT → TTS → second STT | A1.3 returns `kAlreadyExists` |
| A2 | Attach STT (MID_READ) → TTS (INJECT) | both succeed, chain order in mock = `[STT, TTS]` |
| A3 | Attach TTS (INJECT) → STT (MID_READ) without SMBF_FIRST | A3.2 returns `kFailedPrecondition` |
| A4 | Attach STT (MID_READ) → VAD (EARLY) | A4.2 succeeds (manager OR's in `SMBF_FIRST`); mock sees `SMBF_FIRST` in flags |
| A5 | `BugHandle` destructor on dropped handle | mock `switch_core_media_bug_remove_callback` called with matching function name |
| A6 | `DetachAll(uuid)` after 3 attaches | 3 `_remove_callback` calls, manager state empty |
| A7 | `DetachAll(uuid)` twice in a row | second call is no-op (idempotent) |
| A8 | `Detach(unknown_id)` | silent no-op |
| A9 | Concurrent attach/detach (16 threads × 100 ops, distinct channel UUIDs) | no crash, all attached bugs eventually detached, TSAN-clean |

Use the existing `osw_add_unit_test` CMake helper with
`LABEL "media;unit;w6a"` and `DEFINES OSW_TEST_FS_MOCK=1`. Mirror the
mock-seam wiring in `tests/unit/control/CMakeLists.txt` for handler
tests.

---

## FF entries to add

Append to `openspec/changes/core-module-v1/FREESWITCH-FACTS.md`:

```
## FF-031 — `switch_core_media_bug_add` signature + ownership

Signature (FS 1.10.12):
  switch_status_t switch_core_media_bug_add(
      switch_core_session_t *session,
      const char *function,
      const char *target,
      switch_media_bug_callback_t callback,
      void *user_data,
      time_t stop_time,
      switch_media_bug_flag_t flags,
      switch_media_bug_t **new_bug);

- `function` is the function-name string (used by FF-008 _count and the
  _remove_callback filter); we use kFunctionName = "mod_open_switch".
- `target` is opaque metadata; pass a short identifier (e.g. purpose
  name) for log clarity.
- `user_data` is caller-owned. Must outlive the bug — typically the
  BugHandle holds a unique_ptr<BugCallbackContext> that is moved into
  by_id_ on successful attach.
- `*new_bug` is FS-owned; do NOT delete. We retain a non-owning ptr
  for log / debug only.

## FF-032 — Channel state handler `CS_DESTROY` ordering

Register via `switch_core_event_hook_add_state_change`. The callback
fires for EVERY channel transition; filter on
`switch_channel_get_state(channel) == CS_DESTROY` and return
SWITCH_STATUS_SUCCESS. CS_DESTROY runs AFTER the channel's bugs have
been closed by FS-side hangup processing, but before the session is
freed — so DetachAll here is a defensive cleanup that drops any
BugHandle records the manager still tracks. The bug pointers stored
in BugRecord are no longer valid at this point; do NOT call
switch_core_media_bug_remove_callback from DetachAll on CS_DESTROY,
only erase records.
```

---

## Build + test (sub-agent runs locally before push)

```bash
cd /tmp/open-switch-w6a    # worktree
docker buildx build \
  --build-arg OSW_ENABLE_ASAN=OFF --build-arg OSW_BUILD_TESTS=ON \
  --build-arg BUILD_TYPE=Debug --build-arg BASE_TAG=1.10.12-trixie \
  --target fs-builder \
  -f deploy/docker/Dockerfile.builder \
  -t open-switch/builder:w6a --load .
docker run --rm open-switch/builder:w6a \
  ctest --test-dir /usr/src/open-switch/build --output-on-failure -L unit

# Clang-format gate (CI uses silkeh/clang:18 bit-for-bit)
docker run --rm -v "$PWD":/work -w /work silkeh/clang:18 \
  clang-format -i $(git diff --name-only main | grep -E '\.(cc|h)$')
```

Commit message (no AI co-author trailers — contributor is @luongdev):
> `feat(media): MediaBugManager + stage-rank enforcement + RAII BugHandle`

Push: `git push -u origin implementation/wave6-track-a-bug-manager`.
