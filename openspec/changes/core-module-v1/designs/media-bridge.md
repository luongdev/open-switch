# Media bridge — multi-bug per call

## Goal

Bridge audio between a FreeSWITCH channel and one or more external
gRPC services (TTS, STT, voicebot, AMD, recording relay), supporting
several streams **on the same call simultaneously** with well-defined
priority and lifecycle.

## Why multi-bug matters

A typical AI voicebot call has at least these media flows active at
the same time:

1. STT taps the caller's microphone audio.
2. TTS injects bot speech into the caller's earpiece.
3. (optional) Recording captures both sides for compliance.
4. (optional) VAD/barge-in detection watches caller audio to interrupt
   bot speech when the caller talks.

If a single media bug handled all of these, the ordering and isolation
would be impossible to reason about. We use one FreeSWITCH media bug
per logical concern. Ordering is governed by attach order (and the
`SMBF_FIRST` flag for head-of-chain), not by a numeric priority — see
the primer below for the FS-side mechanism.

## FreeSWITCH media bug primer

FS media bugs are inserted into a channel's audio processing pipeline.
A channel has two sides:

- **Read side**: audio received from the network (caller's mic).
- **Write side**: audio sent to the network (what the caller hears).

A media bug can attach with various flags:

| Flag | Stage | Effect |
|---|---|---|
| `SMBF_READ_STREAM` | Read tap | Receives copies of read-side frames (read-only). See FF-006: read side runs `READ_REPLACE` first as one pass, then `READ_STREAM` as a second pass, so READ_STREAM taps observe post-`READ_REPLACE` audio regardless of chain position. |
| `SMBF_WRITE_STREAM` | Write tap (in-chain) | Receives a copy of the current `write_frame` at the bug's position in the chain. **Per FF-001 the write side is a single interleaved loop**: a WRITE_STREAM tap positioned before a `WRITE_REPLACE` bug observes the pre-replace frame; positioned after, it observes the post-replace frame. There is no second pass — chain order is what matters. |
| `SMBF_READ_REPLACE` | Read transform | Replaces read-side frames before further processing. Use to inject audio "as if the caller said it" (rare). |
| `SMBF_WRITE_REPLACE` | Write transform | Replaces write-side frames before network send. Per FF-001 a `WRITE_REPLACE` bug's mutation is visible to every subsequent iteration of the same write-side loop on the same ptime tick. Use to inject bot speech. |
| `SMBF_READ_PING` | Read tick | Periodic callback on read side (no frame). |
| `SMBF_NO_PAUSE` | (modifier) | Bug runs even when channel is on hold. |
| `SMBF_THREAD_LOCK` | (modifier) | Bug callback is serialised with channel's media thread. |
| `SMBF_FIRST` | (modifier) | Prepend to head of bug chain instead of appending to tail. |

### Add-order, NOT numeric priority

A prior draft of this section claimed `switch_core_media_bug_add`
takes a numeric priority via the high byte of `flags`. **That is
false.** Verified against `signalwire/freeswitch`
`src/switch_core_media_bug.c` (Phase 1 Codex finding C-1):

```c
if (!session->bugs) {
    session->bugs = bug;          // first bug: head
} else if (switch_test_flag(bug, SMBF_FIRST)) {
    bug->next = session->bugs;    // SMBF_FIRST: prepend
    session->bugs = bug;
} else {
    for (bp = session->bugs; bp->next; bp = bp->next) {}
    bp->next = bug;                // default: append to tail
}
```

The ONLY ordering controls are:

1. **Add order** — bugs are walked head→tail in attach order, except
2. **`SMBF_FIRST`** — bug is prepended to head (used to ensure a bug
   sees raw frames before any other tap or transform).

There is no numeric priority. Two bugs of the same stage (both
READ_STREAM, both WRITE_REPLACE, etc.) execute in the order they
were attached.

### Semantic ordering between stages (read vs write asymmetry)

The read and write sides of a FreeSWITCH channel are **not
symmetric** in how they iterate the bug chain. Round 2 of the Codex
review claimed both sides ran in two passes; that was false against
FS v1.10.12 source. The correct behaviour is:

**Read side — two passes** (FF-006, `src/switch_core_io.c:646-756`):

```text
For each ptime tick on read side:
  Pass 1: walk bugs in chain order; for each bug with SMBF_READ_REPLACE
          run its callback; the bug may replace read_frame for subsequent
          iterations (the same loop).
  Pass 2: walk bugs in chain order; for each bug with SMBF_READ_STREAM
          buffer the (now post-REPLACE) read_frame and run its callback.
  Then:   FS hands the (possibly replaced) frame upstream.
```

Because all READ_REPLACE callbacks run before any READ_STREAM
callback, a READ_STREAM tap observes the post-replace frame
regardless of chain position. The module does not use READ_REPLACE
in V1, so this distinction has no operational effect today; reads
pass through to STT / VAD / AMD taps unchanged.

**Write side — single interleaved loop** (FF-001,
`src/switch_core_media.c:16096-16156`):

```text
For each ptime tick on write side:
  FS produces the base write_frame (from playback file, hold music,
    silence, or upstream bridged leg).

  Single loop over bugs in chain order:
    For each bug:
      if SMBF_WRITE_STREAM: buffer the CURRENT write_frame and run
        callback with SWITCH_ABC_TYPE_WRITE.
      if SMBF_WRITE_REPLACE: run callback with
        SWITCH_ABC_TYPE_WRITE_REPLACE; if the callback returns TRUE,
        mutate write_frame for the NEXT iteration (i.e., for bugs
        positioned later in the chain).

  Then: FS encodes write_frame and sends to network.
```

The crucial consequence: a `WRITE_STREAM` tap observes the
**pre-replace** frame if it is positioned BEFORE the `WRITE_REPLACE`
bug in the chain, and the **post-replace** frame if it is positioned
AFTER. Chain order is what matters. There is no FS-side "all
WRITE_REPLACE bugs run first, then all WRITE_STREAM bugs run"
two-pass guarantee on the write side.

For our V1 module this means recording captures bot audio **only
when the recording bug is positioned after all `WRITE_REPLACE` bugs
(TTS, voicebot-duplex write half) in the chain**. Bug insertion
follows FF-007 (head-on-`SMBF_FIRST`, tail otherwise), so the
required chain ordering must be achieved by attach order or by
`SMBF_FIRST` placement of the recording bug — see the
MediaBugManager rules below for how we enforce this for bugs we
own, and `recording-with-bot.md` for the operator-side dialplan
requirements when `record_session` (FS-native) is in play.

## Ordering and the MediaBugManager add-order coordinator

Since FS gives us add-order + `SMBF_FIRST` and nothing else, the
`MediaBugManager` enforces a **canonical attach order** and refuses
attaches that would violate it.

Each `Purpose` has an associated **stage rank**:

| Stage rank | Purpose | Flags | Why this rank |
|---|---|---|---|
| EARLY | `VAD_BARGE_IN` | `READ_STREAM` + `SMBF_FIRST` | Must see raw mic audio before any other read tap (cheap, latency-sensitive). |
| EARLY | (FS-internal eavesdrop bugs) | `READ_STREAM + WRITE_STREAM` | FS attaches these via `mod_dptools`; we don't control them. |
| MID_READ | `STT_TRANSCRIBE` | `READ_STREAM` | Tap caller mic after VAD. |
| MID_READ | `AMD_DETECT` | `READ_STREAM` | Tap caller mic after VAD. |
| MID_READ | `VOICEBOT_DUPLEX` (read half) | `READ_STREAM` | Tap caller mic after VAD. |
| INJECT | `TTS_PLAYBACK` | `WRITE_REPLACE` | Inject bot audio onto write side. |
| INJECT | `VOICEBOT_DUPLEX` (write half) | `WRITE_REPLACE` | Inject bot audio onto write side. |
| LATE | `RECORDING_RELAY` (read tap) | `READ_STREAM` | Tap caller mic. Per FF-006, read-side READ_STREAM runs in a second pass after all READ_REPLACE bugs, so chain position relative to other read taps is cosmetic for the audio outcome. |
| LATE | `RECORDING_RELAY` (write tap) | `WRITE_STREAM` | **Must be positioned AFTER all INJECT bugs in the chain to observe post-injection audio** (FF-001 — write side is a single interleaved loop, not a two-pass design). The MediaBugManager enforces this for bugs we own by attach-order gating; FS-native `record_session` (operator dialplan) needs `start_bot` BEFORE `record_session` for the same effect (see `recording-with-bot.md`). |
| LATE | `TEST` / instrumentation | any | Should not perturb production bugs. |

`MediaBugManager.Attach` enforcement:

- The first attach for a session is unrestricted.
- Subsequent attaches must have a stage rank ≥ all already-attached
  bugs' ranks. (You may add LATE after MID_READ; you may not add
  EARLY after MID_READ unless `SMBF_FIRST` is set, in which case
  the manager passes `SMBF_FIRST` to FS to prepend — sound but
  documented.)
