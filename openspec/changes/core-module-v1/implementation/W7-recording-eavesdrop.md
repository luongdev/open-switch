# W7 — Recording relay + eavesdrop policy (wave plan)

Wave 7 closes the three V1 capabilities that W6 explicitly deferred:

- **Recording with bot audio in the file** — module-owned
  `RECORDING_RELAY` purpose with mono + stereo split, plus the
  attach-order rule that recording must follow every INJECT bug in the
  bug chain (FF-001 — write side is one interleaved loop).
- **Eavesdrop policy on bot calls** — supervisor eavesdrop on a TTS /
  voicebot call is gated by tenant policy (`deny` / `audit` / `allow`).
  Layer 1 is a custom `osw_eavesdrop` dialplan app (pre-attach
  enforcement); Layer 2 is a `MEDIA_BUG_START` detector that emits a
  Tier-1 audit when raw `eavesdrop` lands on a bot-marked session.
- **Multi-target bot attachment (`StartBot` / `StopBot`)** — replaces
  the W6 legacy per-channel single-target RPCs (`StartTts`,
  `StartStt`, `StartVoicebot`) with one bot logical entity per call
  that attaches bugs on N target channels (V1 max 2). Multiplexes
  one upstream gRPC stream + fans-out audio into per-target SPSC
  queues. See [`W7-track-D-bot-multitarget.md`](W7-track-D-bot-multitarget.md).

Wave-level review will use Codex CLI then fall back to Gemini CLI if
Codex's output is unusable (W6 pattern). No Claude sub-agents for
review (project memory).

## Scope at a glance

| Component | Track | Approx LOC |
|---|---|---|
| `osw_eavesdrop` dialplan app (Layer 1) | A | ~250 |
| `MEDIA_BUG_START` detector (Layer 2) | A | ~150 |
| Channel-var marker on `StartTts` / `StartVoicebot` (`osw_bot_session`, `osw_bot_purpose`, `osw_eavesdrop_policy`) | A | ~50 |
| 4 eavesdrop audit subclasses + tier-1 routing | A | ~80 |
| Per-tenant `allow_eavesdrop` + module-default `eavesdrop_policy` config | A | ~60 |
| FACT entry for `SWITCH_ADD_APP` + `switch_application_interface_t` lifecycle (FF-035) | A | new FF |
| `RECORDING_RELAY` purpose attach (mono + stereo) | B | ~400 |
| `StereoFramePairer` (timestamp pairing + desync warn/timeout) | B | ~200 |
| `StartRecordingRelay` + `StopRecordingRelay` RPC + handlers | B | ~250 |
| `MediaBugManager` LATE-stage refinement: refuse `kRecordingRelay` if no INJECT bug is present | B | ~80 |
| `warn_record_before_inject` Tier-1 event when TTS attaches to a channel that already has FS-native `record_session` | B | ~60 |
| `osw::recording::relay_started` / `.relay_stopped` Tier-1 audit + `recording_send_overflow` / `recording_lr_desync_count` Tier-2 events | B | ~80 |
| Recording config (send-ring ms, desync thresholds, default sample rate) | B | ~50 |
| `StartBot` / `StopBot` RPCs (multi-target) | D | ~250 |
| `osw::media::BotSession` + `BugFanout` + `ActiveBots` registry | D | ~600 |
| Per-target bug callbacks (`OswBotReadTap`, `OswBotWriteReplace`) | D | ~150 |
| 4 bot audit subclasses + Tier-1/2 routing | D | ~60 |
| 4 module-config fields (bot_max_targets, bot_target_queue_ms, bot_drain_timeout_ms, max_bots_per_channel) | D | ~80 |

Plan-doc LOC budget per track: ~600. Implementation LOC budget total
~2900 across three tracks (test code excluded).

## Dependencies

