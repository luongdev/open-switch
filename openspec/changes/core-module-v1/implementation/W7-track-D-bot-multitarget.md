# W7 Track D — `StartBot` multi-target bug attachment

## Owner / model

Sonnet sub-agent. Architectural decision in
[`tasks.md`](../tasks.md) §"V1 scope confirmed (W6.5 architectural
decision)"; the sub-agent reads this brief as authoritative
implementation scope and does NOT redesign.

## Scope

Replace the W6 legacy per-channel single-target streaming RPCs
(`StartTts` / `StartStt` / `StartVoicebot`) with **`StartBot`** — a
single RPC that handles 1..N target channels with a single bot
identity, a single bidi gRPC stream, and module-owned bug lifecycle
on every target.

Per the V1 architectural decision (max 3-way: 1 bot + 1 KH + 1 Agent),
the design is:

- One **bot logical entity** per `StartBot` invocation, identified by
  a module-minted `bot_id` (UUIDv7), NOT a FreeSWITCH session UUID.
- Per `(bot, target_channel)` pair the module attaches:
  - One `SMBF_READ_STREAM` bug if the bot purpose includes
    listening (STT_LISTEN, VOICEBOT_DUPLEX).
  - One `SMBF_WRITE_REPLACE` bug if the bot purpose includes
    speaking (TTS_BROADCAST, VOICEBOT_DUPLEX, WHISPER — restricted to
    `write_target_channel_uuids`).
- One **bidi gRPC stream** per `bot_id` to the upstream service.
  Inbound (READ side) frames carry a `channel_uuid` tag so the
  upstream demultiplexes which target spoke. Outbound (server-to-
  module) audio is **fan-out** by the module into per-target SPSC
  queues; each target's WRITE_REPLACE callback pops from its own
  queue.
- **Passthrough when bot is silent** (FF-036): a WRITE_REPLACE
  callback whose per-target queue is empty returns `SWITCH_TRUE`
  without calling `switch_core_media_bug_set_write_replace_frame()`.
  FS's `write_replace_frame_out` was initialised to the input frame
  before the callback ran, so the unmodified out-pointer naturally
  passes the original frame through. The target hears whatever
  source FS has driving the write side (bridge audio, silence
  driver, hold music — see W6.6 for the silence-driver hotfix).

The W6 legacy RPCs (`StartTts`, `StartStt`, `StartVoicebot`,
`StopMediaStream`) **stay shippable** in V1 with single-target
semantics (legacy IVR mode: bot + KH only). New use cases MUST use
`StartBot`. See [`W6-track-C-handlers.md`](W6-track-C-handlers.md)
§"Legacy / Deprecation" for the migration matrix.

## Dependencies (HARD)

Track D **cannot** ship without the W6.5 fix-sprint having landed
first on `redesign/from-specs`:

- W6-P1-001 trampoline must forward the real `switch_media_bug_t*`
  (not `nullptr`). Multi-target Track D code calls
  `switch_core_media_bug_get_write_replace_frame(bug, ...)` and
  `switch_core_media_bug_set_write_replace_frame(bug, ...)` per
  target; passing `nullptr` for `bug` would crash on the first
  ptime tick of the first target.
- W6-P1-002 UAF in `ActiveMediaStreams::TearDown` must be closed.
  Track D adds its own bot-session registry (`ActiveBots`), but it
  reuses the same teardown ordering pattern; the broken pattern
  must be fixed in the W6 code before Track D copies it.
- W6-P1-003 `RegisterStateHandlers()` stub must be replaced with a
  real `CS_DESTROY` hook that routes through the module-owned
  manager. Track D's `ActiveBots` registry hooks into the same
  channel-state hook for bot cleanup on hangup.
- W6-P1-004 `Detach` must store the exact `switch_media_bug_t*` and
  remove by that pointer (not by callback function). Track D
  attaches multiple bugs per session with the same trampoline
  callback; the broken multi-bug-removal would tear down every bot
  bug on a single Stop.
- W6-P1-005 `StreamClient::Open()` must NOT set the long-lived
  context's deadline to a 5 s open timeout. Bot sessions can last
  hours.
- W6-P2-003 Resampler integration must be wired into the
  read/write callback path. Track D's per-target callbacks rely on
  the resampler per direction per target.

If the orchestrator spawns this Track D sub-agent before W6.5 fix-
sprint merges, the sub-agent MUST stop and report blocked.

## Files in scope