- `VAD_BARGE_IN` always attaches with `SMBF_FIRST` to ensure it
  prepends regardless of add timing.
- A duplicate `Purpose` per channel is rejected with
  `INVALID_ARGUMENT` (one VAD, one STT, etc. per channel).

The manager keeps a tiny stage-rank-per-bug record. On `Attach`:

```cpp
Result<BugHandle> MediaBugManager::Attach(
    switch_core_session_t* session,
    const BugConfig& cfg) {
  auto& reg = sessions_[uuid];
  if (HasPurpose(reg, cfg.purpose)) {
    return Err(ALREADY_EXISTS, "purpose already attached");
  }
  uint32_t this_rank = StageRank(cfg.purpose);
  uint32_t max_existing_rank = MaxRank(reg);
  uint32_t flags = cfg.flags;
  if (cfg.purpose == Purpose::VAD_BARGE_IN) {
    flags |= SMBF_FIRST;
  } else if (this_rank < max_existing_rank) {
    return Err(FAILED_PRECONDITION,
        "out-of-order attach: expected stage rank >= existing");
  }
  // ... create lease, register, return handle
}
```

### Operator contract: dialplan / orchestration order

Operators starting bugs from the dialplan or via control RPCs must
respect the stage ordering:

1. Start VAD (if used).
2. Start STT / AMD / voicebot read half.
3. Start TTS / voicebot write half (INJECT).
4. Start recording relay (if used) LAST.