| Item | Depends on |
|---|---|
| Track A | W6 Track C — `StartTts` / `StartVoicebot` handlers must exist; Track A *patches* them to set channel variables. |
| Track A | FF-035 (new) for the dialplan-app registration API + `SWITCH_STANDARD_APP` macro. The Track A sub-agent writes this FF entry alongside its implementation. |
| Track B | W6 Track A — `MediaBugManager` + stage-rank gate (`Attach` already enforces `this_rank >= max_existing_rank`). Track B *extends* the gate with a "kRecordingRelay requires an INJECT bug already present" check. |
| Track B | W6 Track B — `StreamClient` + resampler. Track B uses the existing bidi gRPC client; the recording sink is the gRPC peer. |
| Track B | W6 Track C — `ActiveMediaStreams` registry; Track B uses the same registry for recording streams (one entry per channel). |
| Track D | W6.5 fix-sprint (12 codex+gemini findings, especially P1-001/P1-002/P1-003/P1-004/P1-005/P2-003) MUST be merged. Track D relies on the real `switch_media_bug_t*` pointer, the fixed CS_DESTROY hook, the per-bug-pointer remove, the long-lived context, and the wired resampler. |
| Track D | W6.6 silence driver SHOULD be merged. Track D integration tests on parked channels need a write-side driver; if missing, tests must add `<action playback="silence_stream://-1"/>` to fixture dialplans manually. |
| Track D | FF-036 (`write_replace_frame_out` passthrough semantics) — already landed in this plan PR. |

**Track A, Track B, and Track D are independent at the file-system level** —
Track A touches `src/security/`, `src/control/handlers/start_tts*.cc`,
`src/control/handlers/start_voicebot*.cc`, `src/mod_open_switch.cc`;
Track B touches `src/media/recording_relay.cc`,
`src/media/stereo_pairer.cc`, `src/media/bug_manager.cc`,
`src/control/handlers/start_recording_relay_handler.cc`,
`src/control/handlers/stop_recording_relay_handler.cc`,
`proto/open_switch/control/v1/control.proto`, `src/mod_open_switch.cc`;
Track D touches `src/media/bot_session.cc`, `src/media/bug_fanout.cc`,
`src/control/active_bots.cc`, `src/control/handlers/start_bot_handler.cc`,
`src/control/handlers/stop_bot_handler.cc`,
`src/control/handlers/media_bug_callbacks.cc` (PATCH — add bot callbacks
alongside existing W6 ones), `proto/open_switch/control/v1/control.proto`,
`src/mod_open_switch.cc`.

The shared files are `src/mod_open_switch.cc` (module Load / Shutdown
wiring), `proto/open_switch/control/v1/control.proto` (3 tracks add
new RPCs), and `src/control/handlers/media_bug_callbacks.cc` (Track D
extends; Track A only reads channel variables). Merge conflict risk
is moderate; recommended order: **Track A first** (smallest), then
**Track B** (rebases on top of A), then **Track D** (rebases on top
of B). Each tracks lands on `redesign/from-specs` via separate PRs.

## Worktree layout (orchestrator-owned)

```text
/tmp/open-switch              (main, orchestrator clean clone)
/tmp/open-switch-w7a          (Track A worktree, branch implementation/wave7-track-a-eavesdrop)
/tmp/open-switch-w7b          (Track B worktree, branch implementation/wave7-track-b-recording)
/tmp/open-switch-w7d          (Track D worktree, branch implementation/wave7-track-d-bot-multitarget)
```

The orchestrator creates the worktrees from `main` after W6 Track C
merges, hands off the per-track brief, and reaps each worktree when
its PR merges.

## Wave timeline (target)

| Day | Activity |
|---|---|
| D0 | W6 Track C + W6.5 fix-sprint + W6.6 silence-driver merged. Orchestrator spawns Track A + Track B + Track D Sonnet sub-agents in parallel. |
| D1–D2 | Sub-agents draft + commit + open PRs. |
| D2 | CI green on PRs. Orchestrator merges in order: Track A → Track B → Track D (rebases each on top of the previous merge). |
| D3 | All three tracks merged. |
| D3–D4 | Codex CLI wave-level review → fix-sprint W7.5 if findings. |
| D4 | Wave closeout doc + Gemini sanity review post-fix-sprint. |
| D5 | (Buffer) Real-build smoke from operator side; address any FS-host-only finding. |

## Out of scope (deferred to W8 or V2)

