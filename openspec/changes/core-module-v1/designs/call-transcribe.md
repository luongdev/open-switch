# Call transcribe (STT path)

## Why

Call transcription is required for V1 by project owner ("Phục vụ được
cả việc call-transcribe là điều bắt buộc"). The module must support
STT alongside TTS playback, on the same call, without coupling them.

## Where STT fits in the architecture

STT is one of the `Purpose` values supported by the
[`media-bridge.md`](media-bridge.md) MediaBugManager. The module is
the gRPC client; an external STT service implements the server side
of `open_switch.media.v1.MediaBridge`.

The module:

1. Attaches a `SMBF_READ_STREAM` bug at the **`MID_READ`** stage
   rank (per the stage-rank table in `media-bridge.md`). There is
   no numeric priority allocator in FreeSWITCH (see FREESWITCH-FACTS
   FF-007); the MediaBugManager controls ordering by add-order +
   `SMBF_FIRST` only.
2. Reads the caller's audio frames (8 kHz typical for telephony).
3. Resamples to the STT service's preferred rate (16 kHz default).
4. Encodes to `PCM_S16LE`.
5. Streams frames over gRPC.
6. Receives `Transcript` messages back (interim + final).
7. Optionally writes the transcripts to channel variables (e.g.,
   `variable_stt_latest_transcript`) for downstream use.

## What V1 STT does NOT do

- Does NOT run any STT model inside the module. The module is a relay;
  the model is in the external service.
- Does NOT decide what to do with transcripts. That's orchestrator
  logic. The module emits a Tier-2 event with each transcript so
  consumers can react.
- Does NOT diarise (separate speakers). Caller-only by default. For
  diarised recording, use the `RECORDING_RELAY` purpose with stereo.
- Does NOT correlate with TTS playback (e.g., for self-confirmation).
  Orchestrator can stand up both an STT bug AND a `WRITE_STREAM` tap
  on the bot side for that, but V1 doesn't bundle this pattern.

## Bug attachment

Started via control RPC (we add a method on top of `ControlService`
in [`specs/control-api/spec.md`](../specs/control-api/spec.md):

```protobuf
rpc StartStt(StartSttRequest) returns (StartSttResponse);
rpc StopStt(StopSttRequest) returns (StopSttResponse);
```

`StartStt` parameters:

- `channel_uuid` — required
- `stt_endpoint` — gRPC URL of the external STT service
- `language` — BCP-47, e.g., "vi-VN" (passed to STT service; module
  doesn't interpret)
- `sample_rate_hz` — default 16000; channel rate is converted
- `interim_results` — bool; if true, the STT service should emit
  interim (partial) transcripts in addition to finals
- `vocabulary_hints` — optional list of words to bias the recogniser
- `tenant_id` — for ACL gating and for tagging emitted Tier-2
  `osw::stt::transcript` events with tenant scope. The module no
  longer ships events to any in-process transport (post-F0,
  `transport-adr.md`); subscribers receive the tenant_id-tagged
  events via gRPC `SubscribeEvents` and route as they choose.

Alternatively, started inline via Originate's `after_answer`:

```protobuf
OriginateRequest.after_answer = AppSequence {
  apps: [
    { name: "answer" },
    { name: "set", args: "tenant_id=acme" },
    { name: "set", args: "language=vi-VN" },
    { name: "lua", args: "start_stt.lua" },
  ]
}
```

`start_stt.lua` calls the gRPC `StartStt` for the current channel.

## Audio chain

```text
Caller's mic
    │  RTP @ 8 kHz G.711 (typical)
    ▼
FS sofia → media engine → decode → 8 kHz PCM linear
    │
    │ (other read-side bugs may exist: VAD at EARLY stage rank,
    │  attached with SMBF_FIRST so it sits at the head of the chain)
    ▼
STT bug — MID_READ stage rank, SMBF_READ_STREAM
    │
    │ Per FF-006 the read side runs READ_REPLACE first as one pass,
    │ then READ_STREAM as a second pass. The module does not use
    │ READ_REPLACE in V1, so chain order among READ_STREAM bugs is
    │ cosmetic for the audio outcome — STT sees the raw caller frame.
    │
    │ copy of frame
    ▼
Resampler 8 kHz → 16 kHz
    │
    ▼
PCM_S16LE encode (no-op; already linear)
    │
    ▼
Bug send ring (200 ms capacity)
    │
    │ gRPC sender thread drains
    ▼
gRPC MediaBridge.Stream(StreamStart{purpose=STT_TRANSCRIBE},
                        AudioFrame, AudioFrame, ...)
    │
    ▼
External STT service
    │
    │ Transcript { text, final, start_offset_ms, end_offset_ms,
    │              confidence, language }
    ▼
gRPC receiver thread
    │
    ▼ Emit Tier-2 event "stt_transcript" with payload
    ▼ (optional) switch_channel_set_variable(channel,
                  "variable_stt_latest_transcript", text)
```

## Event emission

Each transcript is emitted as a Tier-2 CUSTOM event with subclass
`osw::stt::transcript`:

```
Event-Name: CUSTOM
Event-Subclass: osw::stt::transcript
Unique-ID: <channel_uuid>
tenant_id: <tenant>
stt_text: <transcript text>
stt_final: true|false
stt_confidence: 0.87
stt_language: vi-VN
stt_start_ms: 1234
stt_end_ms: 1456
```

This goes through the in-module tier router; default rule classifies
it as Tier 2. Per the post-F0 design (`transport-adr.md`), gRPC
`SubscribeEvents` is the sole event transport: subscribers receive
the tagged envelope and persist / forward as they choose. Operators
may reclassify to Tier 1 in `open_switch.conf.xml` if transcripts
are compliance-critical (e.g., financial trading floor) — that
changes the tier ring it lands in and the durability promise the
operator must make at the subscriber side.

## Codec considerations

| Caller codec | Channel native rate | Resample to STT 16 kHz | CPU cost |
|---|---|---|---|
| G.711 µ-law / A-law | 8 kHz | 8 → 16 kHz upsample | ~50 µs/frame |
| G.722 | 16 kHz | identity | 0 |
| Opus 24 kHz | 24 kHz | 24 → 16 downsample | ~80 µs/frame |
| Opus 48 kHz | 48 kHz | 48 → 16 downsample | ~120 µs/frame |

The STT service can request a non-default rate via `StreamStart.sample_rate_hz`.
We honour up to 48 kHz; reject sample rates outside [8000, 48000] with
`INVALID_ARGUMENT`.

## Interaction with bot TTS

If the call has BOTH a TTS bug (INJECT stage rank, `WRITE_REPLACE`)
AND an STT bug (MID_READ stage rank, `READ_STREAM`), they coexist
independently:

- STT taps read side; bot speech does not enter the STT input by
  default (it's on write side).
- This is correct for "STT only on caller" — the dominant use case.
- For "STT on both sides" (e.g., for full transcripts of the
  conversation), attach an additional bug at LATE stage rank with
  `SMBF_WRITE_STREAM` flag and `Purpose=RECORDING_RELAY` (or stand
  up a second STT stream over the relay's write-tap output). Per
  FF-001 the write-side bug processing is a single interleaved
  loop, so this LATE write-tap must be positioned AFTER the INJECT
  bug in the chain to observe post-injection (bot) audio — which
  the MediaBugManager's stage-rank coordinator enforces.

## Barge-in (interaction with VAD)

If a VAD bug at EARLY stage rank detects caller speech during bot
playback, the VAD bug emits a `BargeIn` Control message on its
gRPC stream. The orchestrator listens for `osw::vad::barge_in`
events and decides whether to cancel the TTS turn (typically via
`StopMediaStream` on the TTS bug). The VAD bug uses `SMBF_FIRST`
(per the stage-rank table) so it always sits at the head of the
chain regardless of attach order.

The STT bug is independent of barge-in — it just keeps tapping
read-side audio.

## Failure modes

| Failure | Behavior |
|---|---|
| STT service unreachable | StartStt RPC returns UNAVAILABLE; orchestrator may retry. |
| STT stream dies mid-call | Bug emits `bug_stream_lost`; channel continues. Orchestrator can re-issue StartStt to attach a new stream. |
| Resample rate unsupported | StartStt returns INVALID_ARGUMENT. |
| Sample-rate mismatch from STT service | Service-side error; transcript event NOT emitted; module logs. |
| Final transcript never arrives | Per-stream timeout (default 60s of no message after audio sent) → close stream, emit Tier-2 `stt_timeout`. |
| Send ring overflow | Drop oldest audio frames; STT will see a gap. Emit Tier-2 `stt_send_overflow` (rate-limited). |

## Test plan

| Test | What it proves |
|---|---|
| `stt_basic_transcript` | Mock STT server returns a known transcript. Module emits event with matching text. |
| `stt_interim_then_final` | Mock returns interim, interim, final. Three events emitted; `stt_final` flag correct. |
| `stt_coexist_with_tts` | STT + TTS on same channel; verify no audio cross-contamination (STT doesn't see bot output, TTS doesn't see caller input). |
| `stt_reconnect_mid_call` | Restart mock STT; module emits stream_lost; new StartStt re-attaches. |
| `stt_sample_rate_negotiation` | StartStt with rate=22050; module rejects with INVALID_ARGUMENT. |
| `stt_high_volume_caller` | Loud audio (max amplitude) doesn't clip resampler output. |
| `stt_silent_caller` | All-silence audio stream → STT returns no events; no errors. |
| `stt_channel_variable_set` | After final transcript, channel variable `stt_latest_transcript` matches. |
| `stt_event_tier_default` | Verify the emitted `osw::stt::transcript` envelope carries `tier=TIER_2_STATE` and is delivered to subscribers via gRPC `SubscribeEvents` (post-F0 there is no in-module sink). |

## Future work (V1.5+)

- Diarisation flag (`diarise=true` in StartStt; service returns
  `speaker_id` per transcript).
- Word-level timing (`words[]` in Transcript with start/end ms per
  word).
- Vocabulary hot-update mid-call (today: requires StopStt + StartStt).
- Server-streaming "punctuation only" pass for low-bandwidth interim
  flows.
- STT result fan-out to multiple endpoints (e.g., primary + audit
  recorder).