The dialplan recipe in `recording-with-bot.md` documents this for
the recording case. For the WRITE_STREAM recording specifically,
the order is enforced by FS semantic rather than by us — but the
manager still asks operators to follow the canonical sequence for
clarity and to avoid surprising `READ_STREAM` ordering.

### FreeSWITCH-native bugs the module does not control

`record_session`, `mod_spy`, native eavesdrop, etc. attach bugs
directly via `switch_core_media_bug_add` without going through our
`MediaBugManager`. They land per FF-007: at the head of the chain
if `SMBF_FIRST` is set, otherwise at the tail at the moment of
attach. The MediaBugManager can refuse to attach module-owned bugs
in a way that would violate the canonical order, but it cannot
reorder FS-native bugs that operators install from the dialplan.

```text
Bug chain at the moment of audio processing for a typical bot call
(canonical add order: VAD → STT → AMD → TTS → record_session if
operator follows guidance):

  [head]              VAD (osw)  ◀── always at head via SMBF_FIRST
                      STT (osw)                                READ_STREAM
                      AMD (osw)                                READ_STREAM
                      TTS (osw)                                WRITE_REPLACE
                      record_session (FS-native, operator-controlled)
                                                               READ_STREAM + WRITE_STREAM
                      RECORDING_RELAY (osw, if used)           READ_STREAM + WRITE_STREAM
  [tail]
```

**Audio outcome per stage**:

- READ_STREAM bugs (STT, AMD, record_session read-tap,
  RECORDING_RELAY read-tap): per FF-006 the read side runs
  READ_REPLACE in a first pass and READ_STREAM in a second pass.
  Because the V1 module does not use READ_REPLACE, every
  READ_STREAM tap sees the raw caller frame; relative chain order
  among them is cosmetic.
- WRITE_REPLACE (TTS, voicebot-duplex write half): each
  WRITE_REPLACE runs in its position in the single write-side
  loop (FF-001). Its mutation is visible to **subsequent**
  iterations of the same loop only.
- WRITE_STREAM (record_session, RECORDING_RELAY write-tap):
  observes `write_frame` AT ITS POSITION IN THE CHAIN. If it
  precedes TTS, it sees pre-injection audio; if it follows, it
  sees post-injection audio. **Add order matters.**

The MediaBugManager's stage-rank gate enforces the desired order
for bugs we own. For FS-native `record_session`, the spec
`recording-with-bot.md` documents the operator-side dialplan
ordering requirement: start bot **before** `record_session` if the
recording must contain bot audio.

## MediaBugManager

Per-channel object that:

- Holds the set of active bugs (keyed by purpose).
- Allocates a `MediaBugLease` (RAII) per bug.
- Coordinates teardown when the channel is hangup'd.
- Routes audio frames between bug callbacks and gRPC streams.

```cpp
namespace osw::media {

enum class Purpose {
  TTS_PLAYBACK,
  STT_TRANSCRIBE,
  VOICEBOT_DUPLEX,
  AMD_DETECT,
  RECORDING_RELAY,
  VAD_BARGE_IN,
  TEST,
};

struct BugConfig {
  Purpose purpose;
  uint32_t flags;          // SMBF_* combinations (manager may OR in SMBF_FIRST)
  uint32_t target_rate_hz; // 8000 / 16000 / 24000 / 48000
  std::string upstream_endpoint;
  std::string tenant_id;
  std::map<std::string, std::string> variables;
};

class MediaBugManager {
 public:
  // Attach a bug to the channel. Returns a handle; releasing the
  // handle removes the bug.
  //
  // Returns an error if a bug for the same purpose already exists on
  // this channel, or if the attach would violate the canonical stage
  // ordering (FAILED_PRECONDITION).
  Result<BugHandle> Attach(switch_core_session_t* session,
                          const BugConfig& cfg);

  // Detach a specific bug (idempotent).
  void Detach(BugHandle handle);

  // Detach all bugs on this channel. Called from the channel destroy
  // state handler.
  void DetachAll(const std::string& channel_uuid);

  // Stats.
  size_t ActiveBugCount(const std::string& channel_uuid) const;
};

}  // namespace osw::media
```

Lifecycle owned by the channel:

1. State handler `on_init`: empty registry entry created.
2. Control RPC or dialplan action: bugs attached.
3. Audio flows.
4. State handler `on_hangup`: graceful detach all bugs in this channel.
5. State handler `on_destroy`: ensure registry entry is gone.

## Audio flow per bug

Each attached bug runs the FS callback on the channel's media thread:

```cpp
static switch_bool_t osw_bug_callback(switch_media_bug_t* bug,
                                      void* user_data,
                                      switch_abc_type_t type) {
  auto* state = static_cast<BugState*>(user_data);
  try {
    switch (type) {
      case SWITCH_ABC_TYPE_INIT:
        // Open gRPC stream, set initial state.
        return state->Init(bug);
      case SWITCH_ABC_TYPE_READ:        // SMBF_READ_STREAM
      case SWITCH_ABC_TYPE_WRITE:       // SMBF_WRITE_STREAM
        return state->OnFrame(bug, type);
      case SWITCH_ABC_TYPE_WRITE_REPLACE:
        return state->OnWriteReplace(bug);
      case SWITCH_ABC_TYPE_READ_REPLACE:
        return state->OnReadReplace(bug);
      case SWITCH_ABC_TYPE_CLOSE:
        return state->Close(bug);
      default:
        return SWITCH_TRUE;
    }
  } catch (const std::exception& e) {
    osw::log::Error("media bug callback exception: {}", e.what());
    return SWITCH_FALSE;
  } catch (...) {
    osw::log::Error("media bug callback unknown exception");
    return SWITCH_FALSE;
  }
}
```