```text
proto/open_switch/control/v1/control.proto       (PATCH — add StartBot / StopBot RPCs + messages)

include/osw/media/bot_session.h                  (new — public API for one bot)
include/osw/media/bug_fanout.h                   (new — fan-out router)
src/media/bot_session.cc                         (new — BotSession lifecycle)
src/media/bug_fanout.cc                          (new — TtsPlayoutBuffer → N per-target SPSC queues)
src/media/CMakeLists.txt                         (PATCH — list new TUs)

include/osw/control/active_bots.h                (new — bot registry)
src/control/active_bots.cc                       (new — registry impl)

include/osw/control/handlers/start_bot_handler.h (new)
include/osw/control/handlers/stop_bot_handler.h  (new)
src/control/handlers/start_bot_handler.cc        (new — multi-target attach + stream open)
src/control/handlers/stop_bot_handler.cc         (new — drain + detach all bugs + close stream)
src/control/handlers/media_bug_callbacks.cc      (PATCH — add per-bot multi-target callbacks alongside W6 legacy ones)
src/control/handlers/media_bug_callbacks.h       (PATCH — declare new callbacks)
src/control/CMakeLists.txt                       (PATCH — list new TUs)

src/mod_open_switch.cc                           (PATCH — Load step + Shutdown step)

include/osw/config/module_config.h               (PATCH — add 4 new bot-related fields)
src/config/module_config.cc                      (PATCH — XML parse + Validate)

src/events/tier.cc                               (PATCH — Tier-1 allowlist for new audit subclasses)

tests/unit/media/bot_session_test.cc             (new — FS-mock)
tests/unit/media/bug_fanout_test.cc              (new — pure unit, no FS)
tests/unit/control/active_bots_test.cc           (new)
tests/unit/control/start_bot_handler_test.cc     (new — FS-mock)
tests/unit/control/stop_bot_handler_test.cc      (new)
tests/integration/bot_single_target.cc           (new)
tests/integration/bot_two_target_broadcast.cc    (new)
tests/integration/bot_whisper_subset.cc          (new)
tests/integration/bot_hangup_cleanup.cc          (new)
tests/integration/bot_passthrough_when_silent.cc (new)
```

## Spec deltas

### `control.proto` — 2 new RPCs

```protobuf
service Control {
  // ... existing 12 RPCs + W6 4 streaming RPCs ...

  // W7 Track D: Multi-target bot.  Attaches one logical bot to one
  // or more target channels.  Reuses one upstream gRPC stream;
  // multiplexes READ-side frames (tagged with channel_uuid);
  // fans-out WRITE-side frames into per-target SPSC queues.
  //
  // Passthrough when bot is silent: WRITE_REPLACE callback returns
  // SWITCH_TRUE without setting the replace frame.  Bridge audio /
  // silence driver flow through unchanged.  See FF-036.
  //
  // Refused with FAILED_PRECONDITION if any target channel already
  // has the same bot purpose attached (1 bot of each purpose per
  // channel max).  Refused with NOT_FOUND if any target channel
  // does not exist.
  rpc StartBot(StartBotRequest) returns (StartBotResponse);

  // Stops a bot.  Detaches all bugs on all targets + half-closes the
  // gRPC stream + drains in-flight frames up to drain_timeout_ms +
  // cancels the stream.  Idempotent (stops with success on unknown
  // bot_id).
  rpc StopBot(StopBotRequest) returns (StopBotResponse);
}

message StartBotRequest {
  common.v1.RequestHeader header = 1;

  // Target channels for this bot.
  //   [KH_uuid]             — IVR-style 2-party (bot + KH)
  //   [KH_uuid, Agent_uuid] — 3-way (bot + KH + Agent)
  //   [Agent_uuid]          — bot whispering to Agent only
  // V1 supports up to 2 targets per call (matching the 1+KH+Agent
  // ship gate).  Larger N is V2 (conference scope).
  repeated string target_channel_uuids = 2;

  // gRPC URL of the upstream service (TTS / STT / voicebot).
  string upstream_endpoint = 3;

  // What this bot does on each target.
  enum Purpose {
    PURPOSE_UNSPECIFIED = 0;
    TTS_BROADCAST = 1;      // one-way: bot → all targets (server speaks)
    STT_LISTEN = 2;         // one-way: all targets → bot (server transcribes)
    VOICEBOT_DUPLEX = 3;    // bidirectional: targets speak + bot speaks
    WHISPER = 4;            // bidirectional: targets listen-only; bot
                            //   speaks only to write_target_channel_uuids
  }
  Purpose purpose = 4;

  // Used only when purpose=WHISPER.  Must be a subset of
  // target_channel_uuids.  Targets not in this subset get a
  // READ_STREAM bug only (no WRITE_REPLACE), so they cannot hear the
  // bot's audio.  Default (empty) is rejected for WHISPER with
  // INVALID_ARGUMENT.
  repeated string write_target_channel_uuids = 5;

  // Sample format hints; module guarantees these on outgoing frames.
  uint32 sample_rate_hz = 6;             // typical 8000 / 16000

  // Opening utterance for TTS_BROADCAST / VOICEBOT_DUPLEX (passed
  // through to the upstream service as the StreamStart.start_message
  // field).  Empty for STT_LISTEN.
  string start_message = 7;

  // Engine-side context (passed through to the upstream handler).
  map<string, string> variables = 8;

  // Per-bot jitter-buffer override.  Same shape as the W6 single-
  // stream override.
  open_switch.media.v1.TtsBufferOverride buffer_override = 20;

  // Optional drain timeout on Stop (ms).  Default = module config
  // tts_drain_timeout_ms (typically 2000 ms).
  uint32 drain_timeout_ms = 30;
}

message StartBotResponse {
  string bot_id = 1;          // module-minted UUIDv7
  uint32 negotiated_rate_hz = 2;
  ErrorDetail error = 99;
}

message StopBotRequest {
  common.v1.RequestHeader header = 1;
  string bot_id = 2;          // returned by StartBot
}

message StopBotResponse {
  bool was_active = 1;        // false on unknown bot_id (idempotent OK)
  ErrorDetail error = 99;
}
```

