# W6 — Media plane V1 (TTS + STT + voicebot duplex)

**Owner**: Opus 4.7 (orchestrator). Implementation: Sonnet sub-agents per
track (mechanical FS API + protobuf work; audio quality verification
deferred to W7 end-to-end smoke).
**Branch convention**: `implementation/wave6-{track}` off `main`.
Worktrees under `/tmp/open-switch-w6-{track}` (or `.claude/worktrees/`).
**Status**: Plan.

## Why

V1 ship gate (per `proposal.md` §"Exit criteria"): 50 CCU soak with
"mix of Originate + bot duplex + STT + recording" — both **bot duplex**
and **STT** are media-plane features. Today the module has the proto
schema (`proto/open_switch/media/v1/media.proto`) and the design docs
(`designs/media-bridge.md`, `designs/call-transcribe.md`, `designs/
recording-with-bot.md`) but **zero C++ implementation**: no
`MediaBugManager`, no bidi gRPC stream client, no frame router, no
resampler wrapper, no handler RPCs.

W6 ships the **mechanical media plumbing**: media-bug lifecycle, bidi
gRPC stream client, L16 PCM framing, `switch_resample_t` wrapper, and
the three Purpose handlers needed for an end-to-end TTS/STT/duplex
demo. W7 adds recording + eavesdrop. W8 closes V1 with soak +
Valgrind nightly.

## Scope decisions (locked by operator)

| Decision | Value | Rationale |
|---|---|---|
| Codecs in V1 | `PCM_S16LE` + `G711_ULAW` + `G711_ALAW` only | Internal LAN; bandwidth not constrained; G.711 is the FS-native frame format on telephony legs; L16 is what the media bug callback always delivers. |
| Opus codec | **Defer** (proto enum value kept as reserved; encoder/decoder not implemented) | YAGNI for private network. Re-evaluate when WebRTC peers or public-internet upstream appear. |
| Resampler matrix | **8 ↔ 16 kHz only** (drop 24 + 48 kHz paths) | Reuses `switch_resample_t` (FS-built-in spandsp wrapper) — no new dep. 24+ kHz TTS engines downsample on the engine side before sending into the module. |
| Resampler implementation | Thin wrapper around `switch_resample_t` | FS already links spandsp; zero new dep. New FF entry documenting the API contract. |
| AudioFrame metadata | `seq` (uint64) + `timestamp_samples` (uint64) + `duration_samples` (uint32) + `payload` (bytes) + `channel` (enum) | Already in `media.proto`; no changes needed. |
| Recording bug | **Defer to W7** | Bug-priority ordering across bot + recording needs the canonical chain established by W6's MediaBugManager first. |
| AMD detect | **Skip V1** (or defer to W7) | Not on the ship-gate exit-criteria list. The proto reserves the slot; handler can land in V2. |
| Eavesdrop policy (MOD.SEC.001) | **Defer to W7** | Gates supervisor eavesdrop on bot calls; meaningless without the media plane existing. |
| TTS playout jitter buffer | **In W6 Track C** — `TtsPlayoutBuffer` class between `StreamClient::on_audio` and FS `WRITE_REPLACE`. Default target 1000 ms, preroll 500 ms, high-water 1500 ms, hard cap 5000 ms. Per-call override via `StartTtsRequest.buffer_override`. Underrun policy: `silence` (default) \| `repeat_last`. | Without it, any TTS-engine jitter or network burst causes audible gaps in the bot's voice. See W6-track-C-handlers.md §"TtsPlayoutBuffer" for full design. |

## Out of scope (deferred to W7 / V2)

- `RECORDING_RELAY` purpose + bug-priority arch (W7)
- Stereo split for recording (W7)
- `AMD_DETECT` purpose (W7 or V2)
- `VAD_BARGE_IN` purpose (V2 — needs WebRTC barge-in semantics first)
- Eavesdrop policy (W7)
- Opus codec encoder/decoder (V2)
- 24 + 48 kHz resampling (V2)
- Direct in-module sinks (Kafka/Redis/NATS for transcripts) — subscriber
  side per `transport-adr.md`

