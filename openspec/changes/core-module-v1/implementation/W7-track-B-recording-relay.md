# W7 Track B — RECORDING_RELAY purpose + stereo split + LATE-stage refinement

## Owner / model

Sonnet sub-agent. Design fully captured in
[`designs/recording-with-bot.md`](../designs/recording-with-bot.md) and
[`designs/media-bridge.md`](../designs/media-bridge.md). Sub-agent
implements; does NOT redesign.

## Scope

Implement module-owned recording for bot-participating calls:

1. **`RECORDING_RELAY` purpose attach** — two FS media bugs (one
   READ_STREAM, one WRITE_STREAM) glued to a single bidi gRPC stream.
   Both bugs are attached together by `StartRecordingRelay`; both
   detach together on `StopRecordingRelay` or channel hangup.
2. **MediaBugManager LATE-stage refinement** — `kRecordingRelay`
   attach is refused with `FAILED_PRECONDITION` unless ≥ 1 INJECT
   bug (`kTtsPlayback` or `kVoicebotDuplexWrite`) is already in the
   chain. This is the design's contract that bug-managed recording
   never observes pre-injection audio.
3. **`StereoFramePairer`** — per-channel timestamp pairer with 4-frame
   ring per side. Pairs read-tap (L=caller) and write-tap (R=post-
   injection) frames by FS timestamp; warns at >5 ms desync; fills
   the slow side with silence at >25 ms desync timeout. Interleaves
   the paired frame to `BOTH_INTERLEAVED` for the gRPC payload.
4. **`StartRecordingRelay` + `StopRecordingRelay` RPCs** — control
   handlers wiring the two bugs + the pairer + the gRPC stream.
5. **`warn_record_before_inject` hook in `StartTts`/`StartVoicebot`** —
   when one of the two RPCs attaches an INJECT bug, scan the chain for
   FS-native `record_session` bugs (via `switch_core_media_bug_count`
   filter by function name per FF-008). If found and the operator
   knob `warn_record_before_inject` is true, emit a Tier-1 audit so
   operators learn about the silent-bot mistake at attach time.
6. **Tier-1 audit subclasses**: `osw.recording.relay_started`,
   `osw.recording.relay_stopped`, `osw.recording.warn_record_before_inject`.
   **Tier-2 subclasses** (rate-limited 1/min):
   `osw.recording.send_overflow`, `osw.recording.lr_desync`.
7. **Metrics**: Prometheus counters / gauges per channel (frames sent
   L+R, desync count, queue overflow, current depth).
8. **Module wiring**: Load step instantiates a per-module
   `RecordingRelayManager`; Shutdown drains active streams before
   MediaBugManager teardown.

Files in scope:

```text
proto/open_switch/control/v1/control.proto             (PATCH — add 2 RPCs + 2 messages)
proto/open_switch/media/v1/media.proto                 (PATCH — add Channel.BOTH_INTERLEAVED enum value if missing; StreamStart.side already has STEREO per W6)

include/osw/media/recording_relay.h                    (new — public API)
include/osw/media/stereo_pairer.h                      (new — StereoFramePairer)
src/media/recording_relay.cc                           (new — bug callbacks + lifecycle)
src/media/stereo_pairer.cc                             (new — pairer impl)
src/media/CMakeLists.txt                               (PATCH — extend osw_media + osw_media_fs)

src/media/bug_manager.cc                               (PATCH — LATE-stage "require INJECT" gate; warn_record_before_inject hook on INJECT attach)
include/osw/media/bug_manager.h                        (PATCH — public RequireInjectBugPresent helper for the gate)

src/control/handlers/start_recording_relay_handler.cc  (new)
src/control/handlers/stop_recording_relay_handler.cc   (new)
src/control/CMakeLists.txt                             (PATCH — list new TUs)

src/control/handlers/start_tts_handler.cc              (PATCH — warn_record_before_inject scan after Attach success)
src/control/handlers/start_voicebot_handler.cc         (PATCH — same)

src/mod_open_switch.cc                                 (PATCH — Load + Shutdown wiring)

include/osw/config/module_config.h                     (PATCH — 5 new recording fields)
src/config/module_config.cc                            (PATCH — XML parse + Validate)

src/events/audit.cc + src/events/tier.cc               (PATCH — add 5 new subclasses to allowlist with correct tier)

openspec/changes/core-module-v1/specs/control-api/spec.md  (PATCH — document new RPCs + RPCs section + warn_record_before_inject knob)

tests/unit/media/stereo_pairer_test.cc                 (new)
tests/unit/media/recording_relay_test.cc               (new — FS-mock)
tests/unit/media/bug_manager_recording_gate_test.cc    (new — LATE-stage refinement)
tests/unit/control/start_recording_relay_handler_test.cc (new — FS-mock)
tests/integration/recording_mono_with_bot.cc           (new)
tests/integration/recording_stereo_split.cc            (new)
tests/integration/recording_rejects_before_inject.cc   (new)
tests/integration/recording_warn_before_inject_on_native.cc (new)
```