C-callable. Exceptions are caught. Returns `SWITCH_FALSE` on error so
FS removes the bug.

### Read tap (`SMBF_READ_STREAM`)

Used for STT, VAD, AMD, recording-read. The callback is invoked with a
copy of the read frame. We:

1. Read raw frame from FS via `switch_core_media_bug_get_read_replace_frame`
   or `switch_core_media_bug_read`.
2. Resample to target rate (e.g., 16 kHz for STT, 8 kHz for AMD).
3. Encode if requested (default PCM_S16LE).
4. Push to the gRPC stream's send queue (lock-free SPMC ring).
5. Return `SWITCH_TRUE`.

The gRPC sender goroutine (one per bug) drains the ring and writes to
the stream. We do NOT block the FS media thread on gRPC backpressure —
the ring is bounded (~200 ms of audio); on overflow we drop oldest
and emit a Tier-2 event `bug_send_overflow`.

### Write replace (`SMBF_WRITE_REPLACE`)

Used for TTS playback. The callback is invoked when FS is about to send
a write frame. We:

1. Check the receive ring for queued frames from the gRPC stream.
2. If the ring has audio: pop a frame, resample to the channel rate,
   encode to the channel codec (FS handles G.711/Opus encoding —
   we feed PCM_S16LE), call `switch_core_media_bug_set_write_replace_frame`
   to inject.
3. If empty: feed silence (return `SWITCH_TRUE` without setting a
   replace frame, so the channel's normal write data goes through —
   typically silence or hold music).

The gRPC receiver goroutine drains incoming TTS frames from the
stream and enqueues to the receive ring (~500 ms capacity). Overflow
on the receive side means the TTS service is sending faster than the
caller can play — we drop oldest and emit a Tier-2 event
`bug_receive_overflow`.

### Read replace (`SMBF_READ_REPLACE`)

Rarely used. The use case is "send synthesized speech in as if the
caller said it" — useful for ASR/IVR self-tests but not production. V1
supports the API but no production purpose uses it.

## gRPC stream lifecycle per bug

```text
Bug Attach
   │
   ▼
gRPC client connects to upstream service (channel pool, see below)
   │
   ▼
Send StreamStart { channel_uuid, tenant_id, purpose, sample_rate, ... }
   │
   ▼
Receive StreamReady { negotiated_rate, server_stream_id }
   │
   ▼
Bidirectional audio flow
   │   - Bug callback enqueues outbound frames (READ tap)
   │   - gRPC sender drains, sends AudioFrame
   │   - gRPC receiver enqueues inbound frames (for WRITE_REPLACE)
   │   - Bug callback dequeues + injects
   │
   ▼
(any of)
  - Bug Detach (control RPC / hangup state handler)
  - Channel hangup (state handler on_hangup → Detach)
  - Upstream service closes stream
  - gRPC stream timeout
   │
   ▼
gRPC client half-closes its send side
   │
   ▼
Drain inbound for up to `drain_timeout_ms` (default 2s)
   │
   ▼
Cancel stream (if drain didn't complete cleanly)
   │
   ▼
Release MediaBugLease (removes the bug)
   │
   ▼
Emit Tier-2 event `bug_closed` with stats (frames sent/received, errors)
```

### gRPC channel pool

One `grpc::Channel` per (upstream_endpoint, TLS config) tuple. Reused
across many bugs to amortise HTTP/2 + TLS handshake.

Channels are created lazily on first use; `WaitForConnected(1s)` to
fail fast if the service is unreachable. On failure, the bug Attach
returns an error and the channel state cycles to RECONNECTING for the
next attempt.

