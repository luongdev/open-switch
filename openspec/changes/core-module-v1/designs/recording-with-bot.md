# Recording with bot in the call

## The question

When a FreeSWITCH call has a bot participant (TTS injecting audio via
our media bug with `SMBF_WRITE_REPLACE`), does call recording
**capture the bot's audio**? And what if the operator wants caller
and bot on **separate channels** for easier QA / analytics?

This document answers both. The answer hinges on a FreeSWITCH media
pipeline detail that round 1 of the Codex review got wrong (C-2) and
that round 2 also got wrong (the round-2 closeout claimed
WRITE_STREAM ran in a separate pass AFTER all WRITE_REPLACE bugs —
also false at v1.10.12). Per **FF-001** (`src/switch_core_media.c:16096-16156`),
the write side is a **single interleaved loop**: for each bug in
chain order, FS first checks `SMBF_WRITE_STREAM` (which buffers and
calls back with the current `write_frame`) and then checks
`SMBF_WRITE_REPLACE` (which may mutate `write_frame` for the NEXT
iteration). There is no second pass. **Chain order is what matters.**

## Quick answer

**Recording captures bot audio if and only if the recording bug is
positioned AFTER all `WRITE_REPLACE` bugs in the chain at the moment
audio is processed.** For the dominant operator pattern
(`record_session` is FS-native and our TTS bug is added via
`StartTts` / `StartVoicebot` / equivalent), this means:

- Operator dialplan calls `start_bot` (which attaches our TTS
  `WRITE_REPLACE` bug) **then** `record_session`: chain is
  `[TTS, record_session]`. The TTS bug's `WRITE_REPLACE` callback
  mutates `write_frame`; the record_session's `WRITE_STREAM`
  callback then fires with the post-injection frame. **Recording
  contains bot audio.**
- Operator dialplan calls `record_session` **then** `start_bot`:
  chain is `[record_session, TTS]`. The record_session's
  `WRITE_STREAM` callback fires first with the pre-injection (i.e.,
  unmodified) frame; the TTS bug's mutation happens later in the
  same loop, but that mutation only affects subsequent iterations —
  there are no bugs after TTS to observe it. **Recording does NOT
  contain bot audio.**

This is **operator-dialplan-order-dependent**. The previous draft
of this section presented "regardless of dialplan order" as the
default; that was wrong. We document the dependency loudly and
suggest mitigations below.

For module-owned recording (`StartRecordingRelay` → our
`RECORDING_RELAY` purpose, stage rank `LATE`), the MediaBugManager
refuses to attach the relay bug if an INJECT-stage bug
(`TTS_PLAYBACK`, `VOICEBOT_DUPLEX` write half) is not already in
the chain or would be added later. Operators using the relay are
forced into the correct ordering by the manager (`FAILED_PRECONDITION`
returned on out-of-order attach attempts).

For caller+bot **stereo** (L=caller, R=bot), the `RECORDING_RELAY`
purpose produces a synchronised stereo PCM stream. The R channel is
the post-injection write frame — i.e., "bot + IVR prompt + hold
music + any other WRITE_REPLACE contribution mixed together". For a
bot-only call R is effectively bot; operators should know that R
reflects what the caller hears, not bot in isolation.

For caller+bot **mono mixed** via FS-native `record_session`,
operator dialplan ordering rules apply (see above). The module
emits an optional Tier-1 warning event when a TTS `WRITE_REPLACE`
attaches to a channel that already has a `record_session` bug
present — see `warn_record_before_inject` in the operator-knob
table below.

## How write-side bug processing actually works at v1.10.12

Per **FF-001** (`src/switch_core_media.c:16096-16156` in
`switch_core_session_write_frame`), the FS write pipeline is **one
interleaved loop**, not two passes:

```text
For each ptime tick on write side:
  base_frame = produce_from(playback_file | hold_music | silence | bridged_leg)

  single loop over bugs in chain order:
    for each bug bp:
      # WRITE_STREAM check fires FIRST in the iteration ...
      if bp.flags has SMBF_WRITE_STREAM:
        # ... and observes the CURRENT write_frame at this moment.
        # If no earlier-in-chain WRITE_REPLACE has fired yet, this is
        # the pre-injection frame.
        switch_buffer_write(bp.raw_write_buffer, write_frame.data,
                            write_frame.datalen)
        bp.callback(SWITCH_ABC_TYPE_WRITE)

      # WRITE_REPLACE check fires SECOND in the same iteration.
      if bp.flags has SMBF_WRITE_REPLACE:
        bp.callback(SWITCH_ABC_TYPE_WRITE_REPLACE)
        # If callback returns TRUE, write_frame is mutated.
        # The mutation is visible to bugs LATER in the loop only.

  network.send(write_frame)
```

There is **no** "all WRITE_REPLACE bugs run, then all WRITE_STREAM
bugs run" second pass. (Read side IS two passes — see FF-006 — but
write side is the single interleaved loop above. Round 2 of the
Codex review incorrectly claimed both sides ran in two passes; this
section corrects that.)

Consequences for our case (TTS = `WRITE_REPLACE`, recording =
`WRITE_STREAM`):

| Chain order | What recording's WRITE_STREAM callback sees |
|---|---|
| `[recording, TTS]` (record attached first) | Pre-injection frame. Recording does NOT contain bot audio. |
| `[TTS, recording]` (TTS attached first) | Post-injection frame. Recording contains bot audio. |

The outcome is **add-order-dependent**. To make recording reliably
capture bot audio, the recording bug MUST be at the tail of the
chain at the moment audio flows (or at least positioned after every
`WRITE_REPLACE` bug). Two enforcement paths:

1. **Module-owned recording** — `StartRecordingRelay` attaches our
   `RECORDING_RELAY` bug at stage rank `LATE`. The MediaBugManager
   rejects out-of-order attaches with `FAILED_PRECONDITION`. The
   manager cannot reorder FS-native bugs (it only controls bugs we
   own) — but for `RECORDING_RELAY` specifically the manager is
   sufficient to guarantee correctness.
2. **FS-native `record_session`** — there is no FS-side mechanism
   that "moves" an already-attached bug to the tail. The only
   levers are operator dialplan order (call `start_bot` BEFORE
   `record_session`) and `SMBF_FIRST` (puts the recording bug at
   the head, which is the WRONG direction for our case). The
   operator-side ordering is the only solution.

Read-side is symmetric in the high-level sense (per FF-006, the
read side IS two passes, so READ_STREAM taps see post-READ_REPLACE
audio regardless of chain position), but the module does not use
`SMBF_READ_REPLACE` in V1, so for V1 the practical effect is that
all READ_STREAM taps (STT, AMD, recording read-tap) see the same
raw caller frame.

The `media-bridge.md` document covers the broader add-order story
(VAD-at-head via `SMBF_FIRST`, MediaBugManager stage ranks). For
recording specifically, the dialplan ordering above is what
matters for FS-native `record_session`.

## Mono mixed recording (default FS behaviour)

```text
Caller mic ──▶ FS read pipeline
              │
              ├──▶ STT bug          (SMBF_READ_STREAM tap)
              ├──▶ AMD bug          (SMBF_READ_STREAM tap)
              ├──▶ record_session   (SMBF_READ_STREAM tap, FS native)
              ▼
              FS hands frame upstream

Bot TTS ──▶ FS write pipeline (single interleaved loop per FF-001)

  if chain is [TTS, record_session]   (start_bot BEFORE record_session)
    iter 1 (TTS):           WRITE_REPLACE mutates write_frame to bot audio
    iter 2 (record_session): WRITE_STREAM tap fires with POST-injection frame
    ──▶ recording contains bot audio  ✓

  if chain is [record_session, TTS]   (record_session BEFORE start_bot)
    iter 1 (record_session): WRITE_STREAM tap fires with PRE-injection frame
    iter 2 (TTS):           WRITE_REPLACE mutates write_frame, but no later
                            bug observes the mutation
    ──▶ recording does NOT contain bot audio  ✗

  ▼ FS encodes + sends to network
```

