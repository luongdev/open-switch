# Recording with bot in the call

## The question

When a FreeSWITCH call has a bot participant (TTS injecting audio via
our media bug at priority 500, WRITE_REPLACE), does call recording
**capture the bot's audio**? And what if the operator wants caller and
bot on **separate channels** for easier QA / analytics?

This document answers both.

## Quick answer

Yes, recording captures bot audio if the FS recording bug runs **after**
our TTS write-replace bug in the chain. We arrange priorities to make
this the default.

For caller+bot **stereo** (L=caller, R=bot), we provide a built-in
`RECORDING_RELAY` purpose in the media bridge that produces a
synchronized stereo PCM stream. Operators can write this to a file
via a recording service, or to a long-term store via the gRPC stream.

For caller+bot **mono mixed** (default FS behavior), no extra config
needed — operators use `record_session` as they always have, and our
priority defaults ensure bot audio is included.

## Bug priority ordering for recording

Recall the priority table from [`media-bridge.md`](media-bridge.md):

| Priority | Purpose | Flags |
|---|---|---|
| 100 | VAD / barge-in | `READ_STREAM` |
| 200 | STT | `READ_STREAM` |
| 300 | AMD | `READ_STREAM` |
| 500 | TTS playback | `WRITE_REPLACE` |
| 700 | Recording — read tap | `READ_STREAM` |
| 750 | Recording — write tap | `WRITE_STREAM` |
| 900 | Test/instrumentation | — |