Channels are torn down on module unload or when **idle for >
`upstream_channel_idle_timeout_seconds`** (default `1800` = 30 min).
Phase 1 Codex finding I-2: a long-quiet mid-call period (hold music,
IVR menu) longer than the idle timeout would tear down the channel
and the next frame pays a full TLS handshake. 30 min default covers
realistic call durations; operators with longer-quiet patterns crank
up. The previous 5-minute default was too aggressive.

### Cancellation propagation

Three cancellation paths:

1. **Channel hangup** (caller hangs up): FS state handler
   `on_hangup` runs → DetachAll for that channel → for each bug, the
   gRPC stream is closed → upstream service sees end-of-stream.
2. **Upstream service closes**: gRPC client reactor sees server-half
   close → MediaBugManager removes the bug → if no more bugs for
   that purpose, the channel continues normally (e.g., caller hears
   silence on the write side instead of bot).
3. **Module shutdown**: drain procedure (`drain_timeout_seconds`)
   cancels all bugs, all streams.

In ALL paths, the `MediaBugLease` destructor is the actual remove
operation. The lease is held by either the registry (per channel) or
by the upstream client. When the last reference drops, the bug is
removed.

## Resampling

FS provides audio at the channel's native rate (typically 8 kHz for
G.711, 16 kHz for G.722, 48 kHz for Opus). External services typically
want a normalised rate per purpose:

| Purpose | Typical service rate |
|---|---|
| STT (general) | 16 kHz |
| AMD | 8 kHz |
| TTS playback | 24 kHz (model-side) or whatever the model emits |
| Voicebot duplex | model-specific, often 16 kHz |
| Recording relay | 8 kHz mono or 48 kHz stereo |

We resample using FreeSWITCH's `switch_audio_resampler_t` (which wraps
libsamplerate / Speex resampler). One resampler per direction per bug.
Resampling cost at 8 kHz → 24 kHz is ~50 µs/frame on modern CPU;
negligible at 50 CCU.

For recording relay stereo, we maintain two resamplers + interleave
L/R samples into a single payload buffer before send.

## Codec handling

The gRPC stream protocol (`open_switch.media.v1`) supports:

- `PCM_S16LE` (default; clean format, no codec dependency)
- `G711_ULAW` / `G711_ALAW` (8 kHz; for when the upstream wants telco rate)
- `OPUS` (variable; for high-quality)

In V1, we recommend `PCM_S16LE` as the default. Channel-to-PCM
conversion is handled by FS (`switch_core_media_bug_read` returns
linear PCM). Upstream-to-channel conversion is also PCM-first; FS
handles encoding back to the channel codec when we call
`switch_core_media_bug_set_write_replace_frame`.

Opus over gRPC stream is implemented but defaulted off — the
encode/decode cost in-module isn't worth it when the network is
already low-latency.

## Frame format details

Internal struct (in-process), not the wire format:

```cpp
struct AudioFrame {
  uint64_t seq;                  // 0-based within stream
  uint64_t timestamp_samples;    // monotonic, samples since stream start
  uint32_t duration_samples;     // typically 160 for 20 ms @ 8 kHz
  std::vector<uint8_t> payload;  // PCM/G.711/Opus
  Channel channel;               // LEFT, RIGHT, BOTH_INTERLEAVED (stereo recording only)
};
```