## Spec deltas

### `control.proto` — 2 new RPCs

```protobuf
service Control {
  // ... existing 12 RPCs ...

  // W7: Recording relay.  Attaches a READ_STREAM bug (taps caller mic) +
  // WRITE_STREAM bug (taps post-injection write side) and pipes the
  // paired audio to a gRPC recording sink.
  //
  // Refused with FAILED_PRECONDITION if no INJECT-stage bug
  // (TTS / voicebot-duplex-write) is currently attached to the channel.
  rpc StartRecordingRelay(StartRecordingRelayRequest)
      returns (StartRecordingRelayResponse);

  // Stops a previously started recording relay.  Idempotent.
  rpc StopRecordingRelay(StopRecordingRelayRequest)
      returns (StopRecordingRelayResponse);
}

message StartRecordingRelayRequest {
  open_switch.common.v1.RequestHeader header = 1;
  string channel_uuid = 2;

  // gRPC URL of the recording sink (e.g., grpc://recorder.tenant.svc:50071)
  string relay_endpoint = 3;

  // True = stereo split (L=caller, R=post-injection write); false = mono mixed.
  bool stereo = 4;

  // Default 8000.  16000 / 24000 / 48000 also allowed.
  uint32 sample_rate_hz = 5;

  // Per-call override of warn_record_before_inject (optional;
  // empty = use module default).  Reserved field; not consumed in V1.
  string warn_before_inject_override = 6;
}

message StartRecordingRelayResponse {
  string stream_id = 1;   // module-assigned, opaque
  uint32 negotiated_rate_hz = 2;
}

message StopRecordingRelayRequest {
  open_switch.common.v1.RequestHeader header = 1;
  string channel_uuid = 2;
  string stream_id = 3;   // optional; empty = stop all recordings on channel
}

message StopRecordingRelayResponse {
  uint32 streams_stopped = 1;
}
```

### `module_config.h` — 5 new fields

```cpp
struct ModuleConfig {
    // ... existing W6 jitter fields ...

    // === W7 recording ===
    // Per-stream send-ring capacity in ms of audio (paired L+R frames).
    // Larger = more tolerance to slow recording sink; more memory.
    std::uint32_t recording_send_ring_ms = 500;

    // Stereo desync warn threshold.  >5 ms gap between L and R timestamps
    // triggers a WARN log + counter increment.  No frame drop.
    std::uint32_t stereo_desync_warn_ms = 5;

    // Stereo desync silence-fill threshold.  >25 ms gap → fill the slow
    // side with silence and emit the frame; emits lr_desync counter +
    // Tier-2 event (rate-limited).
    std::uint32_t stereo_desync_timeout_ms = 25;

    // Default sample rate for recording sinks.  Per-call StartRecordingRelay
    // request may override.
    std::uint32_t recording_default_rate_hz = 8000;

    // When StartTts / StartVoicebot attaches and an FS-native
    // record_session bug is already on the chain in front of the new
    // INJECT bug, emit a Tier-1 audit warning.  Helps operators catch
    // the "compliance reflex" silent-bot ordering mistake at attach time.
    bool warn_record_before_inject = true;
};
```

`Validate()` clamps:

- `recording_send_ring_ms ∈ [50, 5000]`. Out of range → coerce + WARN.
- `stereo_desync_warn_ms ≤ stereo_desync_timeout_ms ≤ 100`.
- `recording_default_rate_hz ∈ {8000, 16000, 24000, 48000}`. Other →
  coerce to 8000 + WARN.

XML schema in `open_switch.conf.xml`:

```xml
<settings>
  <param name="recording_send_ring_ms" value="500"/>
  <param name="stereo_desync_warn_ms" value="5"/>
  <param name="stereo_desync_timeout_ms" value="25"/>
  <param name="recording_default_rate_hz" value="8000"/>
  <param name="warn_record_before_inject" value="true"/>
</settings>
```

### Audit subclasses

| Subclass | Tier | Trigger |
|---|---|---|
| `osw.recording.relay_started` | 1 | `StartRecordingRelay` succeeded; bugs attached + gRPC opened |
| `osw.recording.relay_stopped` | 1 | `StopRecordingRelay` succeeded OR channel hangup → DetachAll |
| `osw.recording.warn_record_before_inject` | 1 | `StartTts` / `StartVoicebot` detected FS-native `record_session` in front of new INJECT bug |
| `osw.recording.send_overflow` | 2 | Per-stream send-ring dropped frame (rate-limited 1/min per stream) |
| `osw.recording.lr_desync` | 2 | Stereo pairer hit desync timeout, filled silence (rate-limited 1/min per stream) |

Add to Tier-1 / Tier-2 allowlists in `src/events/tier.cc`.

## Implementation steps

### 1. `StereoFramePairer`

`include/osw/media/stereo_pairer.h`:

```cpp
namespace osw::media {

struct PairedFrame {
    std::vector<std::uint8_t> interleaved;  // [L0 R0 L1 R1 ...] int16 LE
    std::uint32_t samples_per_channel = 0;
    std::uint32_t sample_rate_hz = 0;
    std::uint64_t seq = 0;
};

class StereoFramePairer {
 public:
    struct Config {
        std::uint32_t sample_rate_hz = 8000;
        std::uint32_t desync_warn_ms = 5;
        std::uint32_t desync_timeout_ms = 25;
    };

    explicit StereoFramePairer(Config cfg);
    ~StereoFramePairer();

    StereoFramePairer(const StereoFramePairer&) = delete;
    StereoFramePairer& operator=(const StereoFramePairer&) = delete;

    /// Push a left-channel (caller) frame.  Returns a paired frame
    /// if one is ready (matching R timestamp within tolerance).
    /// Otherwise queues internally.  May also return a paired frame
    /// with silence-fill if the desync timeout has passed.
    [[nodiscard]] std::optional<PairedFrame>
        PushLeft(std::uint64_t fs_timestamp_samples,
                 std::span<const std::int16_t> samples) noexcept;

    /// Push a right-channel (post-injection write) frame.  Symmetric to PushLeft.
    [[nodiscard]] std::optional<PairedFrame>
        PushRight(std::uint64_t fs_timestamp_samples,
                  std::span<const std::int16_t> samples) noexcept;

    /// Tick called from a periodic timer (e.g., 10 ms) to flush
    /// any pending side that has waited > desync_timeout_ms.
    [[nodiscard]] std::optional<PairedFrame> Tick() noexcept;

    /// Stats.
    [[nodiscard]] std::uint64_t DesyncCount() const noexcept;
    [[nodiscard]] std::uint64_t PairedCount() const noexcept;

 private:
    struct PendingFrame {
        std::uint64_t fs_timestamp_samples = 0;
        std::chrono::steady_clock::time_point arrived_at;
        std::vector<std::int16_t> samples;
    };

    Config cfg_;
    std::array<PendingFrame, 4> left_ring_;
    std::array<PendingFrame, 4> right_ring_;
    std::size_t left_head_ = 0, left_tail_ = 0;
    std::size_t right_head_ = 0, right_tail_ = 0;
    std::uint64_t seq_ = 0;
    std::uint64_t paired_count_ = 0;
    std::uint64_t desync_count_ = 0;
    std::mutex mu_;
};

}  // namespace osw::media
```

**Pairing algorithm** (in `PushLeft` / `PushRight`):

1. Under `mu_`: enqueue the new pending frame onto its ring (drop
   oldest if full).
2. Try to pair: pop the head of left ring + head of right ring; if
   their `fs_timestamp_samples` differ by ≤
   `desync_warn_ms × sample_rate_hz / 1000`: interleave their samples,
   return a `PairedFrame`. If they differ by > that threshold but
   each side has remaining frames, peek to see if a better match
   exists in the next slot; otherwise pop both and pair anyway
   (small offset is acceptable).
3. If timestamps differ by > `desync_timeout_ms × sample_rate_hz / 1000`:
   pop only the older side, pair it with silence on the other side,
   increment `desync_count_`, return.

**`Tick()`** is the safety net for the case where one side has gone
fully silent (e.g., bot stopped streaming): if the head of either
ring has been waiting > `desync_timeout_ms`, flush it with the other
side as silence.

The mutex is fine because the producer threads (read-tap + write-tap
callbacks) are NOT the same FS thread — read tap fires on the read
side of the media thread, write tap on the write side of the SAME
media thread (per `recording-with-bot.md` §"Sync of L and R": both
callbacks fire on the same FS media thread, single-threaded per
channel). So `mu_` is uncontended in practice; we keep it for
correctness against the `Tick()` thread, which IS different
(periodic timer running in a module-owned thread).

**Interleave format**: per `media-bridge.md`, `BOTH_INTERLEAVED`
stereo at 8 kHz / 20 ms has `duration_samples = 160` and
`payload.size() = 160 * 4` bytes (160 L+R pairs × 4 bytes per pair =
2 bytes per sample × 2 channels).

**Tests** (`stereo_pairer_test.cc`):

- `PairSimultaneousLR_Interleaves`: push L and R at the same
  timestamp → paired frame with `[L0 R0 L1 R1 ...]` interleaving,
  desync_count=0.
- `Pair5msDesync_Warns`: push L at t=0, R at t=5ms → paired with
  small offset, desync_count=0 (5 ms = warn threshold, no drop), but
  WARN logged.
- `PairTimeoutFlush_SilenceFill`: push L only at t=0; Tick at t+30ms →
  paired with R=silence, desync_count=1.
- `RingDropOldest_FrameLoss`: push 5 L frames without any R →
  oldest L dropped, ring has 4.
- `MutexSafeUnderConcurrentPush`: spawn 2 threads, one PushLeft +
  one PushRight, 1000 iterations each → no data races (TSAN clean),
  paired_count matches expectation.