### `module_config.h` — 4 new fields

```cpp
struct ModuleConfig {
    // ... existing W6 fields ...

    // === W7 Track D bot multi-target ===
    // Max number of target channels per bot.  Hard-capped at 2 in
    // V1 (matches 1+KH+Agent ship gate).  V2 raises this for
    // conference scope.
    std::uint32_t bot_max_targets = 2;

    // Per-target fan-out queue capacity in ms of audio.  When a
    // target's WRITE_REPLACE pulls slower than the gRPC reader
    // pushes, the queue fills; this is the high-water mark before
    // drop-oldest kicks in.  Typical 500 ms.
    std::uint32_t bot_target_queue_ms = 500;

    // Drain timeout on StopBot.  Module half-closes the upstream
    // stream and waits up to this ms for the server to finish
    // before forcibly cancelling.  Typical 2000 ms.
    std::uint32_t bot_drain_timeout_ms = 2000;

    // Hard cap on simultaneous bots per channel.  Default 1 (V1
    // matches max 1 bot per call).  V2 may raise.
    std::uint32_t max_bots_per_channel = 1;
};
```

`Validate()` clamps:

- `bot_max_targets ∈ [1, 8]`. Out of range → coerce to 2 + WARN.
- `bot_target_queue_ms ∈ [50, 5000]`. Out of range → coerce + WARN.
- `bot_drain_timeout_ms ∈ [100, 10000]`. Out of range → coerce + WARN.
- `max_bots_per_channel ∈ [1, 4]`. Out of range → coerce + WARN.

### Audit subclasses

| Subclass | Tier | Trigger |
|---|---|---|
| `osw.media.bot.started` | 1 | `StartBot` succeeded; all bugs attached + stream opened |
| `osw.media.bot.stopped` | 1 | `StopBot` succeeded OR channel hangup → DetachAll OR fatal stream error |
| `osw.media.bot.target_drop` | 2 | A single target's fan-out queue dropped a frame (rate-limited 1/min per bot+target pair) |
| `osw.media.bot.target_attach_failed` | 1 | Could not attach one of the bugs to a target (partial-failure path; emitted before unwinding the others) |

Add to Tier-1 / Tier-2 allowlists in `src/events/tier.cc`.

## Implementation steps

### 1. `osw::media::BugFanout` (pure unit, no FS deps)

`include/osw/media/bug_fanout.h`:

```cpp
namespace osw::media {

/// Per-target SPSC queue with timestamps + bounded capacity.
struct TargetQueue {
    std::string channel_uuid;
    std::deque<AudioFrame> queue;    // bounded by capacity_frames_
    std::mutex mu_;                  // guarded so producer (gRPC reader) and
                                     //   consumer (FS write thread) can race.
                                     //   See "Lock-free alternative" below.
};

/// Routes one inbound stream of AudioFrames to N per-target SPSC queues.
/// Two routing modes:
///   - BROADCAST   — every frame goes to every target's queue.
///   - WHISPER     — frames go only to targets in write_subset.
class BugFanout {
 public:
    enum class Mode { kBroadcast, kWhisper };

    struct Config {
        Mode mode;
        std::vector<std::string> target_uuids;            // all targets
        std::vector<std::string> write_subset_uuids;      // WHISPER only
        std::uint32_t capacity_frames;                    // per-target bound
    };

    explicit BugFanout(Config cfg);
    ~BugFanout();

    BugFanout(const BugFanout&) = delete;
    BugFanout& operator=(const BugFanout&) = delete;

    /// Push one PCM frame from the gRPC reader.  Internal copy + enqueue
    /// onto each target queue per the mode.  Drops oldest on overflow,
    /// returns count of (target, dropped-frame) events.
    std::uint64_t Push(AudioFrame frame) noexcept;

    /// Pop one frame from a target's queue (for that target's WRITE_REPLACE
    /// callback).  Returns std::nullopt if the queue is empty (callback
    /// should then passthrough per FF-036).
    [[nodiscard]] std::optional<AudioFrame>
        Pop(std::string_view channel_uuid) noexcept;

    /// Half-close: no more pushes allowed; pops still drain the queues.
    void HalfClose() noexcept;

    /// Stats.
    [[nodiscard]] std::uint64_t TotalDropped() const noexcept;
    [[nodiscard]] std::uint64_t QueueDepth(std::string_view channel_uuid) const noexcept;

 private:
    Config cfg_;
    std::vector<std::unique_ptr<TargetQueue>> queues_;
    std::unordered_map<std::string, TargetQueue*> by_uuid_;
    std::atomic<bool> half_closed_{false};
    std::atomic<std::uint64_t> total_dropped_{0};
};

}  // namespace osw::media
```