Wire format on the gRPC stream is
`open_switch.media.v1.AudioFrame` (protobuf). The `bytes payload` field
on the wire holds the audio bytes contiguous (no length prefix beyond
protobuf's own framing). For BOTH_INTERLEAVED stereo: samples are
interleaved `[L0 R0 L1 R1 ...]` as int16 little-endian per sample, and
`duration_samples` counts L+R pairs (so a 20 ms @ 8 kHz stereo frame
has `duration_samples=160` and `payload.size()=160*4` bytes). Phase 1
Codex finding N-3 (wire vs internal struct distinction).

Frame duration matches the FS frame duration on the channel (usually
20 ms). We do NOT re-frame. If the upstream service wants 30-ms frames
it must re-frame on its side.

`seq` wraps at 2^64; for realistic call durations this never wraps
(2^64 frames @ 50/s = ~12 billion years).

## Multi-stream per call: voicebot duplex

The `VOICEBOT_DUPLEX` purpose is special: one gRPC stream carries
both the caller's read audio AND the service's response audio. The
module attaches TWO media bugs (one READ_STREAM, one WRITE_REPLACE),
both feeding into the same `StreamHandle`. The protocol distinguishes
direction via the `StreamStart.side` field (`CALLER_MIC` vs
`CALLER_EAR`); for duplex the side is implicit (bidi stream).

This saves one TCP connection vs running separate STT + TTS streams,
and gives the service a single barge-in decision point.

### Head-of-line blocking caveat (Codex I-1)

Because a single gRPC bidi stream carries both directions, the gRPC
flow-control window is shared. If the service is slow to read
inbound caller audio (e.g., overloaded STT), the gRPC writer at the
module side back-pressures, which delays outbound writes — including
the TTS frames the service is sending back. Result: gaps in bot
playback while the service catches up.

V1 mitigation:

- Per-direction send queues at the module side decouple FS callback
  latency from gRPC flush latency (FS callback enqueues to module
  ring; gRPC sender thread drains).
- The module reserves a minimum slice of gRPC outbound flow-control
  for inbound frames (the implementation uses HTTP/2 stream priorities
  via gRPC channel args, where supported).
- Operators with frequent service-side slowness should run **two
  separate streams** (TTS + STT_TRANSCRIBE) instead of
  VOICEBOT_DUPLEX. The trade-off is one extra TCP connection and a
  separate barge-in decision point.

V1.5 may add automatic stream-split when the module detects sustained
inbound-direction stall. V1 spec ships duplex as opt-in with
operator awareness of this caveat.

## Stats per bug

Each bug exposes counters via `MediaBugManager`:

- `frames_sent` / `frames_received`
- `bytes_sent` / `bytes_received`
- `send_queue_overflow_count`
- `receive_queue_overflow_count`
- `resample_error_count`
- `last_frame_at` (timestamp)
- `stream_age_ms`

Aggregated into the `Health` RPC response as `active_media_bugs`.

## Failure modes

| Failure | Detection | Behavior |
|---|---|---|
| gRPC channel can't connect | `WaitForConnected` timeout | Attach returns error; control RPC caller decides retry. |
| Stream errors mid-call | `grpc::Status` from sender/receiver reactor | Tear down bug; emit Tier-2 `bug_stream_lost`; channel continues without that bug. |
| Bug callback throws | `catch (...)` returns SWITCH_FALSE | FS removes bug; we emit Tier-2 `bug_callback_error`. |
| Send queue overflow | Ring full | Drop oldest frame; counter; Tier-2 event (rate-limited to 1/min). |
| Receive queue overflow | Ring full on inbound | Drop oldest frame; counter; Tier-2 event (rate-limited). |
| Resample failure | Resampler returns 0 samples | Drop frame; counter; warning log. |
| Channel ends mid-frame | State handler `on_hangup` | DetachAll; in-flight frames are abandoned. |
| Duplicate `Purpose` on same channel | Attach validation | Reject second Attach with `ALREADY_EXISTS`. |
| Out-of-order attach (e.g. INJECT then EARLY without `SMBF_FIRST`) | Stage-rank check | Reject Attach with `FAILED_PRECONDITION`; operator fixes ordering or uses `SMBF_FIRST` explicitly. |
| Bug count limit hit | Configurable max per channel (default 8) | Reject Attach with `RESOURCE_EXHAUSTED`. |

## Performance budget

At 50 CCU with 1 STT + 1 TTS + 1 recording bug per call = 150 active
media bugs, each running at 50 frames/sec (20 ms frames):

- 7500 frames/sec total across all bugs.
- Per-frame cost (best estimate): bug callback 5 µs + resample 50 µs
  + ring enqueue 1 µs = ~56 µs.
- Total CPU on bug callbacks: 7500 × 56 µs = 420 ms/sec of one core,
  or ~0.5 core at 50 CCU.
- gRPC sender threads: one per bug, but mostly sleeping (drains ring
  at 50 fps). Thread pool sized at active_bug_count × 2 for headroom.

Memory:
- Per bug: ~16 KB (ring buffers, resampler state, gRPC stream state).
- 150 bugs: ~2.5 MB.

These numbers are estimates; the load test in CI nightly validates
them.

## Test plan

| Test | What it proves |
|---|---|
| `attach_detach_no_leak` | 1000 attach/detach cycles, ASAN+LSAN clean. |
| `duplicate_purpose_rejected` | Two TTS attaches on the same channel: second returns `ALREADY_EXISTS`. |
| `out_of_order_attach_rejected` | INJECT then MID_READ without `SMBF_FIRST`: second returns `FAILED_PRECONDITION`. |
| `vad_first_flag_set` | VAD attach with no flags: manager OR's in `SMBF_FIRST` before forwarding to FS. |
| `write_stream_after_write_replace_captures_bot` | TTS WRITE_REPLACE attached FIRST, then RECORDING_RELAY WRITE_STREAM attached SECOND → chain is `[TTS, recording]` → recording's WRITE_STREAM callback observes the post-injection frame (FF-001). Asserts bot audio is in the recording. |
| `write_stream_before_write_replace_misses_bot` | RECORDING_RELAY WRITE_STREAM attached FIRST, then TTS WRITE_REPLACE attached SECOND via the MediaBugManager → manager returns `FAILED_PRECONDITION` (out-of-order). Asserts the manager refuses to attach an INJECT bug after a LATE bug. (Documents the inverse-case rejection.) |
| `record_session_before_tts_misses_bot_audio` | Operator dialplan calls `record_session` (FS-native) FIRST, then `start_bot` (which attaches our TTS WRITE_REPLACE). Chain is `[record_session, TTS]` → record_session's WRITE_STREAM tap observes pre-injection frame. Asserts recording does NOT contain bot audio. Documents the operator-side ordering requirement loudly. |
| `multiple_bugs_per_channel` | STT + TTS + recording attached on one channel. Frames flow on each. |
| `cancel_on_hangup` | Channel hangup detaches all bugs within 50 ms. |
| `cancel_on_module_shutdown` | Module unload drains all bugs gracefully. |
| `send_overflow_drops_oldest` | Saturate sender; verify oldest frames dropped and counter increments. |
| `resample_round_trip` | PCM frame → resampler → reverse → assert ≤ 1 LSB error. |
| `grpc_reconnect_mid_call` | Kill upstream gRPC service during a call; the bug emits stream_lost; channel continues. |
| `tts_stream_lost_silent_or_reconnect` | TTS bug loses upstream stream mid-call. Per `tts_reconnect_on_loss` config (default `true`): module attempts re-attach with exponential backoff (1s, 2s, 4s, capped at 30s) for `tts_reconnect_max_seconds` (default 30s). On reconnect success: bot continues. On give-up: silent write side, emit Tier-2 `bot_stream_abandoned`. Phase 1 Codex finding I-3 — silence-without-recovery is not a graceful degradation for unsupervised bots. |
| `duplex_single_stream` | VOICEBOT_DUPLEX uses one stream for both directions; assert correct interleaving. |
| `stereo_recording_relay` | Recording relay produces stereo with caller=L, bot=R. |

## Operator knobs

| Knob | Default | Purpose |
|---|---|---|
| Max bugs per channel | 8 | Prevent runaway attach loops |
| Per-bug send ring | 200 ms of audio | Larger = more tolerance to network hiccup, more memory |
| Per-bug receive ring | 500 ms of audio | Larger = more buffer for TTS bursts |
| Stream drain timeout | 2000 ms | Graceful shutdown deadline |
| gRPC channel idle timeout | 5 min | Tear down unused upstream connections |
| Resampler quality | medium | Tradeoff CPU vs quality (low/medium/high) |