- `SilencePayloadFormat`: paired frame with silence-fill on R has
  exact zero bytes for R samples; L samples preserved.

### 2. `RecordingRelay` (per-channel object)

`include/osw/media/recording_relay.h`:

```cpp
namespace osw::media {

struct RecordingRelayConfig {
    std::string channel_uuid;
    std::string relay_endpoint;
    bool stereo = false;
    std::uint32_t sample_rate_hz = 8000;
    std::uint32_t send_ring_ms = 500;
    std::uint32_t desync_warn_ms = 5;
    std::uint32_t desync_timeout_ms = 25;
    std::string tenant_id;
    std::string stream_id;
};

/// One per active recording stream.  Holds:
///   - StreamClient (W6 Track B) to the relay endpoint
///   - StereoFramePairer (if stereo)
///   - Send-ring of frames being drained by StreamClient::Send
class RecordingRelay {
 public:
    explicit RecordingRelay(RecordingRelayConfig cfg,
                            std::shared_ptr<grpc::Channel> chan);
    ~RecordingRelay();

    RecordingRelay(const RecordingRelay&) = delete;
    RecordingRelay& operator=(const RecordingRelay&) = delete;

    /// Push read-side caller frame.  Stereo: feeds pairer L.  Mono:
    /// adds to mono mixer (queue, paired-with-nothing).
    void PushReadFrame(std::uint64_t fs_ts, std::span<const std::int16_t> samples);

    /// Push write-side post-injection frame.  Stereo: feeds pairer R.
    /// Mono: no-op (mono recording only captures read side; bot audio
    /// is in the recording iff the bug chain is correctly ordered).
    void PushWriteFrame(std::uint64_t fs_ts, std::span<const std::int16_t> samples);

    /// Periodic tick: flushes stale pairer state + drains any
    /// timed-out pending frames.
    void Tick();

    /// Half-closes the upstream stream + cancels in-flight sends.
    /// Idempotent.
    void Stop();

    bool IsStopped() const noexcept;
    std::uint64_t FramesSent() const noexcept;
    std::uint64_t SendOverflowCount() const noexcept;
    std::uint64_t DesyncCount() const noexcept;

 private:
    RecordingRelayConfig cfg_;
    std::unique_ptr<osw::media::StreamClient> client_;     // from W6 Track B
    std::optional<StereoFramePairer> pairer_;              // stereo only
    std::atomic<std::uint64_t> frames_sent_{0};
    std::atomic<std::uint64_t> send_overflow_{0};
    std::atomic<bool> stopped_{false};
};

}  // namespace osw::media
```

`src/media/recording_relay.cc` implements the methods. For the gRPC
stream, reuse the existing `osw::media::StreamClient` infrastructure
from W6 Track B (bidi reader-writer, channel pool).

