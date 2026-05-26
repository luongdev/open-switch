# Recording with bot in the call

## The question

When a FreeSWITCH call has a bot participant (TTS injecting audio via
our media bug with `SMBF_WRITE_REPLACE`), does call recording
**capture the bot's audio**? And what if the operator wants caller
and bot on **separate channels** for easier QA / analytics?

This document answers both. The answer hinges on a FreeSWITCH media
pipeline detail that the prior draft of this spec got wrong (see Phase
1 Codex finding C-2): bug **chain position** does NOT determine
whether recording captures bot audio. What determines it is the
distinction between `SMBF_WRITE_REPLACE` (transform) and
`SMBF_WRITE_STREAM` (post-transform tap).

## Quick answer

**Yes, recording captures bot audio** — but for a different reason
than "bug priority puts recording after TTS" (which was the prior
incorrect explanation). The actual reason:

`SMBF_WRITE_STREAM` callbacks are invoked by FreeSWITCH **after** all
`SMBF_WRITE_REPLACE` bugs on the channel have had a chance to replace
the frame. This is a fixed semantic of the FS media pipeline, not a
property of chain order. So a recording bug using `WRITE_STREAM` will
always observe the post-injection write frame, whether it was attached
before or after the TTS bug.

`switch_ivr_record_session_event` (FS native, what
`record_session` dialplan app calls) attaches with flags
`SMBF_READ_STREAM | SMBF_WRITE_STREAM | SMBF_READ_PING`. The
`WRITE_STREAM` half is what makes it pick up bot audio. **Add order
does not matter for this case.**

For caller+bot **stereo** (L=caller, R=bot), we provide a built-in
`RECORDING_RELAY` purpose in the media bridge that produces a
synchronised stereo PCM stream. The R channel is the post-injection
write frame, which is "bot + IVR audio + any other WRITE_REPLACE
contribution mixed together" — typically just bot for a bot-only
call, but operators should know that the R channel reflects the
caller's earpiece, not bot-isolated audio.

For caller+bot **mono mixed** (default FS behaviour), no extra config
needed — operators use `record_session` as they always have. The
FS semantic above guarantees bot audio is in the recording regardless
of dialplan order between `record_session` and `start_bot`.

## How `WRITE_STREAM` post-injection observation works

FreeSWITCH's media pipeline for write side is (simplified):

```text
For each ptime tick on write side:
  base_frame = produce_from(playback_file | hold_music | silence)

  for each bug in chain order:
    if bug.flags has SMBF_WRITE_REPLACE:
      bug.callback(WRITE_REPLACE)
        → may call switch_core_media_bug_set_write_replace_frame
        → if set, replaces the current frame for next iteration

  # at this point all WRITE_REPLACE bugs have run; the frame is "final"

  for each bug in chain order:
    if bug.flags has SMBF_WRITE_STREAM:
      bug.callback(WRITE)
        → receives the final frame (read-only)

  network.send(final_frame)
```

The two passes are independent. A `WRITE_STREAM` callback never
runs interleaved with `WRITE_REPLACE` callbacks. So:

- TTS bug (`WRITE_REPLACE`) replaces the frame with bot audio.
- Recording bug (`WRITE_STREAM`) observes the replaced frame.

The order in which these two bugs were attached changes only the
order their callbacks fire within their respective pass. The
post-injection observation property holds either way.

Read-side has the symmetric structure: `READ_REPLACE` runs before
`READ_STREAM`. The module does not use `READ_REPLACE` in V1, so
`READ_STREAM` taps see raw caller audio.

The `media-bridge.md` document covers the broader add-order story
(VAD-at-head via `SMBF_FIRST`, etc.). For recording specifically,
the FS semantic above is what matters.

## Mono mixed recording (default FS behaviour)

```text
Caller mic ──▶ FS read pipeline
              │
              ├──▶ STT bug          (SMBF_READ_STREAM tap)
              ├──▶ AMD bug          (SMBF_READ_STREAM tap)
              ├──▶ record_session   (SMBF_READ_STREAM tap, FS native)
              ▼
              FS hands frame upstream

Bot TTS ──▶ TTS bug (SMBF_WRITE_REPLACE) ──▶ frame replaced
                                              │
                                              ▼ WRITE_STREAM pass:
                                              ├──▶ record_session   (SMBF_WRITE_STREAM, sees BOT frame)
                                              ▼
                                              FS encodes + sends to network
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
`WRITE_STREAM`-vs-`WRITE_REPLACE` semantic ensures bot audio is in
the recording **regardless of dialplan order** (record_session first
then start_bot, or vice versa).

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
attaches TWO bugs (LATE stage rank — see `media-bridge.md`):

- Read bug: `SMBF_READ_STREAM` — taps caller's mic (raw, since no
  `READ_REPLACE` is in use).
- Write bug: `SMBF_WRITE_STREAM` — taps write side after TTS injection
  per the FS semantic described above.

Both feed into a single gRPC stream with `StreamStart.side=STEREO`.
The module:

- Receives caller frame on read-tap callback → buffer as L channel.
- Receives post-injection write frame on write-tap callback → buffer
  as R channel. **What's actually in R**: bot's TTS audio PLUS any
  other write-side audio (hold music if active, mod_dptools
  playback if running). For a bot-only conversation, R ≈ bot. For an
  IVR menu that plays a prompt then yields to bot, R is whichever
  source produced the frame at that instant.
- When both L and R frames arrive (they fire on the same FS media
  thread for the same ptime window; see "Sync of L and R" below),
  interleave samples into a single AudioFrame with
  channel=BOTH_INTERLEAVED:
  `[L0][R0][L1][R1]...[L159][R159]` for 20 ms at 8 kHz.
- Send over gRPC.

The external recording service writes to a stereo WAV file or S3 or
whatever durable store, with caller on L and bot-side mix on R.

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
arrives on write-tap callback. **Both callbacks fire on the same FS
media thread**, single-threaded per channel (Phase 1 Codex finding
I-15). They alternate within each ptime tick: the read callback fires
when FS processes the read frame; the write callback fires when FS
processes the write frame; for a given ptime window these are
sequential on one thread, not concurrent across threads.

So the L/R pairer does NOT need cross-thread synchronisation. The
small ring per side is for ordering within the same thread (in case
FS emits multiple frames before our pairer consumes them — rare but
possible during bursts).

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
attaches as a LATE-stage bug; once it's attached, every subsequent
write frame is captured by its `WRITE_STREAM` tap.

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
| `record_write_stream_observes_post_replace` | Attach TTS WRITE_REPLACE first, then record WRITE_STREAM. Verify record frames contain TTS-injected audio. |
| `record_attach_order_independent` | Reverse: attach record first, then TTS. Verify record STILL contains TTS audio (FS WRITE_STREAM-after-WRITE_REPLACE semantic). |
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
  — V1 doesn't address this; the stage-rank model and pairer
  abstraction extend to it but it's not on the V1 path.