`record_session` (FS native) attaches at FS's default priority — which
ends up near 700-750. So recording's read tap sees the same audio STT
sees (caller's mic), and recording's write tap sees the channel write
side **after** our TTS injection (priority 500 runs first). Result:
recording captures the bot's voice.

If operators add custom bugs at unusual priorities, they need to be
aware of this ordering. The module's `mediabug` debug command lists
all bugs and their priorities for verification.

## Mono mixed recording (default FS behavior)

```text
Caller mic ──▶ FS read pipeline ──▶ STT bug @ 200 (tap, no modify)
                                  │
                                  ▼
                                  recording read tap @ ~700 (FS native)
                                          │
                                          ▼
                                          mixed into recording file's L+R or mono
                                          ────────────────────────────────────────
Bot TTS ──▶ TTS bug @ 500 (WRITE_REPLACE) ──▶ FS write pipeline
                                                │
                                                ▼
                                                recording write tap @ ~750 (FS native)
                                                          │
                                                          ▼
                                                          mixed into recording file
```

Output: a single `.wav` (or `.mp3` via mod_shout) file with caller
and bot mixed together.

**Operator config**:

```xml
<extension name="record-with-bot">
  <condition field="destination_number" expression="^.*$">
    <action application="answer"/>
    <action application="set"
            data="RECORD_STEREO=false"/>
    <action application="record_session"
            data="/opt/freeswitch/recordings/${uuid}.wav"/>
    <action application="lua" data="start_bot.lua"/>
    <action application="park"/>
  </condition>
</extension>
```

This is **standard FreeSWITCH**. The module doesn't add anything; the
existing priority ordering ensures bot audio is in the recording.

## Stereo split recording (recommended for AI ops)

For AI quality analysis, you want L=caller / R=bot so the analyst
can mute one side and listen to the other. FreeSWITCH has native
support via `RECORD_STEREO=true` channel variable; the resulting
file has caller on left, bridged party on right.

**With a bot**: the bot is not a bridged party (it's a media bug
write-replace, not a SIP leg). FS native stereo won't put bot on R
because there's no B-leg to allocate to it.

**Two approaches**:

### Option A — Native FS stereo with bot on B-leg

Force the bot to be a "real" leg:

1. Originate a bot leg that just executes `socket` or `audio_stream`
   pointing at our gRPC server (i.e., make the bot a SIP-like B-leg
   in FS's eyes).
2. Bridge the caller to this bot leg.
3. `RECORD_STEREO=true` + `record_session` writes caller=L, bot=R.

**Downside**: the bot leg is a fake SIP channel and the SIP signaling
overhead is wasted. Adds complexity to the dialplan.

**Not recommended** unless you're already running a bot-as-SIP-leg
setup for other reasons.

### Option B (recommended) — Module-provided stereo relay

Use the `RECORDING_RELAY` media purpose with stereo split. The module
attaches TWO bugs:

- Bug at priority 700: `SMBF_READ_STREAM` — taps caller's mic.
- Bug at priority 750: `SMBF_WRITE_STREAM` — taps write side AFTER
  TTS injection.

Both feed into a single gRPC stream with `StreamStart.side=STEREO`.
The module:

- Receives caller frame on read-tap callback → buffer as L channel.
- Receives mixed-with-bot frame on write-tap callback → buffer as R.
- When both L and R frames arrive (or a watchdog fires after 25 ms),
  interleave samples into a single AudioFrame with channel=BOTH_INTERLEAVED:
  `[L0][R0][L1][R1]...[L159][R159]` for 20 ms at 8 kHz.
- Send over gRPC.

The external recording service writes to a stereo WAV file or to S3
or to whatever durable store, with caller on L and bot on R.

This is a **module-side feature**: control RPC `StartRecordingRelay`:

```protobuf
rpc StartRecordingRelay(StartRecordingRelayRequest)
    returns (StartRecordingRelayResponse);
```

Parameters:

- `channel_uuid` — required
- `relay_endpoint` — gRPC URL of the external recording service
- `stereo` — bool; default true
- `sample_rate_hz` — default 8000 (telco standard for recordings)
- `tenant_id`

## Sync of L and R for stereo relay

Caller's mic frame arrives on read-tap callback. Write-side frame
arrives on write-tap callback. These two callbacks fire on the same
FS media thread, but in alternating order; the write frame for time T
arrives shortly after the read frame for time T (typically same ptime
boundary).

We pair them by timestamp:

1. Read tap arrives with FS-internal timestamp `t_read`.
2. Write tap arrives with timestamp `t_write` (typically equal to
   `t_read` for the same ptime window).
3. The module pairs them. If `|t_read - t_write| > 5 ms`, log a
   warning and emit anyway (one channel will have a small offset).
4. If one side fails to arrive within 25 ms, fill that channel with
   silence and emit the frame.

This is implemented in a per-channel `StereoFramePairer` class with a
small 4-frame ring per side.

## File-based recording sink

For operators who just want a local WAV/MP3 file, the recording
service can be a tiny gRPC server that writes incoming AudioFrames to
disk. We provide a reference implementation in `tests/integration/`
under the name `osw_record_sink`. Production operators typically
write their own to S3 / GCS / Azure Blob.

The module itself does NOT write files. We're a bus, not a sink. This
keeps the module focused and lets operators bring their own storage
policy.

## What about RECORD_START / RECORD_STOP events?

FS's `record_session` emits these as Tier 1 events (per our default
routing). Our `RECORDING_RELAY` purpose ALSO emits a CUSTOM Tier 1
event when started/stopped:

- `osw::recording::relay_started` — when `StartRecordingRelay` succeeds
- `osw::recording::relay_stopped` — on channel hangup or stop RPC

Both are Tier 1 for compliance / audit traceability.

## What if the bot speaks BEFORE the recording starts?

Edge case: the orchestrator answers the call, the bot starts speaking,
THEN starts recording. The bot's first words are missed.

Best practice (operator side): start recording immediately after
`answer`, BEFORE any `start_bot` / `start_tts`. The recording bug
attaches at priority 700; once it's attached, every subsequent write
frame is captured.

If the operator needs perfect coverage from the first millisecond,
they should:

1. `answer`
2. `record_session ...` (FS native) OR `StartRecordingRelay`
3. `set bot_started=true`
4. `lua start_bot.lua`

The module logs a warning if `StartTtsStream` is called on a channel
that doesn't have a recording bug attached AND the operator's config
sets `<param name="warn_no_recording" value="true"/>` (default false;
not all deployments want recording on every call).

## Failure modes

| Failure | Behavior |
|---|---|
| Recording service unreachable | StartRecordingRelay returns UNAVAILABLE. Channel continues without recording. |
| Recording service slow | Send ring fills → drop oldest frames; emit Tier-2 `recording_send_overflow`. Operator can size the ring up for bursty services. |
| L/R desync > 25 ms | One side filled with silence; warning logged; emit metric `recording_lr_desync_count`. |
| Read tap removed but write tap stays | Pairer fills L with silence; recording continues with bot-only on R. |
| Recording service emits no AudioFrame (it's a sink) | Module ignores incoming AudioFrame; only Control messages matter. |
| Channel hangs up | DetachAll fires; both bugs released; stream half-closed; recording service finalises file. |

## Test plan

| Test | What it proves |
|---|---|
| `record_with_bot_audio_present` | Record a call with TTS injection; the recording file contains both caller audio AND bot audio (verify by amplitude on both segments). |
| `record_stereo_split` | StartRecordingRelay stereo=true; relay receives L=caller, R=bot; correct interleaving. |
| `record_after_bot_start_misses_pre` | Start bot, then start recording; first N ms of bot audio missed. Documented behavior. |
| `record_stop_finalises` | StopRecordingRelay (or hangup); relay receives end-of-stream; final frame numbered correctly. |
| `record_priority_order` | Verify bugs attached in any order end up in correct priority order (priority allocation is independent of add order). |
| `record_lr_desync_warning` | Inject artificial 30 ms delay on one side; verify warning + metric. |
| `record_overflow_drops_oldest` | Slow relay; ring fills; oldest dropped; counter increments. |

## Operator knobs

| Knob | Default | Purpose |
|---|---|---|
| Recording purpose default sample rate | 8000 Hz | Telco standard; raise to 16k if bot audio is wider |
| Stereo desync warning threshold | 5 ms | Pairing tolerance |
| Stereo desync fill timeout | 25 ms | When to give up waiting for the other side |
| Recording-relay send ring | 500 ms | Tolerance for slow recording service |

## Future work

- HTTP-PUT chunked uploads (instead of gRPC stream) as an alternative
  recording sink for operators who prefer object storage directly.
- Conference recording (when bot is part of a multi-party conference)
  — V1 doesn't address this; the priority model and pairer
  abstraction extend to it but it's not on the V1 path.