- **AMD detect handler** — proto slot is reserved, no V1 use case yet.
- **Conference recording** — bot in multi-party conference; stage-rank
  model extends but not on the V1 path.
- **Bot-as-SIP-leg recording (Option A in `recording-with-bot.md`)** —
  operator-side dialplan pattern; module doesn't add code for it.
- **Patching FreeSWITCH to expose `eavesdrop_callback`** — out of
  scope for V1 (carrying an FS patch is a maintenance cost we are not
  taking on). Layer 2 stays detection-only at v1.10.12.
- **Real-time enforcement of eavesdrop policy on raw `eavesdrop`** —
  documented as a Layer-1 + Layer-3 (ACL) operator responsibility;
  module emits audit and does not remove the bug.
- **OAuth2 / signed audit envelopes / OIDC** — V1.5+.

## Project-memory recap for sub-agents

These rules are HARDENED in project memory and apply to every commit:

- **No AI co-author trailers.** Author / committer is `@luongdev`.
  Do NOT add `Co-Authored-By: ...` lines.
- **Reviews use Codex or Gemini CLI only.** Sub-agents do NOT request
  Claude review; the orchestrator runs Codex/Gemini after merge.
- **Run with ASAN=ON locally before push.** Lesson from W6 Track A
  where an ASAN=OFF local run let a `BugCallbackContext` leak through
  to CI. Both Track A and Track B sub-agents MUST do
  `cmake -DENABLE_ASAN=ON -DENABLE_TSAN=OFF` (or the project's
  equivalent build alias) on every commit cycle.
- **No force-push without explicit user authorization** — even on the
  sub-agent's own feature branch, after the first push, every
  subsequent rewrite-history push requires a fresh per-action approval
  from the user. Auto-mode classifier enforces this.
- **No commits of build artifacts** — `build/`, `*.o`, `*.so`, `*.pb.cc`
  go in `.gitignore`.

## Acceptance gate (wave-level)

W7 ships when:

- [ ] Track A merged on `redesign/from-specs`; PR CI green
      (proto / amd64 / arm64 / TSAN / clang-tidy / markdownlint).
- [ ] Track B merged on `redesign/from-specs`; PR CI green.
- [ ] Codex CLI review produces a findings doc; orchestrator runs the
      W7.5 fix-sprint if any P1/P2 findings exist.
- [ ] Gemini CLI post-fix-sprint sanity check produces a clean
      report.
- [ ] Wave-closeout doc `W7-review-report.md` written by the
      orchestrator and committed.
- [ ] Fresh Docker builder from `main` HEAD builds and passes the repo
      gates:

      ```bash
      docker buildx build \
        --build-arg OSW_ENABLE_ASAN=ON \
        --build-arg OSW_BUILD_TESTS=ON \
        --build-arg BUILD_TYPE=Debug \
        --build-arg BASE_TAG=1.10.12-trixie \
        --target fs-builder \
        -f deploy/docker/Dockerfile.builder \
        --load \
        -t open-switch/builder:w7-gate \
        .

      docker run --rm \
        -e ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=1 \
        -e LSAN_OPTIONS=exitcode=23:print_suppressions=0 \
        open-switch/builder:w7-gate \
        ctest --test-dir /usr/src/open-switch/build --output-on-failure -L unit

      docker run --rm open-switch/builder:w7-gate \
        ctest --test-dir /usr/src/open-switch/build --output-on-failure -L integration
      ```

## Track briefs

- [`W7-track-A-eavesdrop-policy.md`](W7-track-A-eavesdrop-policy.md) — Eavesdrop
  policy (Layer 1 app + Layer 2 detector + channel-var marker + audit).
- [`W7-track-B-recording-relay.md`](W7-track-B-recording-relay.md) — RECORDING_RELAY
  purpose + StereoFramePairer + StartRecordingRelay / StopRecordingRelay
  RPC + LATE-stage refinement.
- [`W7-track-D-bot-multitarget.md`](W7-track-D-bot-multitarget.md) — `StartBot` /
  `StopBot` multi-target bug attachment + BugFanout + per-target SPSC
  queues + ActiveBots registry + per-target bug callbacks.