**Mono path**: `PushReadFrame` pushes directly to `StreamClient::Send`
with `Channel::LEFT` (or `MONO` if the proto has it). `PushWriteFrame`
is a no-op when `stereo=false`; the recording captures only the read
side in mono mode (per `recording-with-bot.md` — mono mixed
recording with bot audio in the file requires the FS chain to be
correctly ordered, which is the operator dialplan's job, not the
module's).

Wait — re-read `recording-with-bot.md`:

> For module-owned recording (`StartRecordingRelay` → our
> `RECORDING_RELAY` purpose, stage rank `LATE`), the MediaBugManager
> refuses to attach the relay bug if an INJECT-stage bug
> ... is not already in the chain or would be added later.

So for **module-owned mono recording**, we DO want bot audio in the
file. The way to get bot audio is the `WRITE_STREAM` bug observes
the post-injection frame. So mono `RecordingRelay` should ALSO push
the write-tap frame (which is post-injection per our LATE-stage
gate). Mono mixed = caller + bot mixed downstream by the recording
service; the module sends both read-tap and write-tap frames
sequentially with timestamps, and the sink mixes / serialises.

Actually simpler: in **mono mode**, only ONE bug attaches —
`SMBF_WRITE_STREAM`. That bug observes `write_frame` AFTER all
WRITE_REPLACE bugs (by our LATE-rank gate), which contains caller-
side audio mixed with bot audio in the FS write pipeline. So mono
captures the post-injection write-side frame, which IS the mixed
audio that the caller hears.

Updated mono path:

- Mono mode: attach only one `SMBF_WRITE_STREAM` bug. Read-tap is
  not used. The recording captures what the caller hears (post-
  injection mix).
- Stereo mode: attach TWO bugs (read + write). Pair them.

This matches the design more cleanly. Sub-agent codes accordingly.

**Tests** (`recording_relay_test.cc`):

- `StereoConstruction_OpensClientAndPairer`: construct with stereo=true
  → `StreamClient` opened to endpoint; `StereoFramePairer` instantiated.
- `MonoConstruction_OpensClientNoPairer`: stereo=false → no pairer;
  PushWriteFrame is the only producer.
- `PushReadFrame_StereoFeedsPairer`: push L + R → paired frame sent
  via `client_->Send`.
- `PushReadFrame_Mono_NoOp`: stereo=false; PushReadFrame does NOT
  send anything (mono mode uses write-tap only).
- `PushWriteFrame_Mono_Sends`: stereo=false; PushWriteFrame sends
  frame via `client_->Send` with `Channel::MONO` (or as design
  dictates).
- `Stop_HalfClosesStream`: call Stop() → `client_->HalfClose()`
  called; subsequent pushes no-op.
- `SendRingOverflow_DropsOldestAndCounts`: saturate sender; assert
  oldest dropped + `send_overflow_` increments.

### 3. `MediaBugManager` LATE-stage refinement

Extend `MediaBugManager::Attach`:

```cpp
// In Attach(), after the existing this_rank vs max_existing_rank check:

if (cfg.purpose == Purpose::kRecordingRelay) {
    if (!HasAnyInjectBug(reg)) {
        return AttachResult::Err(
            grpc::StatusCode::FAILED_PRECONDITION,
            "RECORDING_RELAY attach refused: no INJECT-stage bug present "
            "(TTS / voicebot-duplex-write must be attached first)");
    }
}
```

`HasAnyInjectBug(reg)` walks the per-channel bug list and returns
true iff any entry has `StageRank(purpose) == 3` (INJECT).

Add public helper for tests:

```cpp
[[nodiscard]] bool MediaBugManager::HasInjectBug(
    const std::string& channel_uuid) const noexcept;
```

**Tests** (`bug_manager_recording_gate_test.cc`):

- `AttachRecordingWithoutInject_Rejected`: attach kRecordingRelay
  on empty channel → FAILED_PRECONDITION.
- `AttachRecordingAfterTts_Allowed`: attach kTtsPlayback then
  kRecordingRelay → both succeed.
- `AttachRecordingAfterVoicebotWrite_Allowed`: attach
  kVoicebotDuplexWrite then kRecordingRelay → both succeed.
- `AttachRecordingAfterStt_Rejected`: STT is MID_READ (rank 2), not
  INJECT (rank 3). Attach STT then kRecordingRelay → kRecordingRelay
  refused.
- `DetachInjectThenAttachRecording_Rejected`: attach TTS, detach TTS,
  then attach kRecordingRelay → rejected (no INJECT currently
  present; the gate checks at attach time, not historically).

### 4. `warn_record_before_inject` hook in `StartTts` / `StartVoicebot`

After `MediaBugManager::Attach` returns success in both handlers,
scan the FS-side chain for `record_session` bugs:

```cpp
// After Attach success:
if (module_config_->warn_record_before_inject) {
    osw::raii::SessionLock target(req.channel_uuid());
    if (target) {
        // FF-008: switch_core_media_bug_count filters by `function`.
        const std::uint32_t native_record_count =
            switch_core_media_bug_count(target.session(), "record_session");
        if (native_record_count > 0) {
            osw::audit::Builder b("osw.recording.warn_record_before_inject",
                                  osw::events::Tier::k1Durable);
            b.Header("Unique-ID", req.channel_uuid());
            b.Header("tenant_id", req.header().tenant_id());
            b.Header("inject_purpose", /*StartTts:*/"tts" /*StartVoicebot:*/"voicebot");
            b.Header("native_record_count", std::to_string(native_record_count));
            b.Header("remediation",
                     "Reorder dialplan: call start_bot (Tts/Voicebot) BEFORE "
                     "record_session, or use StartRecordingRelay instead.");
            osw::audit::Emit(std::move(b));
        }
    }
}
```

The check is opt-out (default true). Operators with a known-good
pipeline can disable to suppress.

**Tests** (`start_recording_relay_handler_test.cc::WarnFiresOnPreInject`):

- FS-mock channel with one fake bug attached at function name
  "record_session". Call StartTts → assert audit Tier-1 emitted with
  subclass `osw.recording.warn_record_before_inject`.

- FS-mock channel with no record_session bug. Call StartTts → assert
  NO audit emit for this subclass.

- Module config `warn_record_before_inject=false` + record_session
  bug present + StartTts → assert NO audit emit.

### 5. `StartRecordingRelay` handler

```cpp
// src/control/handlers/start_recording_relay_handler.cc
::grpc::Status StartRecordingRelayHandler::Handle(
    ::grpc::ServerContext* ctx,
    const StartRecordingRelayRequest* req,
    StartRecordingRelayResponse* resp) {
    // 1. Validate args.
    if (req->channel_uuid().empty()) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                              "channel_uuid required");
    }
    if (req->relay_endpoint().empty()) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                              "relay_endpoint required");
    }

    // 2. Idempotency cache check (W5 Track B).
    // ... existing pattern ...

    // 3. SessionLock + attach 1 or 2 bugs via MediaBugManager.
    osw::raii::SessionLock target(req->channel_uuid());
    if (!target) {
        return ::grpc::Status(::grpc::StatusCode::NOT_FOUND,
                              "channel not found");
    }
    auto& mgr = MediaBugManagerSingleton();

    const std::uint32_t rate =
        req->sample_rate_hz() ? req->sample_rate_hz()
                              : module_config_->recording_default_rate_hz;

    // Stereo: read + write bug.  Mono: write bug only.
    std::vector<osw::media::BugHandle> handles;
    if (req->stereo()) {
        auto read_result = mgr.Attach(target.session(), {
            .purpose = osw::media::Purpose::kRecordingRelay,
            .flags = SMBF_READ_STREAM,
            .target_rate_hz = rate,
            .tenant_id = req->header().tenant_id(),
            .upstream_endpoint = req->relay_endpoint(),
        });
        if (!read_result.ok()) return ToGrpcStatus(read_result.status);
        handles.push_back(std::move(read_result.handle));

        auto write_result = mgr.Attach(target.session(), {
            .purpose = osw::media::Purpose::kRecordingRelay,
            .flags = SMBF_WRITE_STREAM,
            .target_rate_hz = rate,
            .tenant_id = req->header().tenant_id(),
            .upstream_endpoint = req->relay_endpoint(),
        });
        if (!write_result.ok()) {
            // First bug is auto-detached when its BugHandle goes out of scope.
            return ToGrpcStatus(write_result.status);
        }
        handles.push_back(std::move(write_result.handle));
    } else {
        auto write_result = mgr.Attach(target.session(), {
            .purpose = osw::media::Purpose::kRecordingRelay,
            .flags = SMBF_WRITE_STREAM,
            .target_rate_hz = rate,
            .tenant_id = req->header().tenant_id(),
            .upstream_endpoint = req->relay_endpoint(),
        });
        if (!write_result.ok()) return ToGrpcStatus(write_result.status);
        handles.push_back(std::move(write_result.handle));
    }

    // 4. Construct RecordingRelay + register with ActiveMediaStreams.
    const std::string stream_id = osw::events::GenerateUuidV7();
    osw::media::RecordingRelayConfig cfg = {
        .channel_uuid = req->channel_uuid(),
        .relay_endpoint = req->relay_endpoint(),
        .stereo = req->stereo(),
        .sample_rate_hz = rate,
        .send_ring_ms = module_config_->recording_send_ring_ms,
        .desync_warn_ms = module_config_->stereo_desync_warn_ms,
        .desync_timeout_ms = module_config_->stereo_desync_timeout_ms,
        .tenant_id = req->header().tenant_id(),
        .stream_id = stream_id,
    };
    auto chan = grpc_channel_pool_->GetOrCreate(req->relay_endpoint());
    auto relay = std::make_unique<osw::media::RecordingRelay>(std::move(cfg),
                                                              std::move(chan));

    // 5. Wire bug callbacks to relay (file-static trampoline pattern from W6 Track C).
    for (auto& h : handles) {
        h.SetUserData(relay.get());  // pointer is owned by ActiveMediaStreams
    }

    active_streams_->Register(req->channel_uuid(),
                              osw::media::Purpose::kRecordingRelay,
                              stream_id,
                              std::move(handles),
                              std::move(relay));

    // 6. Emit relay_started audit.
    osw::audit::Builder b("osw.recording.relay_started", osw::events::Tier::k1Durable);
    b.Header("Unique-ID", req->channel_uuid());
    b.Header("tenant_id", req->header().tenant_id());
    b.Header("stream_id", stream_id);
    b.Header("stereo", req->stereo() ? "true" : "false");
    b.Header("rate_hz", std::to_string(rate));
    osw::audit::Emit(std::move(b));

    resp->set_stream_id(stream_id);
    resp->set_negotiated_rate_hz(rate);
    return ::grpc::Status::OK;
}
```

### 6. `StopRecordingRelay` handler

Mirror of W6's `StopMediaStream` but scoped to recording streams.
`active_streams_->StopByPurpose(channel_uuid, Purpose::kRecordingRelay)`.
Emit `osw.recording.relay_stopped` Tier-1.

### 7. Bug callbacks (file-static, like W6 Track C)

`src/media/recording_relay.cc`:

```cpp
static switch_bool_t OswRecordingReadTap(switch_media_bug_t* bug,
                                         void* user_data,
                                         switch_abc_type_t type) {
    auto* relay = static_cast<osw::media::RecordingRelay*>(user_data);
    if (!relay || relay->IsStopped()) return SWITCH_TRUE;
    try {
        if (type == SWITCH_ABC_TYPE_READ) {
            switch_frame_t* frame = nullptr;
            if (switch_core_media_bug_read(bug, &frame, SWITCH_FALSE)
                    == SWITCH_STATUS_SUCCESS && frame) {
                relay->PushReadFrame(
                    /*fs_ts=*/frame->timestamp,
                    std::span<const std::int16_t>(
                        reinterpret_cast<const std::int16_t*>(frame->data),
                        frame->datalen / 2));
            }
        } else if (type == SWITCH_ABC_TYPE_CLOSE) {
            relay->Stop();
        }
        return SWITCH_TRUE;
    } catch (...) {
        return SWITCH_FALSE;
    }
}

static switch_bool_t OswRecordingWriteTap(switch_media_bug_t* bug,
                                          void* user_data,
                                          switch_abc_type_t type) {
    auto* relay = static_cast<osw::media::RecordingRelay*>(user_data);
    if (!relay || relay->IsStopped()) return SWITCH_TRUE;
    try {
        if (type == SWITCH_ABC_TYPE_WRITE) {
            switch_frame_t* frame = nullptr;
            if (switch_core_media_bug_read(bug, &frame, SWITCH_FALSE)
                    == SWITCH_STATUS_SUCCESS && frame) {
                relay->PushWriteFrame(frame->timestamp,
                    std::span<const std::int16_t>(
                        reinterpret_cast<const std::int16_t*>(frame->data),
                        frame->datalen / 2));
            }
        } else if (type == SWITCH_ABC_TYPE_CLOSE) {
            relay->Stop();
        }
        return SWITCH_TRUE;
    } catch (...) {
        return SWITCH_FALSE;
    }
}
```

The `RecordingRelay` object lifetime is bound to `ActiveMediaStreams`
registry entry. The bug `user_data` pointer is the `RecordingRelay*`;
detach happens when the registry entry is destroyed (channel hangup,
StopRecordingRelay, module unload). The trampoline `CLOSE` calls
`Stop()` to half-close the gRPC stream.

**`MediaBugManager.Attach` already wraps these via `OswMediaBugTrampoline`
(W6 Track A pattern)** — the sub-agent registers the static callbacks
through the existing pattern; no need to introduce a new trampoline.

### 8. Module wiring

`src/mod_open_switch.cc` Load step (insertion):

```cpp
// W7 Track B: recording-relay registry + per-module timer thread for
// StereoFramePairer Tick().
g_recording_streams_ = std::make_unique<osw::media::ActiveRecordingStreams>(
    /*tick_interval_ms=*/10);

// Wire start_recording_relay_handler + stop_recording_relay_handler
// into the gRPC service (existing handler-registration pattern).
RegisterStartRecordingRelayHandler(server_builder, *g_recording_streams_);
RegisterStopRecordingRelayHandler(server_builder, *g_recording_streams_);
```

Shutdown step:

```cpp
// W7 Track B: drain active recording relays before MediaBugManager teardown.
g_recording_streams_->DrainAll();
g_recording_streams_.reset();
```

If the W6 `ActiveMediaStreams` registry can host recording entries
alongside TTS/STT/voicebot, the sub-agent reuses it; otherwise it
adds a parallel `ActiveRecordingStreams` registry. Style preference:
reuse W6's registry to keep one source of truth (less code, less
risk of split-brain on hangup).