## Phase 1 — Foundation (parallel)

Two tracks fork from `main` and run concurrently. Both touch
non-overlapping files; orchestrator merges sequentially without
conflict.

### Track A — MediaBugManager + lifecycle

**Files added**:
- `include/osw/media/bug_manager.h`
- `include/osw/media/bug_handle.h` (RAII lease)
- `src/media/bug_manager.cc`
- `src/media/bug_handle.cc`
- `tests/unit/media/bug_manager_test.cc`

**Responsibilities**:
1. Per-channel registry (`unordered_map<channel_uuid, std::vector<BugRecord>>`)
   keyed by `Purpose`. Duplicate purpose on same channel → `ALREADY_EXISTS`.
2. Stage-rank enforcement per `designs/media-bridge.md` §"Stage rank":
   - `EARLY`: `VAD_BARGE_IN` (always `SMBF_FIRST`)
   - `MID_READ`: `STT_TRANSCRIBE`, `AMD_DETECT`, `VOICEBOT_DUPLEX(read)`
   - `INJECT`: `TTS_PLAYBACK`, `VOICEBOT_DUPLEX(write)`
   - `LATE`: `RECORDING_RELAY` (W7), `TEST`
   - `Attach` rejects out-of-order attaches with `FAILED_PRECONDITION`
     (unless `SMBF_FIRST` is set explicitly).
3. RAII `BugHandle` — Detach is idempotent; destruction triggers
   `switch_core_media_bug_remove` with the `function`-name filter (FF-008)
   gated by `thread_id == switch_thread_self()` for `SMBF_THREAD_LOCK`
   bugs (FF-002).
4. `DetachAll(channel_uuid)` invoked from the channel-destroy state
   handler (CS_DESTROY); idempotent.
5. Single `osw::media::BugManager` instance owned by `Module`. Injected
   into handler classes via setter (W3 pattern).

**New FF entries needed**:
- FF-031 — `switch_core_media_bug_add` signature + ownership of
  `user_data` + callback signature.
- FF-032 — Channel state handler registration + CS_DESTROY ordering.

**Acceptance**:
- Attach VAD then STT then TTS in order — all succeed; chain is
  `[VAD@head] → STT → TTS`.
- Attach STT then VAD without `SMBF_FIRST` — VAD attach succeeds (gets
  `SMBF_FIRST` from manager) and prepends to chain head.
- Attach STT then TTS then a second STT — rejected with `ALREADY_EXISTS`.
- Attach TTS then STT (out of order: INJECT before MID_READ) — rejected
  with `FAILED_PRECONDITION`.
- BugHandle destructor on dropped handle removes the bug; counted via
  Health.
- `DetachAll` is idempotent and removes all bugs for a UUID.

**Tests**: FS-mock seam (`OSW_TEST_FS_MOCK=1`) per existing W3 pattern;
mock counts `switch_core_media_bug_add` + `_remove` calls.

### Track B — Bidi gRPC stream client + L16 framing + resampler

**Files added**:
- `include/osw/media/stream_client.h`
- `include/osw/media/audio_frame.h` (POD: pcm + rate + ts + seq)
- `include/osw/media/resampler.h`
- `src/media/stream_client.cc`
- `src/media/audio_frame.cc`
- `src/media/resampler.cc`
- `tests/unit/media/stream_client_test.cc`
- `tests/unit/media/resampler_test.cc`

**Responsibilities**:

1. **`StreamClient`** — wraps `MediaBridge::Stream` gRPC bidi stream:
   - Constructor: takes `channel_uuid`, `Purpose`, `endpoint`,
     `sample_rate_hz`, optional `start_message`, `tenant_id`.
   - `Open()` — sends `StreamStart`; awaits `StreamReady`; spawns reader
     thread for `FromService` messages.
   - `SendAudio(AudioFrame)` — enqueues into a bounded MPSC ring
     (capacity 256 frames ≈ 5s @ 20ms ptime). Full ring → drop oldest
     + emit `osw.media.frame_dropped` Tier-2 event.
   - `OnAudio(callback)` — invoked from reader thread for each
     `FromService::AudioFrame` (TTS playback path).
   - `OnTranscript(callback)` — for STT (Transcript messages).
   - `OnControl(callback)` — for Cancel / DTMF / BargeIn / Heartbeat.
   - `Close()` — half-closes send stream, joins reader thread.
   - Cancellation: gRPC cancel on either side tears the stream; channel
     state handler signals Close() via Detach path.

2. **`AudioFrame`** — POD-like view over L16 PCM + metadata.
   Construction from `switch_frame_t` (read side) and from
   `FromService::AudioFrame` proto (write side).

3. **`Resampler`** — thin wrapper around `switch_resample_t`:
   - Constructor: `(int from_hz, int to_hz)`. Allowed: 8↔16; any
     other pair → returns nullptr (caller logs + skips frame).
   - `Process(const int16_t* in, std::size_t in_samples,
            int16_t* out, std::size_t out_cap)` → `std::size_t`
     written. Reuses internal `switch_resample_t` state across calls
     (stateful FIR filter; do not free between frames).
   - Destructor: `switch_resample_destroy`.
   - Per-stream owned; not shared between threads.

**New FF entries needed**:
- FF-033 — `switch_resample_t` allocation, write/read pattern, lifecycle
  (lives across frames, not per-frame).
- FF-034 — gRPC ClientReaderWriter half-close + cancellation semantics
  for bidi streams (1.74 reference).

**Acceptance**:
- `StreamClient::Open` → mock server sees `StreamStart` then `StreamReady`
  → returns OK.
- `SendAudio` 100 frames → mock server receives 100 `AudioFrame` in seq
  order.
- Ring overflow → oldest frames dropped, counter advances, no crash.
- `Close()` → graceful half-close + reader join within 1 s.
- `Resampler(8000, 16000)` — input 160 samples (20 ms @ 8 kHz) → output
  ~320 samples (20 ms @ 16 kHz); fidelity check via 1 kHz sine RMS
  comparison within ±0.5 dB.
- `Resampler(8000, 24000)` — returns nullptr (out of supported matrix).

**Tests**: gRPC mock server (in-process) per W4C `metrics_server_test`
pattern. Resampler tested with synthesized sine waves.

## Phase 2 — Handlers (sequential after Phase 1 merges)

### Track C — TTS playback + STT + voicebot duplex handlers

Forks from `main` after Tracks A + B are merged. Cannot start earlier
(depends on `BugManager::Attach` API + `StreamClient` API).

**Files added**:
- `include/osw/control/handlers/start_tts_handler.h`
- `include/osw/control/handlers/start_stt_handler.h`
- `include/osw/control/handlers/start_voicebot_handler.h`
- `src/control/handlers/start_tts_handler.cc`
- `src/control/handlers/start_stt_handler.cc`
- `src/control/handlers/start_voicebot_handler.cc`
- `src/control/handlers/stop_media_stream_handler.cc` (one stop RPC for
  all purposes; routes by `BugHandle` lookup)
- `tests/unit/control/start_tts_handler_test.cc`
- `tests/unit/control/start_stt_handler_test.cc`
- `tests/unit/control/start_voicebot_handler_test.cc`

**Files modified**:
- `proto/open_switch/control/v1/control.proto` — add
  `StartTts` / `StartStt` / `StartVoicebot` / `StopMediaStream` RPCs +
  request/response messages.
- `src/control/control_service_skeleton.{h,cc}` — wire handlers in.
- `src/core/module.cc` — construct `MediaBugManager` + inject into
  skeleton.

