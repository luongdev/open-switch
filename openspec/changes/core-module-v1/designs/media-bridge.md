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
per logical concern, with explicit priority (= position in the audio
chain).

## FreeSWITCH media bug primer

FS media bugs are inserted into a channel's audio processing pipeline.
A channel has two sides:

- **Read side**: audio received from the network (caller's mic).
- **Write side**: audio sent to the network (what the caller hears).

A media bug can attach with various flags:

| Flag | Effect |
|---|---|
| `SMBF_READ_STREAM` | Receives copies of read-side frames (read-only tap). |
| `SMBF_WRITE_STREAM` | Receives copies of write-side frames (read-only tap). |
| `SMBF_READ_REPLACE` | Replaces read-side frames before further processing. Use to inject audio "as if the caller said it" (rare). |
| `SMBF_WRITE_REPLACE` | Replaces write-side frames. Use to inject bot speech. |
| `SMBF_READ_PING` | Periodic callback on read side (no frame). |
| `SMBF_NO_PAUSE` | Bug runs even when channel is on hold. |
| `SMBF_THREAD_LOCK` | Bug callback is serialised with channel's media thread. |

Bug priority is set at `switch_core_media_bug_add` via the `flags`
field (high byte) and via add-order. The earlier-added bug runs
earlier in the chain. We use an explicit priority allocator (below)
rather than depending on add-order.

## Priority allocation

Lower priority number = runs earlier in the chain.

| Priority | Purpose | Flags | Notes |
|---|---|---|---|
| 100 | VAD / barge-in detector | `READ_STREAM` | Tap raw read first; cheap analysis. |
| 200 | STT | `READ_STREAM` | Tap raw read for ASR; ASR sees same audio VAD sees. |
| 300 | AMD detector | `READ_STREAM` | Tap raw read for AMD (mutually exclusive with full voicebot in practice). |
| 500 | Bot TTS write | `WRITE_REPLACE` | Inject bot speech onto write side. |
| 700 | Recording (read tap) | `READ_STREAM` | Tap read AFTER all read-side analysis. |
| 750 | Recording (write tap) | `WRITE_STREAM` | Tap write AFTER bot inject — so recording captures bot. |
| 900 | Test/instrumentation | `READ_STREAM` / `WRITE_STREAM` | For debug / load-test bugs. |

Priority 0-99: reserved for FreeSWITCH-internal modules.

The MediaBugManager rejects priority overlap (two bugs requesting the
same priority value) at registration time — operators add their own
bugs at non-default priorities to avoid collisions.

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
  uint32_t priority;
  uint32_t flags;          // SMBF_* combinations
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
  // Returns an error if a bug for the same purpose already exists,
  // or if priority collides with an existing bug.
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

Channels are torn down on module unload or when idle for >5 minutes.

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

```cpp
struct AudioFrame {
  uint64_t seq;                  // 0-based within stream
  uint64_t timestamp_samples;    // monotonic, samples since stream start
  uint32_t duration_samples;     // typically 160 for 20 ms @ 8 kHz
  std::vector<uint8_t> payload;  // PCM/G.711/Opus
  Channel channel;               // LEFT, RIGHT, BOTH_INTERLEAVED (stereo recording only)
};
```

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
| Two purposes with same priority | Attach validation | Reject second Attach with INVALID_ARGUMENT. |
| Bug count limit hit | Configurable max per channel (default 8) | Reject Attach with RESOURCE_EXHAUSTED. |

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
| `priority_collision_rejected` | Attach with same priority returns error. |
| `multiple_bugs_per_channel` | STT + TTS + recording attached on one channel. Frames flow on each. |
| `cancel_on_hangup` | Channel hangup detaches all bugs within 50 ms. |
| `cancel_on_module_shutdown` | Module unload drains all bugs gracefully. |
| `send_overflow_drops_oldest` | Saturate sender; verify oldest frames dropped and counter increments. |
| `resample_round_trip` | PCM frame → resampler → reverse → assert ≤ 1 LSB error. |
| `grpc_reconnect_mid_call` | Kill upstream gRPC service during a call; the bug emits stream_lost; channel continues. |
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