## Acceptance tests

| ID | Name | What it proves |
|---|---|---|
| R1 | `stereo_pairer_PairSimultaneousLR_Interleaves` | Basic pairing at t=t |
| R2 | `stereo_pairer_5msDesync_Warns_NoDrop` | Warn threshold |
| R3 | `stereo_pairer_TimeoutFlush_SilenceFill` | Timeout = silence fill + counter |
| R4 | `stereo_pairer_RingDropOldest_FrameLoss` | Bounded ring |
| R5 | `stereo_pairer_MutexSafeUnderConcurrentPush` | TSAN clean |
| R6 | `stereo_pairer_SilencePayloadFormat` | Silence bytes are zero, L preserved |
| R7 | `recording_relay_StereoConstructor_OpensClientAndPairer` | Stereo wires correctly |
| R8 | `recording_relay_MonoConstructor_NoPairer` | Mono lighter path |
| R9 | `recording_relay_Stop_HalfClosesStream` | Idempotent stop |
| R10 | `recording_relay_SendRingOverflow_DropsOldest` | Send-side backpressure |
| R11 | `bug_manager_AttachRecordingWithoutInject_Rejected` | LATE-stage gate |
| R12 | `bug_manager_AttachRecordingAfterTts_Allowed` | Happy path |
| R13 | `bug_manager_AttachRecordingAfterStt_Rejected` | STT is not INJECT |
| R14 | `bug_manager_DetachInjectThenAttachRecording_Rejected` | Gate checks live state |
| R15 | `start_recording_relay_handler_StereoHappyPath` | RPC end-to-end stereo |
| R16 | `start_recording_relay_handler_MonoHappyPath` | RPC end-to-end mono |
| R17 | `start_recording_relay_handler_NoInject_Returns_FailedPrecondition` | Surfaces LATE-stage gate as gRPC error |
| R18 | `start_recording_relay_handler_Idempotent` | Same request_id returns cached response |
| R19 | `start_tts_handler_WarnRecordBeforeInject_FiresOnNativeRecord` | warn_record_before_inject Tier-1 audit on native record_session pre-existing |
| R20 | `start_tts_handler_WarnRecordBeforeInject_SilentWhenDisabled` | Knob = false → no audit |
| R21 | `stop_recording_relay_handler_StopsAllStreams_EmitsRelayStopped` | StopRecordingRelay drains + Tier-1 audit |
| R22 | `recording_relay_HangupTriggersStopAndAudit` | Channel hangup auto-stops + emits `relay_stopped` |
| R23 | `recording_relay_TierClassification` | All 5 new subclasses route to correct tier |
| R24 | `integration_record_with_bot_mono_chain_correct` | TTS first then StartRecordingRelay (mono) → relay sees post-injection frames |
| R25 | `integration_record_with_bot_stereo_chain_correct` | TTS first then StartRecordingRelay (stereo) → relay receives L=caller, R=post-injection |
| R26 | `integration_record_warn_before_inject_on_native_record` | record_session attached first, then StartTts → Tier-1 warn audit |