**Lock-free alternative**: per-target `std::deque` under `std::mutex`
is what the spec mandates here for V1 simplicity. A true SPSC queue
(folly's `ProducerConsumerQueue` shape) is acceptable as an
implementation upgrade later; the public API of `BugFanout` does not
change. The reason for the mutex in V1: FS media threads are
single-producer (the gRPC reader pushes from the gRPC thread; per-
target WRITE_REPLACE callbacks pop from the FS media thread for that
target). The mutex is contended only when both threads touch the same
queue at the same instant — sub-microsecond contention in practice.
Track gemini-W6 noted the issue; V1.5 may convert if profiling
shows it matters.

**Tests** (`bug_fanout_test.cc`, pure unit):

- `BroadcastMode_PushDeliversToAll`: 2 targets, push 1 frame → both
  queues have 1.
- `WhisperMode_PushDeliversToSubsetOnly`: 2 targets, write_subset =
  [target_2]; push 1 frame → target_1 queue empty, target_2 has 1.
- `Capacity_DropsOldestOnOverflow`: capacity=4, push 5 → oldest
  dropped, queue has 4, `TotalDropped() == 1`.
- `HalfClose_NoMorePushes`: HalfClose then Push → returns 0,
  `total_dropped_` unchanged.
- `HalfClose_PopStillDrains`: queue has 3, HalfClose, Pop x 3 → all
  3 returned.
- `EmptyQueue_PopReturnsNullopt`: no frames pushed; Pop returns
  `std::nullopt`.
- `ConcurrentPushPop_TsanClean`: spawn 2 threads (one Push 1000
  frames, one Pop 1000 times) → no races (TSAN clean).

### 2. `osw::media::BotSession`

`include/osw/media/bot_session.h`:

```cpp
namespace osw::media {

struct BotSessionConfig {
    std::string bot_id;                                  // module-minted
    std::string tenant_id;
    std::string upstream_endpoint;
    std::vector<std::string> target_channel_uuids;
    std::vector<std::string> write_target_channel_uuids; // WHISPER only
    osw::control::v1::StartBotRequest::Purpose purpose;
    std::uint32_t sample_rate_hz;
    std::string start_message;
    std::map<std::string, std::string> variables;
    osw::media::TtsPlayoutBuffer::Config buffer_cfg;     // from W6 fix-sprint
    std::uint32_t drain_timeout_ms;
};

/// One bot logical entity.  Owns:
///   - the upstream bidi gRPC stream (one per bot)
///   - the per-target bug handles (one or two per target)
///   - the BugFanout (gRPC reader → per-target queues)
///   - per-direction Resamplers per target
class BotSession {
 public:
    BotSession(BotSessionConfig cfg,
               std::shared_ptr<grpc::Channel> chan);
    ~BotSession();

    BotSession(const BotSession&) = delete;
    BotSession& operator=(const BotSession&) = delete;

    /// Phase 2: attach bugs to all targets.  Returns the first error
    /// encountered; on partial failure, unwinds any bugs already
    /// attached.
    [[nodiscard]] absl::Status Attach(osw::media::MediaBugManager& mgr) noexcept;

    /// Phase 3: half-close + drain + cancel.  Idempotent.
    void Stop() noexcept;

    /// Called by the read-tap callback when a target speaks.  Sends
    /// the frame upstream with `channel_uuid` tag so the server can
    /// demultiplex.
    void OnTargetReadFrame(std::string_view channel_uuid,
                          std::uint64_t fs_ts,
                          std::span<const std::int16_t> samples);

    /// Called by the write-replace callback when a target needs an
    /// audio frame.  Returns the next frame for that target, or
    /// nullopt if the queue is empty (passthrough per FF-036).
    [[nodiscard]] std::optional<AudioFrame>
        PopWriteFrame(std::string_view channel_uuid);

    bool IsStopped() const noexcept;

    /// Stats.
    std::uint64_t FramesSentUpstream() const noexcept;
    std::uint64_t FramesReceivedFromUpstream() const noexcept;
    std::uint64_t TargetDropCount() const noexcept;

    const std::vector<std::string>& TargetUuids() const noexcept;
    const std::vector<BugHandle>& BugHandles() const noexcept;

 private:
    BotSessionConfig cfg_;
    std::unique_ptr<osw::media::StreamClient> client_;      // from W6 Track B
    std::unique_ptr<osw::media::BugFanout> fanout_;          // shared input source
    std::vector<BugHandle> handles_;                         // 1-2 bugs per target
    std::unordered_map<std::string, std::unique_ptr<Resampler>> read_resamplers_;
    std::unordered_map<std::string, std::unique_ptr<Resampler>> write_resamplers_;
    std::atomic<bool> stopped_{false};
    std::atomic<std::uint64_t> frames_sent_{0};
    std::atomic<std::uint64_t> frames_recv_{0};
};

}  // namespace osw::media
```

**Critical lifecycle ordering** (mirrors W6.5 fix-sprint pattern after
P1-002 is closed):

1. `~BotSession`: call `Stop()` (idempotent).
2. `Stop()` body:
   - `stopped_ = true` (atomic; callbacks short-circuit on read).
   - `handles_.clear()` — this triggers `~BugHandle` → `MediaBugManager::
     Detach` → `switch_core_media_bug_remove(session, &bug)` for the
     EXACT bug pointer per target (W6.5 P1-004 fix). FS may still
     run the callback once more before remove takes effect; the
     callback handles that case via `stopped_` flag + safe nullptr
     guards.
   - `fanout_->HalfClose()` — no more frames can land in queues.
   - `client_->HalfCloseAndDrain(cfg_.drain_timeout_ms)` — sends
     WritesDone, waits up to drain_timeout_ms for upstream to
     half-close, then `TryCancel()` (W6.5 P1-006 fix pattern).
   - Resamplers destruct via `unique_ptr`; `fanout_` likewise.

3. **DO NOT** delete the bot session object before bugs are detached.
   The `ActiveBots` registry owns the `unique_ptr<BotSession>` and
   only frees it after `Stop()` returns. (W6.5 P1-002 UAF lesson.)

### 3. `osw::control::ActiveBots` (registry)

`include/osw/control/active_bots.h`:

```cpp
namespace osw::control {

/// Registry of all active bots, keyed by bot_id (UUIDv7).
class ActiveBots {
 public:
    /// Register a new bot.  Caller transfers ownership.
    void Register(std::string bot_id, std::unique_ptr<osw::media::BotSession> bot);

    /// Lookup by bot_id (returns nullptr if not found).
    osw::media::BotSession* Find(std::string_view bot_id) const noexcept;

    /// Stop and remove a bot.  Idempotent (returns false if not found).
    bool Stop(std::string_view bot_id);

    /// Stop all bots on a channel (called from CS_DESTROY state hook).
    std::size_t StopByChannel(std::string_view channel_uuid);

    /// Drain everything (called from Module::Shutdown).
    void DrainAll(std::uint32_t drain_timeout_ms);

    /// Stats.
    std::size_t ActiveCount() const noexcept;

 private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<osw::media::BotSession>> bots_;
    std::unordered_map<std::string, std::vector<std::string>> by_channel_;
    //                ^ channel_uuid              ^ bot_ids on that channel
};

}  // namespace osw::control
```

**Hangup integration**: the CS_DESTROY state handler (W6.5 P1-003
fix) calls `ActiveBots::StopByChannel(channel_uuid)` for every
channel that destroys. This is one of two paths — the other is the
explicit `StopBot` RPC. Both end in `BotSession::Stop()` which is
idempotent.

### 4. Bug callbacks (file-static, like W6 Track C)

`src/control/handlers/media_bug_callbacks.cc` ADDITIONS:

```cpp
// W7 Track D — per-target bot read tap.  user_data is a heap-
// allocated BotReadTapCtx that lives for the bug's lifetime; owned
// by the bug.
extern "C" switch_bool_t OswBotReadTap(switch_media_bug_t* bug,
                                       void* user_data,
                                       switch_abc_type_t type) noexcept {
    auto* ctx = static_cast<BotReadTapCtx*>(user_data);
    if (!ctx || !ctx->bot || ctx->bot->IsStopped()) return SWITCH_TRUE;
    try {
        if (type == SWITCH_ABC_TYPE_READ) {
            // Per W6.5 P1-001 fix: bug is now a real pointer, not nullptr.
            switch_frame_t* frame = nullptr;
            if (switch_core_media_bug_read(bug, &frame, SWITCH_FALSE)
                    == SWITCH_STATUS_SUCCESS && frame) {
                ctx->bot->OnTargetReadFrame(
                    ctx->channel_uuid,
                    frame->timestamp,
                    std::span<const std::int16_t>(
                        reinterpret_cast<const std::int16_t*>(frame->data),
                        frame->datalen / 2));
            }
        } else if (type == SWITCH_ABC_TYPE_CLOSE) {
            // Channel hangup or detach.  Mark the bot's per-target
            // queue as drained; the BotSession dtor will tear down
            // the gRPC stream if this was the last target.
            ctx->bot->OnTargetClose(ctx->channel_uuid, /*direction=*/0);
        }
        return SWITCH_TRUE;
    } catch (...) {
        // W6.5 / FF-036 reminder: returning SWITCH_FALSE would PRUNE
        // the bug.  We log and return SWITCH_TRUE to keep the bug
        // alive; the exception was in our code, not FS.
        osw::log::Error("OswBotReadTap: exception on channel {}",
                        ctx ? ctx->channel_uuid : "?");
        return SWITCH_TRUE;
    }
}

// W7 Track D — per-target bot write replace.  user_data is a heap-
// allocated BotWriteReplaceCtx.
extern "C" switch_bool_t OswBotWriteReplace(switch_media_bug_t* bug,
                                            void* user_data,
                                            switch_abc_type_t type) noexcept {
    auto* ctx = static_cast<BotWriteReplaceCtx*>(user_data);
    if (!ctx || !ctx->bot || ctx->bot->IsStopped()) return SWITCH_TRUE;
    try {
        if (type == SWITCH_ABC_TYPE_WRITE_REPLACE) {
            auto frame_opt = ctx->bot->PopWriteFrame(ctx->channel_uuid);
            if (!frame_opt.has_value()) {
                // Empty queue → passthrough per FF-036.
                // Returning SWITCH_TRUE without calling
                // switch_core_media_bug_set_write_replace_frame()
                // makes FS keep the original write_frame_out
                // (initialised to input frame before this callback ran).
                return SWITCH_TRUE;
            }
            // Have a frame.  Set the replacement.
            switch_frame_t replace = {};
            replace.data = const_cast<std::uint8_t*>(frame_opt->payload.data());
            replace.datalen = static_cast<std::uint32_t>(frame_opt->payload.size());
            replace.samples = frame_opt->duration_samples;
            replace.rate = frame_opt->sample_rate_hz;
            switch_core_media_bug_set_write_replace_frame(bug, &replace);
            return SWITCH_TRUE;
        } else if (type == SWITCH_ABC_TYPE_CLOSE) {
            ctx->bot->OnTargetClose(ctx->channel_uuid, /*direction=*/1);
        }
        return SWITCH_TRUE;
    } catch (...) {
        osw::log::Error("OswBotWriteReplace: exception on channel {}",
                        ctx ? ctx->channel_uuid : "?");
        return SWITCH_TRUE;
    }
}
```

`BotReadTapCtx` / `BotWriteReplaceCtx`:

```cpp
struct BotReadTapCtx {
    osw::media::BotSession* bot;     // non-owning; ActiveBots owns the BotSession
    std::string channel_uuid;
    // No per-callback seq/ts state: it lives in BotSession's
    // per-target Resampler (W6.5 P4-001 fix: no thread_local).
};

struct BotWriteReplaceCtx {
    osw::media::BotSession* bot;
    std::string channel_uuid;
};
```

These are heap-allocated by the `StartBot` handler on attach,
freed by `BotSession::Stop()` after `Detach` completes (mirrors
W6.5 P1-002 fix ordering).

### 5. `StartBot` handler

```cpp
// src/control/handlers/start_bot_handler.cc
::grpc::Status StartBotHandler::Handle(
    ::grpc::ServerContext* ctx,
    const StartBotRequest* req,
    StartBotResponse* resp) {
    // 1. Validate args.
    if (req->target_channel_uuids().empty()) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                              "target_channel_uuids required (≥1)");
    }
    if (static_cast<std::uint32_t>(req->target_channel_uuids().size())
            > module_config_->bot_max_targets) {
        return ::grpc::Status(::grpc::StatusCode::RESOURCE_EXHAUSTED,
                              "too many targets; module cap is " +
                                  std::to_string(module_config_->bot_max_targets));
    }
    if (req->purpose() == StartBotRequest::PURPOSE_UNSPECIFIED) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                              "purpose required");
    }
    if (req->purpose() == StartBotRequest::WHISPER &&
        req->write_target_channel_uuids().empty()) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                              "WHISPER requires write_target_channel_uuids");
    }
    // Subset check.
    if (req->purpose() == StartBotRequest::WHISPER) {
        std::unordered_set<std::string> all_targets(
            req->target_channel_uuids().begin(),
            req->target_channel_uuids().end());
        for (const auto& w : req->write_target_channel_uuids()) {
            if (all_targets.find(w) == all_targets.end()) {
                return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                      "write target " + w +
                                          " not in target_channel_uuids");
            }
        }
    }
    if (req->upstream_endpoint().empty()) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                              "upstream_endpoint required");
    }

    // 2. Idempotency cache check (W5 Track B pattern).
    // ...

    // 3. Validate each target channel exists.
    std::vector<osw::raii::SessionLock> session_locks;
    session_locks.reserve(req->target_channel_uuids().size());
    for (const auto& uuid : req->target_channel_uuids()) {
        osw::raii::SessionLock lock(uuid);
        if (!lock) {
            return ::grpc::Status(::grpc::StatusCode::NOT_FOUND,
                                  "channel not found: " + uuid);
        }
        session_locks.push_back(std::move(lock));
    }

    // 4. Mint bot_id + open upstream gRPC stream.
    const std::string bot_id = osw::events::GenerateUuidV7();
    auto chan = grpc_channel_pool_->GetOrCreate(req->upstream_endpoint());
    if (!chan) {
        return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE,
                              "grpc channel pool busy");
    }

    osw::media::BotSessionConfig cfg = {
        .bot_id = bot_id,
        .tenant_id = req->header().tenant_id(),
        .upstream_endpoint = req->upstream_endpoint(),
        .target_channel_uuids = {req->target_channel_uuids().begin(),
                                 req->target_channel_uuids().end()},
        .write_target_channel_uuids = {req->write_target_channel_uuids().begin(),
                                       req->write_target_channel_uuids().end()},
        .purpose = req->purpose(),
        .sample_rate_hz = req->sample_rate_hz() ? req->sample_rate_hz() : 8000u,
        .start_message = req->start_message(),
        .variables = {req->variables().begin(), req->variables().end()},
        // ... buffer_cfg merging with module defaults ...
        .drain_timeout_ms = req->drain_timeout_ms() ?
            req->drain_timeout_ms() : module_config_->bot_drain_timeout_ms,
    };

    auto bot = std::make_unique<osw::media::BotSession>(std::move(cfg),
                                                        std::move(chan));

    // 5. Attach bugs.  Partial-failure path unwinds inside BotSession::Attach.
    if (auto st = bot->Attach(*bug_manager_); !st.ok()) {
        // Audit the partial failure before returning.
        osw::audit::Builder b("osw.media.bot.target_attach_failed",
                              osw::events::Tier::k1Durable);
        b.Header("bot_id", bot_id);
        b.Header("tenant_id", req->header().tenant_id());
        b.Header("error", std::string(st.message()));
        osw::audit::Emit(std::move(b));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL,
                              std::string(st.message()));
    }

    // 6. Register with ActiveBots.
    active_bots_->Register(bot_id, std::move(bot));

    // 7. Emit bot.started audit.
    osw::audit::Builder b("osw.media.bot.started", osw::events::Tier::k1Durable);
    b.Header("bot_id", bot_id);
    b.Header("tenant_id", req->header().tenant_id());
    b.Header("purpose", PurposeToString(req->purpose()));
    b.Header("target_count", std::to_string(req->target_channel_uuids().size()));
    for (int i = 0; i < req->target_channel_uuids().size(); ++i) {
        b.Header("target_" + std::to_string(i),
                 req->target_channel_uuids(i));
    }
    osw::audit::Emit(std::move(b));

    resp->set_bot_id(bot_id);
    resp->set_negotiated_rate_hz(cfg.sample_rate_hz);
    return ::grpc::Status::OK;
}
```

### 6. `StopBot` handler

```cpp
::grpc::Status StopBotHandler::Handle(
    ::grpc::ServerContext* ctx,
    const StopBotRequest* req,
    StopBotResponse* resp) {
    if (req->bot_id().empty()) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                              "bot_id required");
    }
    const bool was_active = active_bots_->Stop(req->bot_id());

    if (was_active) {
        osw::audit::Builder b("osw.media.bot.stopped", osw::events::Tier::k1Durable);
        b.Header("bot_id", req->bot_id());
        b.Header("tenant_id", req->header().tenant_id());
        b.Header("reason", "explicit_stop");
        osw::audit::Emit(std::move(b));
    }
    resp->set_was_active(was_active);
    return ::grpc::Status::OK;
}
```

### 7. Module wiring

`src/mod_open_switch.cc`:

```cpp
SWITCH_MODULE_LOAD_FUNCTION(mod_open_switch_load) {
    // ... existing W6 wiring ...
    // === W7 Track D: bot registry ===
    g_active_bots_ = std::make_unique<osw::control::ActiveBots>();
    RegisterStartBotHandler(server_builder, *g_active_bots_, *g_bug_manager_,
                             *g_grpc_channel_pool_, *g_module_config_);
    RegisterStopBotHandler(server_builder, *g_active_bots_);
    // ... rest of Load ...
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_open_switch_shutdown) {
    // ... existing Shutdown steps ...
    // === W7 Track D: drain active bots before MediaBugManager teardown ===
    g_active_bots_->DrainAll(g_module_config_->bot_drain_timeout_ms);
    g_active_bots_.reset();
    // ... rest of Shutdown ...
}
```

Update the CS_DESTROY state handler (W6.5 P1-003 fix introduces it):

```cpp
static void on_channel_destroy(switch_core_session_t* session) {
    const char* uuid = switch_core_session_get_uuid(session);
    if (!uuid) return;
    // W6.5 fix: route through both registries.
    g_active_media_streams_->RemoveForChannel(uuid);
    g_active_bots_->StopByChannel(uuid);
    g_bug_manager_->DetachAll(uuid);
}
```

## Acceptance tests

| ID | Name | What it proves |
|---|---|---|
| D1 | `bug_fanout_BroadcastMode_PushDeliversToAll` | Multi-target fan-out works |
| D2 | `bug_fanout_WhisperMode_PushDeliversToSubsetOnly` | Whisper restricts WRITE side |
| D3 | `bug_fanout_Capacity_DropsOldestOnOverflow` | Bounded queue |
| D4 | `bug_fanout_ConcurrentPushPop_TsanClean` | Race-free under load |
| D5 | `bot_session_AttachAllTargets_Happy` | Attach succeeds across 2 targets |
| D6 | `bot_session_AttachPartialFailure_Unwinds` | Partial attach failure rolls back |
| D7 | `bot_session_Stop_Idempotent` | Multiple Stop() calls are safe |
| D8 | `bot_session_PassthroughWhenSilent` | Empty queue → callback returns SWITCH_TRUE without setting frame |
| D9 | `active_bots_StopByChannel_Cleans` | CS_DESTROY → all bots on channel stop |
| D10 | `active_bots_DrainAll_Waits` | Module Shutdown drains gracefully |
| D11 | `start_bot_handler_HappyPath_TwoTargets` | RPC end-to-end with 2 targets |
| D12 | `start_bot_handler_TooManyTargets_ResourceExhausted` | bot_max_targets enforced |
| D13 | `start_bot_handler_WhisperWithoutSubset_InvalidArgument` | WHISPER requires write_subset |
| D14 | `start_bot_handler_WhisperSubsetNotInTargets_InvalidArgument` | Subset validation |
| D15 | `start_bot_handler_NonexistentTarget_NotFound` | Missing channel rejected |
| D16 | `stop_bot_handler_Idempotent_UnknownBotId` | Stop returns was_active=false |
| D17 | `integration_bot_single_target_audio_flows` | One target hears bot voice via gRPC roundtrip |
| D18 | `integration_bot_two_target_broadcast_both_hear` | Both targets hear bot voice |
| D19 | `integration_bot_whisper_subset_only_some_hear` | Whisper: only write_subset hears |
| D20 | `integration_bot_hangup_one_target_other_continues` | Hangup target_1 → target_2 still has bot |
| D21 | `integration_bot_passthrough_no_interrupt_bridge_audio` | Bot silent → bridge audio flows unchanged through WRITE_REPLACE |

Total ~21 acceptance scenarios.

## Verification gate (per commit)

```bash
cd /tmp/open-switch-w7d
make protos.lint && make protos
cmake -B build -DENABLE_ASAN=ON -DENABLE_TSAN=OFF -DBUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
make edge.lint
docker run --rm -v "$PWD":/work davidanson/markdownlint-cli2:v0.18.0 'openspec/changes/core-module-v1/**/*.md'
# Then TSAN run for fanout + bot session:
cmake -B build-tsan -DENABLE_ASAN=OFF -DENABLE_TSAN=ON -DBUILD_TESTS=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan -R "bug_fanout|bot_session" --output-on-failure
```

All MUST be green before push. ASAN lesson from W6 Track A: never
skip ASAN local run before pushing.

## Commit message

```text
feat(control,media): W7 Track D — StartBot multi-target bug attachment

Replace W6 legacy per-channel single-target streaming RPCs with the
multi-target StartBot RPC per tasks.md §"V1 scope confirmed".

  - osw::media::BugFanout: TtsPlayoutBuffer-shaped router that takes
    one inbound gRPC audio stream and fans-out to N per-target SPSC
    queues. Broadcast or whisper-subset mode.
  - osw::media::BotSession: one bot logical entity per StartBot call.
    Owns the upstream bidi gRPC stream, the per-target bug handles
    (READ_STREAM + WRITE_REPLACE per target), per-direction
    resamplers, and the fanout.
  - osw::control::ActiveBots: bot registry keyed by module-minted
    UUIDv7. Integrates with the CS_DESTROY channel state hook for
    automatic cleanup on hangup (closes W6.5 P1-003 design gap).
  - 4 purposes: TTS_BROADCAST, STT_LISTEN, VOICEBOT_DUPLEX, WHISPER.
    WHISPER uses write_target_channel_uuids subset to restrict where
    the bot's voice lands.
  - Passthrough when bot is silent (FF-036): WRITE_REPLACE callback
    returns SWITCH_TRUE without setting the replace frame; bridge
    audio / silence driver flows through unchanged.
  - 4 new audit subclasses: osw.media.bot.{started, stopped} Tier-1;
    osw.media.bot.{target_drop, target_attach_failed} Tier-1/2.
  - 4 new module-config fields (bot_max_targets, bot_target_queue_ms,
    bot_drain_timeout_ms, max_bots_per_channel).
  - W6 legacy RPCs (StartTts, StartStt, StartVoicebot) stay shippable
    for single-target IVR-style mode.

21 acceptance scenarios pass under ASAN+TSAN locally.
```

No `Co-Authored-By:` line. Author / committer is `@luongdev`.

## Out of scope (for Track D explicitly)

- Conference-based multi-party (N ≥ 4 participants) — V2.
- `UpdateBotTargets` RPC (dynamic add/remove) — V1.5.
- N ≥ 2 bots simultaneously per call — V2 (needs module-side mix
  arbiter).
- Recording (W7 Track B).
- Eavesdrop policy (W7 Track A).
- Silence driver (W6.6 hotfix is a prerequisite for the demo to work
  on a non-bridged channel; Track D itself doesn't add a driver).

## Sub-agent prompt template (orchestrator-internal)

```text
You are the W7 Track D sub-agent. Your authoritative scope is
/tmp/open-switch-w7d/openspec/changes/core-module-v1/implementation/W7-track-D-bot-multitarget.md.

HARD PREREQUISITES (verify before starting):

  - W6.5 fix-sprint must be merged on main. Confirm via
    `git log origin/main | grep -E "P1-001|P1-002|P1-003|P1-004|P1-005|P2-003"`
    finding the fix commits. If not present, STOP and report blocked.
  - W6.6 silence driver should be merged. Track D does not directly
    depend on it, but the W6.5 demo + integration tests on a
    non-bridged channel need it. You may proceed if missing, but
    flag in your PR description.

Implementation rules (HARDENED in project memory):

  - NO Co-Authored-By trailers in commits. Author/committer is @luongdev.
  - Run ASAN=ON locally before every push.
  - Do NOT force-push without explicit user authorization.
  - Do NOT commit build artifacts.
  - Reviews are the orchestrator's job (Codex/Gemini CLI).

When done, commit on branch implementation/wave7-track-d-bot-multitarget
with the exact commit message in §"Commit message" of the brief.
Push with `git push -u origin implementation/wave7-track-d-bot-multitarget`.
Open PR via `gh pr create`. Do NOT merge.
```
