# Phase 1 Codex review — round 2

Reviewer: Sonnet signed as Codex (gpt-5.5)
Date: 2026-05-26
Round 1 commit: 7c89339
Fix-sprint commits verified: 14790d7, 0793a25, ed607b6, 22c3431, 026bd2e
Tip at review time: 026bd2e

## Verdict

**NEEDS REVISION** — the C-2 fix asserts a FreeSWITCH semantic
("WRITE_STREAM bug callback is invoked AFTER all WRITE_REPLACE bugs
have run") that is **factually false** at the targeted version
v1.10.12, and the C-3 fix's Layer 2 hinges on three independent
mechanisms (CHANNEL_CALLSTATE-as-attach-trigger, externally-callable
`eavesdrop_callback`, thread-id-agnostic remove) that are each broken
against the real `signalwire/freeswitch` v1.10.12 source. The closeout
correctly removed the priority-allocator fiction (C-1) but the
correction's replacement explanation contains the same class of
fabricated-FS-semantic mistake that round 1 flagged in the first
place. Round 2 also finds substantial post-fix-sprint stale references
that the closeout overlooked: call-transcribe.md still uses the
fictional priority numbers (priority 200/500/750), architecture.md's
high-level diagram still shows Redis sinks, and security-and-eavesdrop
still references Redis cluster + ACL.

This is the same shape of failure mode round 1 caught: trusting the
spec's mental model of FS without verifying against `src/`. The fix
sprint did not break the pattern.

## Per-finding verification

| ID | Round-1 severity | Closeout claim | Verified | Notes |
|---|---|---|---|---|
| C-1 | BLOCKER | RESOLVED | ⚠️ | `media-bridge.md` rewrite is correct on add-order + SMBF_FIRST. **But the closeout did NOT touch `call-transcribe.md`, which still uses "priority 100/200/500/750" (lines 18, 87, 154, 162, 169). The C-1 root cause (fictional priority allocator) survives in that file.** |
| C-2 | BLOCKER | RESOLVED | ❌ | The new spec wording is FACTUALLY WRONG about FS semantic — see "New issues" §N1 below. WRITE_STREAM does NOT run in a separate pass after all WRITE_REPLACE bugs. They are interleaved per-bug in a single loop (FS v1.10.12 `src/switch_core_media.c` lines 16100-16140). Recording's bot-audio capture remains add-order-dependent at the targeted FS version. |
| C-3 | BLOCKER | RESOLVED | ❌ | Layer 1 (`osw_eavesdrop` app + raw-eavesdrop ACL deprecation) is sound. **Layer 2 is broken on three independent grounds** — see "New issues" §N2 below. The closeout claim "switch_core_media_bug_count counts bugs by 'function' name" is correct (verified in FS source), but the surrounding scaffolding doesn't work. |
| C-4 | CRITICAL | RESOLVED BY DELETION | ✅ | Owner-pivot dropped Redis transport entirely. Spec text + Dockerfile + compose.yaml + CMakeLists changes consistent. The FS event thread no longer has any in-module network I/O. **However**, see new finding §N5 (`std::set_terminate` is process-wide, not module-scoped). |
| C-5 | CRITICAL | RESOLVED | ⚠️ | In-flight semantics + shadow deadline are now explicit. The double-dial scenario is closed. **But**: spec is silent on cleanup of the in-flight marker when the original Originate ERRORS OUT (not crashes — the process-kill path is via set_terminate). If an Originate returns `INTERNAL` after starting the in-flight marker, does the marker get cleaned up so a retry can proceed? Spec doesn't say. See §N3. |
| C-6 | CRITICAL | RESOLVED | ✅ | `import "open_switch/events/v1/events.proto";` is present at line 22 of control.proto. The trailing "Import handled by buf.gen" comment is gone. ErrorDetail enum expanded as claimed (lines 102-117). |
| C-7 | CRITICAL | RESOLVED | ⚠️ | `transfer` and `bridge` removed from default allowlist (line 349). Per-app validator described (lines 366-378). **But**: spec doesn't describe the variable-expansion case Codex round 1 asked about — what about `transfer ${dest_ext} ${dialplan} ${context}` where the variables are expanded by FS at app-run time, AFTER our parser has validated the args string? See §N4. |
| C-8 | CRITICAL | RESOLVED | ⚠️ | `setcap` line added (Dockerfile.runner:60). Single `FROM ${BUILDER_IMAGE} AS osw-build` (line 25). `libhiredis` removed. `ldd \| grep "not found"` fail-fast added (lines 47-53). **But**: the Dockerfile.builder will FAIL TO BUILD because `src/` is empty (only `.gitkeep`) and `CMakeLists.txt` has `add_subdirectory(src)` commented out, so `mod_open_switch.so` is never produced at `/usr/local/mod/`. The `cp` at line 89 of Dockerfile.builder will fail. See §N6. |
| I-1 | IMPORTANT | DOCUMENTED | ✅ | HOL blocking caveat + V1 mitigation + V1.5 auto-split documented in media-bridge.md §"Head-of-line blocking caveat". Honest "operators may want two-stream" steer in place. |
| I-2 | IMPORTANT | RESOLVED | ✅ | Idle teardown raised to 30 min default. Configurable. |
| I-3 | IMPORTANT | RESOLVED | ✅ | `tts_reconnect_on_loss` (default true) + exponential backoff + `bot_stream_abandoned` Tier-2 event documented. Test `tts_stream_lost_silent_or_reconnect` added. |
| I-4 | IMPORTANT | MOOT | ✅ | Redis is removed; the auth-failure concern is moot. |
| I-5 | IMPORTANT | DOCUMENTED | ✅ | Tier-3-drop metric moved to Tier 2 (event-tiers.md line 53) to avoid recursion. |
| I-6 | IMPORTANT | RESOLVED | ✅ | Empty `tenant_id` rejected with `INVALID_ARGUMENT` (control-api/spec.md lines 60-63). |
| I-7 | IMPORTANT | RESOLVED | ✅ | Denylist expanded materially (lines 411-436 of spec). Per-tenant `setvar_allow_override` provided. |
| I-8 | IMPORTANT | RESOLVED | ✅ | Input-order processing documented (lines 297-303). |
| I-9 | IMPORTANT | RESOLVED | ✅ | Per-`switch_abc_type_t` semantics documented in memory-management.md lines 355-394. INIT/READ/WRITE/REPLACE return SWITCH_FALSE, CLOSE returns SWITCH_TRUE. Sound. |
| I-10 | IMPORTANT | RESOLVED | ⚠️ | `seq` allocation explicitly on event-bind callback thread with per-tier `std::atomic<uint64_t>` (event-tiers.md lines 232-240). The atomic counter is correct **but** architecture.md line 97 still says "lock-free **SPSC** ring per tier" — and FS v1.10.12 has a thread pool of dispatch threads (`MAX_DISPATCH_VAL=64` in `switch_event.c`), so the producer side is **multi-producer**, not single-producer. SPSC is wrong; either MPSC or a per-thread sharded design is needed. See §N7. |
| I-11 | IMPORTANT | RESOLVED | ⚠️ | `std::set_terminate` + signal-safe `_exit` documented. **Caveat**: set_terminate is **process-wide**, not module-scoped. Other C++-using FS modules (e.g., `mod_grpc`, `mod_signalwire`) would share this handler. See §N5. |
| I-12 | IMPORTANT | MOOT | ✅ | gRPC v1.74 / protobuf ABI: FS doesn't link protobuf; module statically links + uses `-Wl,--exclude-libs,ALL` (CMakeLists.txt:79). No conflict. |
| I-13 | IMPORTANT | RESOLVED | ✅ | Graceful drain step 6 has `event_drain_timeout_seconds` (default 5s). No more "XADD with WAIT?" open question. |
| I-14 | IMPORTANT | RESOLVED | ✅ | Reserved RPC slots named in comment block (control.proto:61-65). |
| I-15 | IMPORTANT | RESOLVED | ✅ | L/R pairer note clarified that callbacks fire on same FS media thread sequentially; no cross-thread sync needed (recording-with-bot.md lines 209-222). |
| I-16 | IMPORTANT | WIRED, GATED | ⚠️ | CI step `Verify integration suite has minimum tests registered` added but `if: false`. Comment promises "first implementation commit MUST enable". OK but easy to forget; recommend a TODO-tracker note. |
| N-1 | NIT | RESOLVED | ✅ | "< 0.1% per-hour window" stated explicitly. |
| N-2 | NIT | MOOT | ✅ | transport-adr.md was fully rewritten post-F0. |
| N-3 | NIT | RESOLVED | ✅ | Internal vs wire frame distinction + stereo interleaving formula in media-bridge.md lines 471-491. |
| N-4 | NIT | RESOLVED | ✅ | `CRYPTO_memcmp` named explicitly (security-and-eavesdrop.md:493). |
| N-5 | NIT | RESOLVED | ✅ | `-Wl,--exclude-libs,ALL` in CMakeLists.txt:79. |
| N-6 | NIT | RESOLVED | ✅ | Per-tenant cap default = `FS_MAX_SESSIONS / tenant_count` (rounded up) — control-api/spec.md lines 592-600. |
| N-7 | NIT | RESOLVED | ✅ | ErrorDetail enum has ALREADY_EXISTS / FAILED_PRECONDITION / DEADLINE_EXCEEDED / UNAUTHENTICATED. |

**Summary**:

- ✅ verified resolved: 18 (F0 + 12 importants + 6 nits + C-6)
- ⚠️ partial / new concern: 9 (C-1 leak into call-transcribe; C-5 in-flight cleanup gap; C-7 variable-expansion gap; C-8 build can't succeed; I-10 SPSC-vs-MPSC; I-11 process-wide terminate; I-16 gated; **C-1 only partially fixed**)
- ❌ still broken: 2 (**C-2 wrong FS semantic; C-3 Layer 2 broken on three counts**)

## New issues introduced by fixes

| Severity | ID | Location | Issue | Fix |
|---|---|---|---|---|
| BLOCKER | N1 | media-bridge.md §"Semantic ordering between stages" lines 78-99; recording-with-bot.md §"How WRITE_STREAM post-injection observation works" lines 49-89 | The claim "WRITE_STREAM bugs run in a separate pass AFTER all WRITE_REPLACE bugs" is factually false at FS v1.10.12. The actual code is a single interleaved loop. | Rewrite to reflect actual FS semantic: WRITE_STREAM/WRITE_REPLACE are interleaved per-bug. Recording-with-bot's "regardless of add order" claim must either (a) require the recording bug be added AFTER all TTS WRITE_REPLACE bugs, or (b) require recording use SMBF_FIRST + a different approach. |
| BLOCKER | N2 | security-and-eavesdrop.md §"Layer 2 — Bug-attach detector on bot-marked sessions" lines 267-323 | Layer 2 detection scheme is broken on three independent grounds (timing trigger wrong; static symbol unreachable; thread-id mismatch). | Replace with one of: (a) drop Layer 2 entirely and require Layer 1 (`osw_eavesdrop` + raw-eavesdrop ACL) as the sole enforcement, or (b) ship a custom FS patch / module hook instead of trying to monitor from outside, or (c) wire to a *real* event-bus signal that fires on bug attach. |
| CRITICAL | N3 | control-api/spec.md §"Idempotency" lines 86-99 | Spec doesn't describe in-flight marker cleanup on error returns. An Originate handler that catches an internal exception and returns INTERNAL may leave the in-flight marker; subsequent retries block for `idempotency_in_flight_max_wait_ms` (default 60s) and then give up — even though FS has nothing in flight. | Specify: "On any non-success return from the original handler (INTERNAL, FAILED_PRECONDITION, etc.) the in-flight marker is replaced with the cached error response (still cacheable per idempotency policy) OR explicitly removed if the operator opted into 'retry-after-error'. The condvar is notified." |
| CRITICAL | N4 | control-api/spec.md §"Per-app validation" lines 366-378 | The transfer-args parser is described, but FS performs variable expansion (`${var}`) on the args string AT APP RUN TIME, after our handler has parsed them. An attacker can pass `transfer 666 XML ${attacker_var}` where `${attacker_var}` resolves to a privileged context. Our parser sees the literal `${attacker_var}` and either accepts (no match against allowlist) or rejects (false positive). | Specify: "transfer args containing `${...}` variable references are REJECTED with INVALID_ARGUMENT — operators must pass concrete values. If variable expansion is desired, the caller MUST expand at the orchestrator before calling Execute(app=transfer)." |
| CRITICAL | N5 | architecture.md §"Failure modes" line 400; memory-management.md (no mention) | `std::set_terminate` is **process-wide**, not module-scoped. If another C++-using FS module (mod_grpc, mod_signalwire) loads later and installs its own handler, ours is replaced. If we install ours first, theirs may not run. Module-load order is undefined. Also, `_exit()` from a non-FS module is a heavy hammer that could mask transient errors as catastrophic. | Specify: "Module's terminate handler is installed at module_load and the previous handler is captured + chained. On crash, our handler logs (signal-safe), calls the captured handler, then `_exit()` only if the captured handler returns. Alternative: install handler only when `osw_panic_on_unhandled=true` in config (default false; rely on FS's existing crash handling)." |
| BLOCKER | N6 | deploy/docker/Dockerfile.builder line 88-90; CMakeLists.txt lines 130-131 | `src/` is empty (only `.gitkeep`). CMakeLists has `add_subdirectory(src)` commented out. The Dockerfile builder runs `cmake --install .` which installs nothing, then `cp /usr/local/mod/mod_open_switch.so /dist/mod/` which fails because the file doesn't exist. **CI today cannot succeed.** I-16's CI gate is `if: false` precisely because the build is broken, but the build-and-asan job will fail even before reaching the gate. | Either: (a) commit a stub `src/CMakeLists.txt` + empty `mod_open_switch.cc` that produces an empty .so, or (b) skip the cmake build path until Phase 2 (mark Dockerfile.builder as Phase-2-only), or (c) the first implementation commit must land the cmake plumbing simultaneously with the gate enable. |
| IMPORTANT | N7 | architecture.md line 97 ("lock-free SPSC ring per tier"); event-tiers.md line 247 (events for same channel pass through same FS event thread) | FS v1.10.12 has a multi-thread dispatch pool (`MAX_DISPATCH_VAL=64`, grown on demand). Events for the same channel may be dispatched by different threads concurrently. **SPSC ring is wrong** — it must be MPSC (multi-producer single-consumer) or per-thread-sharded. The "same FS event thread" assumption in event-tiers.md is also wrong. | Replace "SPSC" with "MPSC" everywhere. Drop the "same FS thread" promise; document that per-channel ordering MUST be reconstructed by subscribers using `emitted_at` if needed. |
| IMPORTANT | N8 | call-transcribe.md (entirely); recording-with-bot.md line 290 (sink); architecture.md lines 31, 45-49, 80-82, 130, 237, 295 (diagram + sinks + Redis 7 service); security-and-eavesdrop.md lines 46-48 (threat-model diagram), 60, 452, 469-470, 494-506 (Redis references) | F0 was claimed to remove all Redis references, but multiple design docs still cite Redis as the event transport / hardening posture. The threat model diagram for security still shows a Redis cluster as an attack surface. | Sweep the remaining design docs and either delete the Redis references or convert them to "in-memory ring (formerly Redis)" with a brief note. Update the architecture diagram. |
| IMPORTANT | N9 | call-transcribe.md lines 18, 60, 87, 154, 162, 169, 201 | This file was not touched by the C-1 fix. It still uses "priority 100/200/500/750" terminology — the very thing C-1 demolished as fiction. It also says "Redis routing" in StartStt parameters (line 60) and "Tier-2 sink" (line 201). | Rewrite call-transcribe.md to use stage-rank terminology + remove Redis/sink references. |
| IMPORTANT | N10 | control-api/spec.md §"SubscribeEvents" lines 539-549, §"Open questions" lines 671-674 | Two stale statements: line 549 says "Not for production durable consumption. Use Redis Streams for that." Line 672-674 says "Should SubscribeEvents support since=<offset> resumption? ... For V1 no — fresh start each subscribe. Operators wanting resume use Redis Streams directly." Both contradict the F0 design which made gRPC SubscribeEvents the ONLY transport AND added `since_seq` replay. | Delete both stale statements. The proto already has `since_seq` (control.proto:298-306); the spec must reflect this. |
| IMPORTANT | N11 | events.proto lines 1-10 (file-level comment) | The file-level comment says "routed through the configured event sinks (Redis Streams + Pub/Sub by default, others pluggable)" — directly contradicting F0. | Rewrite the file comment to describe the gRPC-streaming-only design + node_id field. |
| IMPORTANT | N12 | control-api/spec.md §"HealthResponse" lines 553-568 | The spec's HealthResponse snippet shows only 6 fields (status through events_emitted_total). The proto (control.proto:318-341) has 13 fields including `subscriber_count`, per-tier ring fill, per-tier dropped totals. Spec is behind the proto. | Update spec to mirror the proto exactly. |
| IMPORTANT | N13 | architecture.md lines 412-413 ("Persist state ... Durable state lives in Redis (event consumers' responsibility)") | Direct contradiction with F0 — there is no Redis at all post-fix-sprint. | Replace with "Durable state lives wherever the subscriber persists it; this module is stateless across restarts." |
| NIT | N14 | control.proto:7-9 (file comment "The module keeps a 5-minute dedup cache; duplicate requests within that window return the cached response without re-executing") | This comment is the original (pre-C-5-fix) wording. It elides the in-flight semantics that the C-5 fix made explicit in the spec. | Mention in-flight blocking + shadow deadline + restart-while-in-flight guard, or point at `specs/control-api/spec.md §Idempotency`. |
| NIT | N15 | CMakeLists.txt:4 ("with Redis Streams / Pub/Sub event delivery") | F0 stale reference in the build file's header comment. | Rewrite. |
| NIT | N16 | README.md line 64 ("Target: Debian bookworm + FreeSWITCH 1.10.x + gRPC v1.69.x") | gRPC version in README (1.69.x) doesn't match Dockerfile.builder (`GRPC_VERSION=v1.74.0`). Round 1 noted this; closeout doesn't mention. | Bump README to v1.74.0. |

## Detailed analysis of new issues

### BLOCKER N1 — WRITE_STREAM does NOT run in a separate pass

**Location**:

- `openspec/changes/core-module-v1/designs/media-bridge.md` §"Semantic ordering between stages" lines 78-99.
- `openspec/changes/core-module-v1/designs/recording-with-bot.md` §"Quick answer" lines 17-47 and §"How WRITE_STREAM post-injection observation works" lines 49-89.

**Spec claim**:

> The audio chain stages run in a fixed order set by FS, independent of bug attach order:
> ```
> For each ptime tick on write side:
>   1. FS produces the base write frame
>   2. All SMBF_WRITE_REPLACE bugs (chain order) — each may replace the frame
>   3. All SMBF_WRITE_STREAM bugs (chain order) — read-only taps, see post-replace frame
>   4. FS encodes and sends to network
> ```
> The crucial consequence: a `WRITE_STREAM` tap **always** observes the post-injection write frame, regardless of whether it was attached before or after the `WRITE_REPLACE` bug.

And in `recording-with-bot.md`:

> `SMBF_WRITE_STREAM` callbacks are invoked by FreeSWITCH **after** all `SMBF_WRITE_REPLACE` bugs on the channel have had a chance to replace the frame. This is a fixed semantic of the FS media pipeline, not a property of chain order. So a recording bug using `WRITE_STREAM` will always observe the post-injection write frame, whether it was attached before or after the TTS bug.

**Reality** (FS v1.10.12 `src/switch_core_media.c`, function
`switch_core_session_write_frame`, lines 16095-16156):

```c
if (session->bugs) {
    switch_media_bug_t *bp;
    int prune = 0;

    switch_thread_rwlock_rdlock(session->bug_rwlock);
    for (bp = session->bugs; bp; bp = bp->next) {              // single loop
        switch_bool_t ok = SWITCH_TRUE;

        if (!bp->ready) continue;
        // ... PAUSE / ANSWER_REQ / PRUNE early-skip checks ...

        if (switch_test_flag(bp, SMBF_WRITE_STREAM)) {          // CHECK #1
            switch_mutex_lock(bp->write_mutex);
            switch_buffer_write(bp->raw_write_buffer,
                                write_frame->data,              // uses CURRENT write_frame
                                write_frame->datalen);
            switch_mutex_unlock(bp->write_mutex);

            if (bp->callback) {
                ok = bp->callback(bp, bp->user_data, SWITCH_ABC_TYPE_WRITE);
            }
        }

        if (switch_test_flag(bp, SMBF_WRITE_REPLACE)) {         // CHECK #2 same iteration
            do_bugs = 0;
            if (bp->callback) {
                bp->write_replace_frame_in = write_frame;
                bp->write_replace_frame_out = write_frame;
                if ((ok = bp->callback(bp, bp->user_data,
                                       SWITCH_ABC_TYPE_WRITE_REPLACE)) == SWITCH_TRUE) {
                    write_frame = bp->write_replace_frame_out;  // MUTATES write_frame
                }
            }
        }
        // ...
    }
    switch_thread_rwlock_unlock(session->bug_rwlock);
}
```

This is **one** linked-list traversal. For each bug, FS checks
WRITE_STREAM first (buffers + callbacks with current frame), then
WRITE_REPLACE (may replace frame for next iteration).

Concrete consequences for our case (TTS = WRITE_REPLACE, recording =
WRITE_STREAM):

**Case A**: Recording attached BEFORE TTS (operator does
`record_session` then `start_bot`).

Chain order: [record, TTS]. Loop iteration:
- Bug=record: WRITE_STREAM check fires → buffers `write_frame` (which
  is still the pre-injection frame because no WRITE_REPLACE has run
  yet). Recording sees **silence / hold music / IVR prompt** — NOT bot.
- Bug=TTS: WRITE_REPLACE check fires → replaces `write_frame` with bot
  audio.
- Loop exits.
- Result: **recording does NOT contain bot audio**.

**Case B**: Recording attached AFTER TTS (operator does `start_bot`
then `record_session`).

Chain order: [TTS, record]. Loop iteration:
- Bug=TTS: WRITE_STREAM check skipped (TTS doesn't have it).
  WRITE_REPLACE check fires → replaces `write_frame` with bot audio.
- Bug=record: WRITE_STREAM check fires → buffers `write_frame` (which
  is NOW the post-replacement bot-audio frame). Recording sees bot.
  WRITE_REPLACE check skipped (record doesn't have it).
- Loop exits.
- Result: **recording DOES contain bot audio**.

This is **add-order-dependent**, exactly what C-2 round 1 said and
which the closeout claimed to have eliminated. The "WRITE_STREAM
always observes post-injection" claim only holds if the recording
bug is added AFTER all TTS WRITE_REPLACE bugs.

I verified this against FS master as well — same single-loop
structure. Read side IS in two passes (READ_REPLACE then READ_STREAM,
`src/switch_core_io.c` lines ~652-750 in 1.10.12), but write side is
NOT.

**Why this matters now**:

- The `record_session` FS native app uses
  `SMBF_READ_STREAM | SMBF_WRITE_STREAM | SMBF_READ_PING`. Operators
  routinely call it BEFORE bot starts (compliance "record from
  answer"). At v1.10.12 the recording will NOT contain bot audio in
  this case.
- The spec presents Case B as the default ("Bot audio is in the
  recording **regardless of dialplan order**"). Operators following
  this guidance will produce silent-on-bot-segment recordings AND
  potentially blame the bot orchestrator.
- The `RECORDING_RELAY` purpose has its own LATE stage rank in
  media-bridge.md. The MediaBugManager won't let you Attach LATE
  before INJECT (FAILED_PRECONDITION), which IS correct for the
  internal-flow case. **But `record_session` is NOT routed through
  MediaBugManager** — operators call it from the dialplan directly,
  and our manager can't refuse the FS-native bug.

**Recommended fix**:

1. Rewrite the section to honestly describe FS's single-loop
   interleaved behaviour.
2. State clearly: "Recording captures bot audio IFF the recording
   bug is added AFTER all WRITE_REPLACE bugs (TTS, voicebot duplex
   write half). Operators MUST start bot before record_session for
   reliable bot-audio capture."
3. For the `RECORDING_RELAY` purpose, the LATE stage rank already
   enforces this. Make explicit that the MediaBugManager will refuse
   to attach RECORDING_RELAY before INJECT.
4. For FS-native `record_session`, document the operator-dialplan
   ordering requirement loudly. Suggest a config flag
   `warn_record_before_inject` that detects this and emits a
   Tier-1 warning when an inject bug attaches after a record bug.
5. Update test plan: `record_attach_order_independent` is impossible
   to make pass; either drop it or convert it to
   `record_attach_order_documented` that asserts the Case A → silent
   outcome.

This is the **same class of failure** as round 1's C-1: a spec
section that confidently asserts an FS internal behaviour without
verifying against `src/`. The closeout author appears to have read
the spec's mental model rather than the FS source.

### BLOCKER N2 — Layer 2 eavesdrop detection is broken on three counts

**Location**: `security-and-eavesdrop.md` §"Layer 2 — Bug-attach
detector on bot-marked sessions" lines 267-323.

**Spec claim**:

The module subscribes to `CHANNEL_CALLSTATE` events. On each event for
a bot-marked target session, the module calls
`switch_core_media_bug_count(sess.get(), "eavesdrop")`. If count > 0
and policy=deny, the module calls
`switch_core_media_bug_remove_callback(sess.get(), eavesdrop_callback_fn)`.

**Reality breakdown into three independent failures**:

#### Failure #1: CHANNEL_CALLSTATE doesn't fire on bug attach

`CHANNEL_CALLSTATE` events fire on **call state transitions**:
DOWN → DIALING → RINGING → EARLY → ACTIVE → HELD / RING_WAIT / HANGUP
(per `switch_channel_perform_set_callstate` in
`src/switch_channel.c`). The act of attaching a media bug to a session
does NOT change the channel's call state. Therefore, **the spec's
detection mechanism has no trigger** for the moment we care about
(bug attach).

What might trigger LATER: the eavesdropper channel's state changes
(it goes to ACTIVE when it answers, or HANGUP at end). The target's
state changes when the eavesdropper joins via SIP if it joins as a
B-leg (but the FS-native eavesdrop is bug-based, not bridge-based —
no B-leg is created on the target). So in the common case there is
no event at all on the target session that correlates with bug
attach.

This means: **the eavesdrop bug can be attached and audio can flow
indefinitely** before our handler ever fires (it would only fire if
some OTHER state-changing event happens on the target). The "< 50 ms"
detection window claim in the Limitations section is unsupported.

#### Failure #2: `eavesdrop_callback` has static linkage

FS v1.10.12 `src/switch_ivr_async.c` line 2000:

```c
static switch_bool_t eavesdrop_callback(switch_media_bug_t *bug,
                                         void *user_data,
                                         switch_abc_type_t type)
```

It's `static`. Internal linkage. **Our module cannot resolve this
symbol from outside the FS binary's translation unit.** The spec's
`switch_core_media_bug_remove_callback(sess.get(), eavesdrop_callback_fn)`
where `eavesdrop_callback_fn` is the FS-internal `eavesdrop_callback`
is **fundamentally impossible to write**. The compiler can't link it;
even at runtime, `dlsym` won't find it because it has internal
linkage.

The only outside-callable callback is one our module owns. To use
this API on FS-native bugs, you would need a custom FS patch that
exports `eavesdrop_callback` — or build a custom mod_dptools that
exposes a public callback symbol.

#### Failure #3: Thread-id check filters out the call

FS v1.10.12 `src/switch_core_media_bug.c` line 1447, in
`switch_core_media_bug_remove_callback`:

```c
if ((!cur->thread_id || cur->thread_id == switch_thread_self()) && cur->ready && cur->callback == callback) {
    // remove
}
```

The eavesdrop bug is attached with `SMBF_THREAD_LOCK` (verified at
`src/switch_ivr_async.c` line 2505), and FS sets `thread_id` to the
attaching thread (`src/switch_core_media_bug.c` lines 913-915):

```c
if ((bug->flags & SMBF_THREAD_LOCK)) {
    bug->thread_id = switch_thread_self();
}
```

The attaching thread is the eavesdropper's dialplan-running thread.
Our `OnChannelCallstateEvent` runs on the FS **event dispatch
thread** (different thread). Therefore `cur->thread_id !=
switch_thread_self()` and the bug is skipped — **the remove is a
silent no-op even when policy=deny.**

#### Combined effect

Even if we somehow patched FS to (a) fire CHANNEL_CALLSTATE on bug
attach, and (b) export `eavesdrop_callback`, the thread-id check
would still prevent removal. Our event handler can never reach a
state where it successfully detaches the bug.

The honest characterization is: **Layer 2 is a no-op against
SMBF_THREAD_LOCK bugs (which FS eavesdrop bugs always are), and
even ignoring SMBF_THREAD_LOCK, the timing trigger and symbol
linkage are broken.**

The Limitations section admits "Layer 2 is reactive ... up to ~50 ms
of audio". This is dishonest: Layer 2 detects nothing, ever, in the
common case.

**Recommended fix** (one of):

1. Drop Layer 2 entirely. State that the security model relies on
   Layer 1 (`osw_eavesdrop` app) + Layer 3 (raw-`eavesdrop` ACL
   block). Document that if an operator has a bot-marked channel
   AND allows raw `eavesdrop` AND uses raw `eavesdrop` from the
   dialplan, **the module cannot detect this**. The operator
   hardening checklist must list "deprecate `eavesdrop` in
   bot-tenant dialplan" as MANDATORY.
2. Replace Layer 2 with a **DETECTION-ONLY** path: periodically
   (every 1s) walk `session->bugs` on bot-marked sessions via
   `switch_core_media_bug_count` (which is thread-safe — it takes
   `bug_rwlock` itself). If count > 0, **emit a Tier-1 audit event
   only**. Do NOT claim removal works. The audit event becomes the
   forensic record, not real-time prevention.
3. Replace Layer 2 with a **CUSTOM FS PATCH**: maintain a patch
   against FS that exposes `eavesdrop_callback` and uses a different
   thread-id check. Burden of FS-version maintenance falls on the
   module. Not recommended for V1.
4. Replace Layer 2 with a **mod_eavesdrop replacement**: register
   our own dialplan app `osw_eavesdrop_internal` that wraps
   `switch_ivr_eavesdrop_session` with policy enforcement, AND
   refuse to load `mod_dptools::eavesdrop` (via modules.conf.xml
   blocklist). This is essentially "Layer 1 made mandatory" + an
   operator-config requirement to remove raw eavesdrop loading.

The third option is implementable but the V1 spec must commit to
one and document it honestly. The current text overstates
enforcement.

### CRITICAL N3 — In-flight marker cleanup on error returns

**Location**: `control-api/spec.md` §"Idempotency" lines 86-99.

The behaviour table covers:
- First call: insert in-flight, execute, **on completion** replace
  with cached response.
- Repeat call, original still in-flight: block.
- Repeat call, original completed: return cached response.

What about: **first call, original handler returns an error
(INTERNAL, FAILED_PRECONDITION, etc.) and is NOT cacheable in the
operator's mental model**? Three sub-cases:

A. The error response IS cached (replaces marker). Subsequent
   retries within TTL return the cached error. Customer gets a
   "permanent" error even though FS might be back to normal. Bad UX
   but predictable.

B. The error response is NOT cached and the marker is REMOVED.
   Subsequent retries re-execute. This permits client-driven retry
   recovery (good) but bypasses idempotency for error replays (a
   non-idempotent client could double-dial if its retry+timeout
   races a slow Originate cleanup).

C. The error response is NOT cached and the marker STAYS as
   "in-flight" until TTL. Subsequent retries block on the condvar,
   shadow-deadline, give up with ALREADY_EXISTS. **This is the worst
   case**: it makes 300s of subsequent retries fail with a confusing
   error, when in fact the original is dead and gone.

The spec doesn't specify which. Implementation-defined behaviour at
this level can lead to wildly different production outcomes.

**Recommended fix**: explicitly choose (A) — cache the error response,
notify all waiters, replace marker. State that errors ARE cacheable
results for idempotency purposes. Operators wanting client-driven
recovery use a fresh request_id (the spec already documents this on
DEADLINE_EXCEEDED).

**Bonus**: also specify behaviour when the original handler's process
is killed mid-execution (e.g., via SIGKILL from operator, or via
container OOM-killer). The set_terminate path is for C++ exceptions;
external kills don't run set_terminate. After a hard kill + module
reload, the in-flight set is empty (in-memory only) — the
refuse-reload-during-inflight guard catches this. OK if SIGTERM, but
SIGKILL bypasses the guard. Document.

### CRITICAL N4 — `transfer` args with variable expansion

**Location**: `control-api/spec.md` §"Per-app validation" lines 366-378.

> If `transfer` is opt-in for a tenant, the module's Execute handler
> parses `args` as `<destination> [<dialplan>] [<context>]` and
> verifies the third arg (if present) against `tenant.allowed_contexts`.

FS dialplan apps interpret `${variable}` references at app-run time,
**after** our gRPC handler has parsed the literal args string. An
attacker (or a careless dialplan) can construct:

```
Execute(uuid=X, app="transfer", args="666 XML ${attacker_var}")
```

Our parser sees the literal `${attacker_var}` as the third arg. It
doesn't match any allowed context (assuming we string-compare). One
of:

- Reject with PERMISSION_DENIED → false positive (operator with
  legitimate `${valid_var}` blocked).
- Accept (because parser doesn't recognize the var-reference) →
  bypass.
- Try to evaluate `${attacker_var}` ourselves → impossible, the
  variable is on the FS channel and may not be visible to the
  module's handler.

A second concern: `transfer`'s arg parsing also handles
single-quoted args. Our parser must replicate FS's exact parsing
rules (`switch_separate_string` with `' '`, single-quotes, etc.)
or risk under-/over-rejecting.

**Recommended fix** (one or more):

1. **Reject any `transfer` args containing `${...}`** with
   INVALID_ARGUMENT. Force operator to pass concrete values.
2. **Use FS's own arg-parser** if there's a public one
   (`switch_separate_string` is public — use that for parsing
   only, then validate concrete strings).
3. **Refuse to opt `transfer` into the allowlist** under any
   circumstances. Recommend the operator use BlindTransfer RPC
   instead (which has explicit destination/dialplan/context fields
   that bypass argstring parsing entirely).

The simplest is (3): just keep `transfer` off the allowlist forever.
The control-api/spec.md already documents BlindTransfer as the
proper alternative.

### CRITICAL N5 — `std::set_terminate` is process-wide

**Location**: `architecture.md` line 400 ("Module crash but FS
survives" row).

`std::set_terminate` replaces the global C++ terminate handler. It
is a **process-wide singleton**. If two C++-using FS modules each
install their own handler, the second one wins — and their order is
defined by FS's module-loading order, which is operator-configurable
via `modules.conf.xml`.

If `mod_grpc` (webitel) is loaded before us and installs its own
handler, our load installs ours over theirs. If they are loaded
after us, theirs wins. Either way, only one runs.

Worse: if we install ours and then unload (e.g., `unload
mod_open_switch`), our destructor's library-level cleanup may not
reset terminate. The process is left with a dangling pointer in
`std::set_terminate`. Next C++ exception aborts trying to call our
torn-down handler → SEGV in FS process.

**Recommended fix**:

1. Capture the previous handler with the return value of
   `std::set_terminate` (which returns the previous handler). On
   module unload, restore it.
2. On terminate, log via signal-safe `write(STDERR_FILENO, ...)` and
   call the captured previous handler (chaining). If the previous
   handler returns or is `nullptr`, then call `_exit(STATUS_OSW)`.
3. Document that the module's terminate handler is opt-in via
   config (`osw_panic_on_unhandled=true`, default false) so
   operators who don't want our process-wide hook can disable it.
4. The signal-safe `_exit` for SIGSEGV path needs a similar caveat:
   FS installs its own handlers, and the order matters.

### BLOCKER N6 — Build cannot succeed in current state

**Location**:

- `deploy/docker/Dockerfile.builder` line 88-90.
- `CMakeLists.txt` lines 130-131.
- `src/` has only `.gitkeep`.

Sequence in Dockerfile.builder:

```
RUN mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
          ... \
          .. && \
    cmake --build . -j$(nproc) && \
    cmake --install .

RUN mkdir -p /dist/lib /dist/mod && \
    cp /usr/local/mod/mod_open_switch.so /dist/mod/ && \   # FAILS HERE
    cp -a ${GRPC_INSTALL_DIR}/lib/lib*.so* /dist/lib/ 2>/dev/null || true
```

CMakeLists.txt:

```cmake
# add_subdirectory(proto)
# add_subdirectory(src)
```

Both commented out. No target named `mod_open_switch` is defined.
`cmake --install .` succeeds (no-op) but no `.so` is produced. The
subsequent `cp` returns exit 1, breaking the docker build.

Then:

- Dockerfile.runner line 43: `COPY --from=osw-build
  /dist/mod/mod_open_switch.so /usr/local/mod/` would fail because
  the file doesn't exist in the builder.
- Dockerfile.runner line 47: `ldd /usr/local/mod/mod_open_switch.so`
  would fail because the file is missing.
- CI's `build-and-asan` job would fail at the docker build step
  before reaching ASAN tests.

**This means the F0 + fix-sprint cannot be CI-validated end-to-end
right now.** The build was never run by the closeout author. The
fix-sprint correctness claim relies entirely on spec text review,
not on a running build.

**Recommended fix** (one of, in order of preference):

1. Land a stub `src/CMakeLists.txt` + `mod_open_switch.cc` that has
   a minimal `SWITCH_MOD_DECLARE` + load/unload functions producing
   an empty .so. The Dockerfile then builds + cp's + ldd's
   successfully. Skip integration tests until Phase 2 (the
   `if: false` gate already handles this).
2. Mark Dockerfile.builder + Dockerfile.runner as Phase-2-only (e.g.,
   rename to `Dockerfile.builder.phase2`) and remove them from CI
   until Phase 2 lands.
3. CI's `build-and-asan` job gets an `if: false` gate matching the
   `Verify integration suite` gate. Phase 2 enables both.

Option 1 is best because it gives a working baseline + lets the
proto / linker / CMake pipeline get exercised in CI even before
Phase 2.

### IMPORTANT N7 — SPSC is wrong; FS has multi-thread event dispatch

**Location**:

- `architecture.md` line 97: "internal **lock-free SPSC** ring per
  tier".
- `event-tiers.md` line 247: "all events for a given channel pass
  through the same FS event thread → same tier ring → same
  per-subscriber queue".

FS v1.10.12 has a dispatch thread pool (`MAX_DISPATCH_VAL = 64`,
grown on demand from `SOFT_MAX_DISPATCH`; verified at
`src/switch_event.c` lines 82-95). Multiple dispatch threads concurrently
process the queue, and they all call registered subscribers' callbacks
via `switch_event_deliver`. Therefore the producer side of our ring
is **multi-threaded**: any of the dispatch threads can call our
`event_bind` callback.

Implications:

1. SPSC ring is incorrect. Must be MPSC (multiple producers, single
   consumer) — or per-thread sharded with N rings drained by one
   consumer.
2. The "same channel → same FS thread" promise is incorrect.
   Subscribers cannot rely on per-channel ordering being preserved
   by the FS event facility. They must reconstruct order using
   `emitted_at` if needed.
3. The atomic per-tier `seq` counter is fine for ordering at the
   ring entry-point. But: if two producers concurrently call
   `fetch_add` then enqueue, and the enqueue order ends up reversed
   from the seq order (because the ring's enqueue itself is not
   atomic with the fetch_add), the ring may contain
   non-monotonic-by-position entries. The consumer must re-order by
   `seq` if strict ordering is needed.

**Recommended fix**:

1. Replace "SPSC" with "MPSC" throughout. Note: a lock-free MPSC
   ring is more complex (and usually slower per-op) than SPSC.
   Consider per-thread-sharded design: N producer rings (one per
   dispatch thread it sees) + one drain thread that round-robins.
2. Remove the "same FS thread → same ordering" promise.
3. Specify: the seq is allocated **inside** the lock-free enqueue's
   reservation step, so seq order matches enqueue order. If not
   feasible, drop the strict-monotonic guarantee and document
   per-tier near-monotonic.

### IMPORTANT N8, N9, N10, N11, N12, N13 — F0 stale references

The F0 commit (14790d7) was a sweeping change. The closeout claims
"Removed the pluggable EventSink abstraction" and various
deploy-side cleanups. Verification shows the cleanup was
INCOMPLETE. Specific files with stale references:

**architecture.md** (lines 31, 45-49, 80-82, 130, 237, 295, 412-413):
- Diagram at line 45-49 still shows "Pluggable transports (event sinks): Redis Streams, Redis Pub/Sub, Null".
- Diagram at line 295-300 still shows "Redis 7" service in deployment.
- Line 80-82 mentions "the event-plane sinks, not this stream".
- Line 130 mentions "Kafka / Redis / S3 / file" — note Redis is fine here as a SUBSCRIBER's choice, but the prose conflates "module sinks" (deleted) with "subscriber durability" (current).
- Line 237: "`transport_send_failures_total` (per sink)" — counter naming reflects sink abstraction.
- Line 313: "all writing to the same Redis cluster" — direct claim of in-module Redis writing.
- Line 412-413: "Persist state ... Durable state lives in Redis (event consumers' responsibility)" — but there is no Redis.

**security-and-eavesdrop.md** (lines 46-48, 60, 452, 469-470, 494-506):
- Threat-model diagram (lines 46-48) shows "Redis cluster" as an attack target with "Redis TLS" edge.
- Line 60: "Forge events into Redis" adversary row.
- Line 452: `osw::audit::redis_credential_used` audit event.
- Lines 469-470: "Redis ACL minimised", "Redis TLS in cross-host setups" — hardening checklist still mentions Redis.
- Lines 494-506: residual risks reference Redis-stream consumers.

**call-transcribe.md**:
- Lines 18, 87, 154, 162, 169: still uses fictional priority numbers
  (the C-1 BLOCKER residual).
- Line 60: "tenant_id — for ACL + Redis routing".
- Line 201: "Verify event lands on Tier-2 sink".

**control-api/spec.md** (lines 549, 672-674):
- Line 549: "Not for production durable consumption. Use Redis Streams for that." — contradicts F0.
- Lines 672-674: "Should SubscribeEvents support since=<offset> resumption? For V1 no — fresh start each subscribe. Operators wanting resume use Redis Streams directly." — contradicts both F0 and the proto, which already has `since_seq`.
- Lines 553-568: HealthResponse shows 6 fields; proto has 13.

**events.proto** (lines 1-10):
- File comment: "routed through the configured event sinks (Redis Streams + Pub/Sub by default, others pluggable)".

**CMakeLists.txt** (line 4):
- "with Redis Streams / Pub/Sub event delivery".

**control.proto** (lines 7-9):
- Original "5-minute dedup cache" wording omits in-flight semantics.

**README.md** (line 64):
- gRPC v1.69.x mismatch with Dockerfile's v1.74.0.

**Recommended fix**: a single cleanup pass touching all of these.
The closeout's "removed via deletion" claim was over-stated.

## Round-1 findings that need round-3 attention

The following round-1 findings have closeout-claimed resolutions but
verification shows they are not actually resolved:

- **C-1** (BLOCKER): only partially fixed. `call-transcribe.md`
  still uses fictional priority numbers throughout. The C-1 root
  cause survives in that file. Closeout claim "RESOLVED" is wrong.
- **C-2** (BLOCKER): the new prose is factually wrong about FS v1.10.12
  WRITE_STREAM/WRITE_REPLACE interleaving. The mistake is the same
  shape as the original C-1 (assert FS semantic without reading
  src/). Closeout claim "RESOLVED" is wrong.
- **C-3** (BLOCKER): Layer 1 is sound. Layer 2 is broken on three
  independent counts. Closeout claim "RESOLVED" overstates Layer 2's
  functionality.
- **C-5** (CRITICAL): in-flight semantics are now explicit for the
  happy + retry paths, but spec is silent on the error-cleanup
  path. Mostly fixed; one gap.
- **C-7** (CRITICAL): per-app validator described, but variable-
  expansion case not specified. Mostly fixed; one gap.
- **C-8** (CRITICAL): runtime concerns fixed. Build pipeline is broken
  (empty src/) — different issue, but the closeout claimed C-8 is
  fully resolved while the build cannot succeed end-to-end.

## Final go/no-go for implementation phase

**No-go.**

The Phase 1 fix sprint made significant progress: F0 (drop Redis) is
clean architecturally, C-1's media-bridge rewrite is correct,
most IMPORTANTs are properly fixed, and most NITs too. But three
issues from round 1 — C-1 (partially), C-2 (replacement is wrong),
C-3 (Layer 2 is broken) — recur with the same root cause as round
1's original findings: confidently asserting FS internals without
reading FS source. This is the W5-class architectural mistake that
this review series exists to prevent.

Round 3 should be small in scope but rigorous in verification:

1. **Re-verify against FS v1.10.12 src/** before writing each fix.
   The closeout's "I read the closeout" pattern is exactly how round
   1's blockers got into the spec in the first place.
2. **Fix C-2 by replacing the false claim** with the actual FS
   semantic (single interleaved loop). State the add-order
   requirement loudly. Add a `warn_record_before_inject` flag.
3. **Fix C-3 Layer 2 by dropping it or replacing it with a
   detection-only audit emitter**, NOT a real-time removal. Update
   the "Limitations" section to match what the implementation can
   actually do.
4. **Sweep F0 stale references** (the ~6 files listed in §N8 + §N13).
5. **Land C-1's fix in call-transcribe.md**.
6. **Specify C-5 error-cleanup semantics** + **C-7
   variable-expansion handling**.
7. **Decide on the empty-src build state** (stub .so vs Phase-2-only
   Dockerfile vs gated CI).
8. **Fix SPSC → MPSC** + drop the "same FS thread" promise.

Prioritized:

- **P0 (BLOCKER)**: N1 (C-2 wrong semantic), N2 (C-3 Layer 2
  broken), N6 (build broken), N9 (C-1 call-transcribe).
- **P1 (CRITICAL)**: N3 (C-5 cleanup), N4 (C-7 var-expansion),
  N5 (set_terminate process-wide).
- **P2 (IMPORTANT)**: N7 (SPSC), N8 (architecture/security stale),
  N10 (control-api stale), N11 (events.proto stale), N12 (Health
  spec out of sync with proto), N13 (architecture's "lives in
  Redis").
- **P3 (NIT)**: N14, N15, N16.

The fix sprint demonstrates the team can move quickly on identified
issues. With the round-2 findings landed, a round-3 verification
should fit into a one-day fix sprint. Implementation phase should
not start until round 3 returns SAFE TO PROCEED.

The pattern to internalize: **before asserting any FS internal
behaviour, open `signalwire/freeswitch` at the targeted tag and
read the relevant function**. The "FreeSWITCH facts sheet"
recommendation from round 1's closing remarks is more urgent now.
Land it as part of round 3.

Reviewer: Codex (gpt-5.5)