Total ~26 acceptance scenarios. Each lands in the file path listed in
"Files in scope".

## Verification gate (per commit)

```bash
cd /tmp/open-switch-w7b
make protos.lint
make protos
cmake -B build -DENABLE_ASAN=ON -DENABLE_TSAN=OFF -DBUILD_TESTS=ON   # ASAN local lesson from W6A
cmake --build build -j
ctest --test-dir build --output-on-failure
make edge.lint
docker run --rm -v "$PWD":/work davidanson/markdownlint-cli2:v0.18.0 'openspec/changes/core-module-v1/**/*.md'
```

All MUST be green before push. Run TSAN race check on the pairer
test suite specifically as a final pass:

```bash
cmake -B build-tsan -DENABLE_ASAN=OFF -DENABLE_TSAN=ON -DBUILD_TESTS=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan -R stereo_pairer --output-on-failure
```

## Commit message

```text
feat(control,media): W7 Track B — RECORDING_RELAY purpose + stereo split

Add module-owned recording for bot-participating calls per
designs/recording-with-bot.md + designs/media-bridge.md:

  - osw::media::RecordingRelay manages per-stream lifecycle: one or
    two FS media bugs (READ_STREAM + WRITE_STREAM), a bidi gRPC
    stream via the W6 StreamClient, and (stereo only) a per-channel
    StereoFramePairer.

  - osw::media::StereoFramePairer pairs L (caller mic) + R (post-
    injection write) frames by FS timestamp with 4-frame ring per
    side, 5ms warn / 25ms silence-fill timeout. Interleaves to the
    BOTH_INTERLEAVED PCM format for the gRPC payload.

  - MediaBugManager::Attach refuses kRecordingRelay if no INJECT
    bug (kTtsPlayback / kVoicebotDuplexWrite) is currently present
    on the channel (FAILED_PRECONDITION). Documents the contract
    that module-owned recording never observes pre-injection audio
    on the write side.

  - StartRecordingRelay / StopRecordingRelay RPCs land in the
    control service.

  - StartTts / StartVoicebot scan for FS-native record_session
    bugs at attach time (via switch_core_media_bug_count per
    FF-008) and emit Tier-1 osw.recording.warn_record_before_inject
    audit when one is found in front of the new INJECT bug. Helps
    operators catch the compliance-reflex ordering mistake before
    they discover silent-bot recordings.

  - 5 new audit subclasses: osw.recording.{relay_started,
    relay_stopped, warn_record_before_inject} Tier-1;
    osw.recording.{send_overflow, lr_desync} Tier-2 (rate-limited
    1/min per stream).

  - 5 new module-config fields (recording_send_ring_ms,
    stereo_desync_warn_ms, stereo_desync_timeout_ms,
    recording_default_rate_hz, warn_record_before_inject).

26 acceptance scenarios pass under ASAN+TSAN locally.
```

No `Co-Authored-By:` line. Author / committer is `@luongdev`.

## Out of scope (for Track B explicitly)

- Eavesdrop policy (Track A).
- Conference recording (V2).
- Bot-as-SIP-leg recording (operator-side dialplan, not module code).
- AMD detect handler (V2).
- File-based recording sink (operator-side; `tests/integration/osw_record_sink`
  in W8 if needed).

## Sub-agent prompt template (orchestrator-internal)

```text
You are the W7 Track B sub-agent. Your authoritative scope is
/tmp/open-switch-w7b/openspec/changes/core-module-v1/implementation/W7-track-B-recording-relay.md.

Implementation rules (HARDENED in project memory):

  - NO Co-Authored-By trailers in commits. Author/committer is @luongdev.
  - Run ASAN=ON locally before every push (lesson from W6 Track A).
  - Do NOT force-push without explicit user authorization.
  - Do NOT commit build artifacts.
  - Reviews are the orchestrator's job (Codex/Gemini CLI).

When done, commit on branch implementation/wave7-track-b-recording with
the exact commit message in §"Commit message" of the brief. Push with
`git push -u origin implementation/wave7-track-b-recording`. Open PR
via `gh pr create` or notify the orchestrator. Do NOT merge.
```