Output: a single `.wav` (or `.mp3` via mod_shout) file with caller
and bot mixed together **iff the operator dialplan ordering is
correct**.

**Operator config (correct — start bot BEFORE recording)**:

```xml
<extension name="record-with-bot">
  <condition field="destination_number" expression="^.*$">
    <action application="answer"/>
    <action application="set" data="RECORD_STEREO=false"/>
    <!-- start_bot.lua attaches the TTS WRITE_REPLACE bug.
         This MUST be in the chain before record_session. -->
    <action application="lua" data="start_bot.lua"/>
    <!-- record_session attaches at the tail. Its WRITE_STREAM
         tap fires AFTER TTS's WRITE_REPLACE in each write-side
         iteration, so the recording captures bot audio. -->
    <action application="record_session"
            data="/opt/freeswitch/recordings/${uuid}.wav"/>
    <action application="park"/>
  </condition>
</extension>
```

**Operator config (incorrect — recording silent on bot segments)**:

```xml
<extension name="record-then-bot-INCORRECT">
  <condition field="destination_number" expression="^.*$">
    <action application="answer"/>
    <action application="set" data="RECORD_STEREO=false"/>
    <!-- record_session attaches first; its WRITE_STREAM tap sits
         BEFORE the TTS bug in the chain and observes pre-injection
         frames only. -->
    <action application="record_session"
            data="/opt/freeswitch/recordings/${uuid}.wav"/>
    <action application="lua" data="start_bot.lua"/>
    <action application="park"/>
  </condition>
</extension>
```

The "incorrect" ordering is unfortunately a common compliance
reflex ("record from answer, no exceptions"). Operators using
compliance recording with bot calls must either:

1. Use the correct ordering above (start bot first, then record).
2. Use `StartRecordingRelay` instead of `record_session` so the
   MediaBugManager enforces stage-rank ordering.
3. Accept that mono mixed recording captures only the caller's
   side when ordering is wrong, and source bot audio another way
   (e.g., bridged bot leg + native stereo).

The module emits a Tier-1 warning event when it detects the
incorrect pattern — see `warn_record_before_inject` in the
operator-knob table below. There is no FS-side mechanism that
auto-fixes ordering after the fact; the warning is the only
recovery signal.

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
attaches TWO bugs at LATE stage rank — see `media-bridge.md`. The
MediaBugManager refuses to attach these bugs unless every INJECT-rank
bug (TTS, voicebot-duplex write half) is already present in the chain,
so the resulting chain order places the relay AFTER all
`WRITE_REPLACE` bugs (per FF-001 this is what's needed for the
write-tap to observe post-injection audio).

- Read bug: `SMBF_READ_STREAM` — taps caller's mic (raw, since no
  `READ_REPLACE` is in use).
- Write bug: `SMBF_WRITE_STREAM` — taps write side AFTER the TTS
  WRITE_REPLACE in the chain, so it observes the post-injection
  frame in the single write-side loop (FF-001).

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

## Tension: "record from answer" vs. "bot in recording"

Two operator practices pull in opposite directions:

- **Compliance reflex** (PCI-DSS, HIPAA, contact-center QA): start
  recording immediately after `answer`, before anything else. This
  attaches `record_session` first, putting its `WRITE_STREAM` tap
  at the head of the bug chain.
- **Bot-in-recording requirement**: per FF-001 the
  `WRITE_STREAM` tap only observes post-injection audio if it sits
  AFTER all `WRITE_REPLACE` bugs in the chain — i.e., recording
  must attach AFTER the bot.

The two cannot be reconciled with FS-native `record_session`
alone. There is no FS-side mechanism to (a) reorder an
already-attached bug, or (b) make the bug observe post-replace
audio "regardless of position". The choices are:

| Strategy | Captures bot audio? | First-ms coverage? | Notes |
|---|---|---|---|
| `record_session` first, then `start_bot` (compliance-default) | NO — pre-injection only | YES (covers caller) | Bot segments are silent in the recording. Module emits Tier-1 warning event when TTS attaches behind record_session. |
| `start_bot` first, then `record_session` | YES | NO — bot's first frames before record_session attaches are missed | The dominant correct pattern for bot-in-recording when using `record_session`. |
| `StartRecordingRelay` (module-managed) before any INJECT bug | n/a — manager rejects with `FAILED_PRECONDITION` | n/a | The MediaBugManager forces the correct order. |
| `StartRecordingRelay` after `start_bot` | YES | NO — same pre-attach gap as above | Manager-enforced ordering AND module-owned relay (operator-friendly). Recommended. |
| Bridge bot as a SIP leg + native `RECORD_STEREO` (Option A above) | YES | YES | Adds operational complexity (bot-as-SIP-leg). Worth it only if bot is already deployed this way. |

For perfect first-ms coverage on bot-in-recording, the only viable
V1 path is **bridge the bot as a SIP leg + use native FS stereo
recording** (Option A in "Stereo split recording" above). Module-
managed `RECORDING_RELAY` plus bot-first ordering gives bot-in-
recording with a small attach-gap on the very first frames.

The module logs a warning if `StartTts` (or any module-attached
`WRITE_REPLACE`) is invoked on a channel that does NOT yet have a
recording bug attached AND the operator's config sets
`<param name="warn_no_recording" value="true"/>` (default false;
not all deployments want recording on every call).

The reverse warning — `WRITE_REPLACE` attaching to a channel that
already has `record_session` ahead of it — is `warn_record_before_inject`
(default true; see the operator-knobs table below) and emits a
Tier-1 audit event so operators can detect the silent-bot mistake
without listening to recordings.

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
| `record_write_stream_observes_post_replace` | Attach TTS WRITE_REPLACE first, then record_session (FS-native) second → chain `[TTS, record]`. Verify recorded frames contain TTS-injected audio. |
| `record_before_inject_misses_bot` | Attach record_session first, then TTS WRITE_REPLACE → chain `[record, TTS]`. Verify recorded frames are PRE-injection (no bot audio). Documents the operator-side ordering requirement. Emits Tier-1 `warn_record_before_inject` event when TTS attaches. |
| `recording_relay_rejects_before_inject` | Call `StartRecordingRelay` on a channel with no INJECT-stage bug → MediaBugManager returns `FAILED_PRECONDITION`. Documents the manager-side enforcement for module-owned recording. |
| `record_lr_desync_warning` | Inject artificial 30 ms delay on one side; verify warning + metric. |
| `record_overflow_drops_oldest` | Slow relay; ring fills; oldest dropped; counter increments. |

## Operator knobs

| Knob | Default | Purpose |
|---|---|---|
| Recording purpose default sample rate | 8000 Hz | Telco standard; raise to 16k if bot audio is wider |
| Stereo desync warning threshold | 5 ms | Pairing tolerance |
| Stereo desync fill timeout | 25 ms | When to give up waiting for the other side |
| Recording-relay send ring | 500 ms | Tolerance for slow recording service |
| `warn_record_before_inject` | `true` | When the module's TTS / voicebot-write bug attaches to a channel that already has a bug with `bp->function == "record_session"` (detected via `switch_core_media_bug_count` per FF-008), emit a Tier-1 audit event `osw::recording::record_before_inject_warning` containing channel_uuid, tenant_id, and a remediation pointer. This catches the common compliance-first ordering mistake at attach time so operators learn about it before the recording is silent. Default true because the symptom (silent bot in recording) is easy to mistake for an upstream bot failure. |

## Future work

- HTTP-PUT chunked uploads (instead of gRPC stream) as an alternative
  recording sink for operators who prefer object storage directly.
- Conference recording (when bot is part of a multi-party conference)
  — V1 doesn't address this; the stage-rank model and pairer
  abstraction extend to it but it's not on the V1 path.