**Per-handler flow** (same pattern; differences in `Purpose` + which
`On*` callbacks are wired):

1. Validate request: `channel_uuid` present + locatable; `endpoint`
   non-empty; `sample_rate_hz` in {8000, 16000}.
2. `switch_core_session_locate(uuid)` (FF-016) — rwunlock via RAII
   `SessionGuard` (already in `src/control/session_guard.{h,cc}` from W3).
3. Construct `StreamClient`. `Open()`. If `Open` fails → return
   `UNAVAILABLE` with FS-cause.
4. `BugManager::Attach(session, BugConfig{purpose, sample_rate_hz, ...})`.
   If rejected → close stream, return mapped status (`ALREADY_EXISTS`
   or `FAILED_PRECONDITION`).
5. Bug callback (file-static — FF-003) routes:
   - **TTS**: `WRITE_REPLACE` — fetches next `FromService::AudioFrame`
     from `StreamClient` rx-side queue, resamples if needed, writes to
     `switch_frame_t` payload.
   - **STT**: `READ_STREAM` — copies `switch_frame_t` payload into
     `AudioFrame`, resamples to engine's sample rate, calls
     `StreamClient::SendAudio`. Transcripts arrive via
     `StreamClient::OnTranscript` — emitted as Tier-2 event
     `osw.stt.transcript` (W2 event plane).
   - **Voicebot duplex**: BOTH `READ_STREAM` + `WRITE_REPLACE` on the
     same channel (two `BugConfig` attaches: read side at MID_READ,
     write side at INJECT). Read path = STT-style send; write path =
     TTS-style consume.
6. Return `OK` with `stream_id` (UUIDv7 minted by handler).

**StopMediaStream**: lookup `BugHandle` by `(channel_uuid, stream_id)`,
detach (RAII destructor closes the stream + joins reader thread),
return OK. Idempotent (unknown id → OK with `was_active=false`).

**Acceptance**:
- StartTts → mock server receives `StreamStart{purpose=TTS_PLAYBACK,
  sample_rate=16000}` → emits 100 `AudioFrame` → bug callback observes
  frames in write_replace → channel write side sees the audio.
- StartStt → bug callback copies caller mic frames → mock server
  receives 100 frames in order → emits 3 `Transcript` (2 interim + 1
  final) → handler emits 3 `osw.stt.transcript` Tier-2 events.
- StartVoicebot → both read + write bugs attached → bidirectional flow
  works.
- StopMediaStream — bug detached, stream closed, idempotent on second
  call.
- Channel hangup mid-stream — `DetachAll` from CS_DESTROY closes all
  streams; mock server sees gRPC cancel.

## Wave-level gates

- `make protos.lint && make build && make test` clean (ASAN amd64 +
  arm64 + TSAN).
- New TSAN test: 16-thread concurrent attach/detach against a single
  `BugManager` instance with shared channel UUID — clean.
- Codex / Gemini CLI wave review (per HARDENED memory — no Claude
  sub-agent reviews).
- W6 closeout doc with findings + SHAs.

## Sequencing

1. Plan PR (this doc) → main.
2. Phase 1: branch + spawn 2 Sonnet sub-agents in parallel (Track A on
   `wave6-track-a-bug-manager`, Track B on `wave6-track-b-stream-resample`).
3. Merge A → main; merge B → main (resolve `module.cc` conflict if
   both touch it).
4. Phase 2: branch `wave6-track-c-handlers` off post-merge main; spawn
   single Sonnet sub-agent.
5. Merge C → main.
6. Codex / Gemini wave review → fix sprint if findings → merge.
7. W7 plan (recording + eavesdrop) starts.

## Wall-time estimate

- Phase 1 (parallel): ~3-5 days
- Phase 2 (single): ~3-5 days
- Review + fix sprint: ~2 days
- **Total: ~1.5-2 weeks**
