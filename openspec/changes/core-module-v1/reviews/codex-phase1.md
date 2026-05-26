# Phase 1 Codex review — mod_open_switch

Reviewer: Sonnet signed as Codex (gpt-5.5)
Date: 2026-05-26
Specs reviewed: 10 docs at commit `0b371c1`
Reviewer's brief: adversarial review of architecture, memory safety, event
tiering, transport ADR, media bridge, STT, recording, eavesdrop, control
API. Mandated to break with prior W5-class architectural blind spots.

---

## Verdict

**NEEDS REVISION** — three of the design's load-bearing claims are
factually wrong about FreeSWITCH internals, and one of them is the W5-class
"writing bytes to the wrong socket" mistake: the media-bug priority allocator
is fiction. The other two — the eavesdrop Layer-2 enforcement and the
`record_session` "runs at ~700-750" claim — collapse for the same root
reason. Until those are fixed at the design level, no implementation should
start.

The rest of the design is largely sound; the memory-management discipline,
license posture, transport ADR, and Tier 1/2/3 taxonomy are mostly defensible
with small fixes. The work to repair the three core fictions is days, not
weeks, but it has to happen before Phase 2.

## Top concerns (executive summary)

1. **The bug-priority allocator (`media-bridge.md` §"Priority allocation")
   is not how FreeSWITCH works.** `switch_core_media_bug_add` ignores the
   "priority" concept entirely. Bugs are appended to a linked list in
   add-order; only `SMBF_FIRST` provides head-insertion. The numeric
   priorities 100/200/300/500/700/750 in the spec are not honored by FS.
   This is **BLOCKER C-1**.
2. **The "recording captures bot audio" claim depends on add-order,
   not priority.** `switch_ivr_record_session` does NOT pass `SMBF_FIRST`,
   so it appends. If our TTS `WRITE_REPLACE` bug is attached AFTER
   `record_session`, recording will see the pre-injection (silent) write
   side. The design's correctness is invariant to operator dialplan
   ordering, but the actual behavior is fragile to dialplan ordering. **BLOCKER C-2**.
3. **The eavesdrop Layer-2 enforcement reads a variable that does
   not exist on the eavesdropper channel.** `mod_dptools::eavesdrop`
   does NOT set `eavesdrop_uuid` on the eavesdropper channel. The state
   handler in `security-and-eavesdrop.md` §"Layer 2" will fire `SUCCESS`
   (no policy applied) for every eavesdrop session, by design. The
   "hard enforcement" claim is unsupported. **BLOCKER C-3**.
4. **Tier 1 back-pressuring the FS event thread is unsafe in a way
   the spec underestimates.** FS event facility shares its threading
   with critical signaling (`mod_sofia` SIP transactions can be
   serialised on the same event dispatch). Blocking the event thread
   for "a few minutes" while Redis is down means new INVITEs cannot be
   processed. The spec says "we accept this"; the operator probably
   doesn't. **CRITICAL C-4**.
5. **Architectural inconsistencies in `Originate` cancellation,
   idempotency-cache rebuild, and proto cross-package imports** that
   produce surprising runtime behavior (duplicate originates, false-
   negative dedup window, build-time circular import). Each is
   addressable but should be resolved in Phase 1. **CRITICAL C-5 .. C-8**.

## Findings table

| Severity | ID | Spec location | Issue |
|---|---|---|---|
| BLOCKER | C-1 | `media-bridge.md` §Priority allocation | FreeSWITCH does not implement bug-priority ordering. Bugs append in add-order; only `SMBF_FIRST` heads. The spec's 100/200/500/700/750 are not real. |
| BLOCKER | C-2 | `recording-with-bot.md` §Quick answer + §Bug priority ordering | "record_session attaches at FS's default priority — which ends up near 700-750" is false. Add-order determines chain; if TTS bug is added after `record_session`, recording misses bot audio. |
| BLOCKER | C-3 | `security-and-eavesdrop.md` §"Layer 2" | `eavesdrop_uuid` is not set by `mod_dptools::eavesdrop` on the eavesdropper channel. The state handler's policy check will silently no-op for every eavesdrop session. Hard enforcement is missing. |
| CRITICAL | C-4 | `event-tiers.md` §"Tier 1 backpressure policy"; `architecture.md` §"Event plane" | Blocking FS event facility under Redis outage blocks SIP signaling and call setup. "A few minutes" of back-pressure may equal "the SIP gateway looks down". |
| CRITICAL | C-5 | `control-api/spec.md` §"Originate"; `idempotency_ttl_seconds` setting | 60s timeout + 300s dedup TTL + module restart wipes cache. Client retry after timeout collides with an in-flight Originate. |
| CRITICAL | C-6 | `control.proto` lines 60-63, 301-302 | Control proto references `open_switch.events.v1.EventEnvelope` but does not `import "open_switch/events/v1/events.proto";`. The trailing "Import handled by buf.gen" is incorrect — buf.gen.yaml does not synthesize imports. |
| CRITICAL | C-7 | `control-api/spec.md` §"Execute" | `transfer` is in the default allowed-app list. `Execute(app=transfer, args=)` bypasses the dial-context ACL because `transfer` itself targets arbitrary contexts. |
| CRITICAL | C-8 | `Dockerfile.runner` lines 16-44 | Stage `osw-builder` defined but not used; the runtime libs reachable to FS are inconsistent. `freeswitch` user + `cap_add: SYS_NICE` requires file caps on the FS binary, which the runner image does not set. |
| IMPORTANT | I-1 | `media-bridge.md` §"VOICEBOT_DUPLEX"; `architecture.md` §"Media plane" | Single gRPC stream for two bugs serialises READ/WRITE_REPLACE via a single send/receive pair → head-of-line blocking risk when TTS is rate-limited. |
| IMPORTANT | I-2 | `media-bridge.md` §"gRPC channel pool" — idle 5 min teardown | Mid-call quiet period > 5 min (hold music, IVR menu) tears down the gRPC channel; first frame after teardown costs a full TLS handshake. |
| IMPORTANT | I-3 | `media-bridge.md` §"Reconnect mid-call" + Failure modes | On bug stream loss for TTS, channel continues with SILENCE. For a voicebot that is unsupervised, silence ≠ "graceful degradation"; it's a stalled call. No reconnect attempt is specified. |
| IMPORTANT | I-4 | `transport-adr.md` §"redis-plus-plus connection per shipper thread" + "Reconnect on error" | `redis-plus-plus` `Redis` object is thread-safe but auto-reconnect on ACL-AUTH failure is NOT guaranteed — auth failures stick. Spec needs explicit re-authenticate logic. |
| IMPORTANT | I-5 | `event-tiers.md` §"Tier 3 dropping is fine but..." | The Tier-3-drop metric event itself is Tier 1; under sustained Tier-3 overflow, you flood Tier 1 with `osw::tier3_dropped` events at 1/min. Rate-limit is documented but check the rate-limiter is per-(sink, subclass), not global. |
| IMPORTANT | I-6 | `security-and-eavesdrop.md` §"Idempotency cache keyed by (tenant, request_id)" | If `tenant_id` is empty (allowed by proto3 zero-default), the cache key degenerates. Spec must either reject empty tenant or hash empty as `(__anon__, req)`. |
| IMPORTANT | I-7 | `control-api/spec.md` §"SetVariables — Reserved variable prefixes" | Allowlist of reserved prefixes is incomplete: `bridge_*`, `playback_*`, `recording_*` (note: not `record_*`), `hold_music`, `transfer_*`, `originate_*`, `endpoint_*`, etc. each affect security or recording. Denylist-by-prefix is wrong direction; switch to per-variable allowlist for set, or expand denylist materially. |
| IMPORTANT | I-8 | `control-api/spec.md` §"HangupMany" | Spec says "may return DEADLINE_EXCEEDED with partial result" but doesn't specify order. Caller "resumes with remainder" but doesn't know which are in `hungup_uuids` (the response includes them) — but order vs input is unspecified. Specify input-order processing. |
| IMPORTANT | I-9 | `memory-management.md` §"Exception-safety boundary"; `media-bridge.md` `osw_bug_callback` | The `catch (...)` returns SWITCH_FALSE for media bug. FS may interpret SWITCH_FALSE as "bug should be removed" but the in-flight callback returning SWITCH_FALSE on `SWITCH_ABC_TYPE_INIT` vs `_READ` has different consequences. Audit per-ABC-type semantics. |
| IMPORTANT | I-10 | `event-tiers.md` §"Sequencing guarantees: seq strictly increasing" | "Lock-free SPSC ring per tier" — `seq` allocation must be on the producer (event_bind callback) side, before enqueue, with a per-tier `std::atomic<uint64_t>`. Spec doesn't say where seq is allocated; if allocated on the shipper thread, gaps appear under reorder. |
| IMPORTANT | I-11 | `architecture.md` §"Module crash but FS survives" | Plan is "set up a custom SIGSEGV handler that aborts to trigger container restart". A SIGSEGV handler that runs in the broken process can race with FS's own handlers and result in zombie state. Recommend setting `abort()` via `std::set_terminate` for unhandled C++, not SIGSEGV. |
| IMPORTANT | I-12 | `Dockerfile.builder` line 21-22; `CMakeLists.txt` line 99-100 | gRPC v1.74.0 + Debian trixie's protobuf 3.21.12 ABI mismatch potential. Trixie's `libprotobuf-dev` is not used here (good — module builds fully against `/opt/grpc`), but FreeSWITCH itself does not link protobuf so this is a non-issue. Verify in CI by `ldd mod_open_switch.so` showing only `/opt/grpc/lib/libprotobuf.so` and not `/usr/lib/x86_64-linux-gnu/libprotobuf.so`. |
| IMPORTANT | I-13 | `architecture.md` §"Graceful drain" step 8 (Redis flush) — open question | "XADD with WAIT?" is unresolved. WAIT blocks for replication ACK; not appropriate at drain. Use a synchronous round-trip via `redis-plus-plus` synchronous mode and a per-tier drain timeout. Resolve in spec, don't punt to implementation. |
| IMPORTANT | I-14 | `proto/open_switch/control/v1/control.proto` line 23-66 | `Attended transfer` is mentioned as "Reserved for v1" but the proto declares no reserved tag — this is a comment-only deferral. If V1 ships and a V1.1 adds it, the new RPC slot uses next tag. OK but flag for explicit `// reserved tag XX for AttendedTransfer` style markers. |
| IMPORTANT | I-15 | `media-bridge.md` §"Stereo recording L/R pairer" + "FS media thread is single-threaded per channel" | Spec says "5ms tolerance, 25ms fill timeout" implying cross-thread sync. Both read-tap and write-tap callbacks fire on the same media thread sequentially, not concurrently. Pairer logic is correct in outcome but the spec misleads the implementer toward locking that isn't needed. |
| IMPORTANT | I-16 | `tests/integration/` per CI workflow `ci.yml` | Directory is `tests/integration/.gitkeep` only. CI `Run integration tests under ASAN` runs `ctest -L integration` with no integration test labels in the binary → trivially passes. **CI green is a false signal at Phase 1.** Add an explicit "ASAN integration suite must register at least N labeled tests" guard. |
| NIT | N-1 | `event-tiers.md` table at line 23-29 | "Acceptable loss rate" for Tier 2 is "< 0.1%" — over what window? Per-stream lifetime? Per-hour? Specify. |
| NIT | N-2 | `transport-adr.md` §"MULTI/EXEC?" | Document concludes "Not used in V1" but does not specify that consumer dedup is the at-least-once contract — already covered elsewhere but a forward-reference would help. |
| NIT | N-3 | `media-bridge.md` Frame format details (line 334-345) | `payload` is `std::vector<uint8_t>` in the internal struct but `bytes payload` in proto. State that "payload sent on the wire is `payload.data()..size()`; no length prefix in proto value". |
| NIT | N-4 | `security-and-eavesdrop.md` §"Timing attacks" | `constant_time_compare` is referenced but not specified where it lives. Either point at the OpenSSL function used (`CRYPTO_memcmp`) or commit to a stdlib helper. Otherwise implementer rolls their own and gets it wrong. |
| NIT | N-5 | `proposal.md` §Risks | Risk 2 "gRPC version drift" mentions `-Wl,--exclude-libs,ALL` but `CMakeLists.txt` does not set it. Add to CMake link flags now. |
| NIT | N-6 | `architecture.md` §"Per-tenant active-call cap" | Default = unbounded; operator sets `max_active_channels`. Make it bounded by default at FS's own `max-sessions` divided by tenant count rounded up — prevents foot-gun for first-time operators. |
| NIT | N-7 | `proto/open_switch/control/v1/control.proto` `ErrorDetail` line 87-104 | `ErrorDetail.type` enum is missing `ALREADY_EXISTS`, `FAILED_PRECONDITION`, `DEADLINE_EXCEEDED` — but `control-api/spec.md` references them. Either add to enum or document mapping to `INTERNAL` (which loses signal). |

---

## Detailed analysis per finding

### BLOCKER C-1 — Bug priorities are not how FreeSWITCH works

**Location**: `openspec/changes/core-module-v1/designs/media-bridge.md`,
section "Priority allocation" (lines 52-69).

**Claim in spec**:

> Lower priority number = runs earlier in the chain.
> | Priority | Purpose | Flags | Notes |
> | 100 | VAD / barge-in detector | `READ_STREAM` | Tap raw read first; cheap analysis. |
> | 200 | STT | `READ_STREAM` | ...
> | 500 | Bot TTS write | `WRITE_REPLACE` | ...
> | 700 | Recording (read tap) | ...
> | 750 | Recording (write tap) | ...
> ...
> Bug priority is set at `switch_core_media_bug_add` via the `flags`
> field (high byte) and via add-order. The earlier-added bug runs
> earlier in the chain. We use an explicit priority allocator (below)
> rather than depending on add-order.

**Reality** (from `signalwire/freeswitch` `src/switch_core_media_bug.c`):

The `switch_core_media_bug_add` function signature is:

```c
SWITCH_DECLARE(switch_status_t) switch_core_media_bug_add(
    switch_core_session_t *session, const char *function, const char *target,
    switch_media_bug_callback_t callback, void *user_data, time_t stop_time,
    switch_media_bug_flag_t flags, switch_media_bug_t **new_bug);
```

The `flags` parameter is a bitfield of `SMBF_*` flags (`switch_media_bug_flag_t`).
There is no "high byte priority" encoding. The chain insertion code does
exactly one of two things:

```c
// from switch_core_media_bug.c, switch_core_media_bug_add
if (!session->bugs) {
    session->bugs = bug;
    added = 1;
} else if (switch_test_flag(bug, SMBF_FIRST)) {
    bug->next = session->bugs;
    session->bugs = bug;
    added = 1;
}
if (!added) {
    for (bp = session->bugs; bp; bp = bp->next) {
        if (!bp->next) { bp->next = bug; break; }   // append to tail
    }
}
```

So:

- `SMBF_FIRST` puts the bug at the head.
- Everything else appends in add-order.
- There is **no** numeric priority allocator. Priorities 100, 500, 750
  are spec fiction.

**Impact**:

If two bugs need ordering relative to each other, the only mechanisms are:

1. Add them in the right order. (Add-order is a property of the *caller*,
   not of the bug.)
2. Mark one as `SMBF_FIRST`. (Binary, not graduated.)

The spec's claim that the priority allocator "ensures recording captures
bot audio regardless of dialplan order" is unsupported. See C-2.

**Recommended fix**:

1. Replace `media-bridge.md` priority allocator with an explicit add-order
   coordinator. The `MediaBugManager` per-channel tracks which logical
   roles are attached and applies them in a deterministic order matching
   the spec table — BUT enforces this by:
   - Refusing to attach a "later" role until the "earlier" roles already
     present in the chain.
   - On attach of a "later" role when an "earlier" role attaches afterward,
     temporarily detaching and re-attaching to maintain chain order
     (expensive, document the cost).
   - OR, accept that the chain order is what the caller built and
     document the contract that the operator's dialplan must call
     start-VAD → start-STT → start-AMD → start-TTS in that order
     before any recording starts.
2. Drop all references to "priority 100/200/300/500/700/750". Replace
   with named ordering tiers: `ANALYSIS_EARLY`, `ANALYSIS_LATE`,
   `INJECT`, `RECORD_READ`, `RECORD_WRITE`, and document the required
   add-order to achieve them.
3. Update `recording-with-bot.md` and `call-transcribe.md` to remove
   priority references.

This is the W5-class architectural mistake. The spec describes a
mechanism that does not exist. Implementation cannot proceed.

---

### BLOCKER C-2 — Recording captures bot audio is order-dependent

**Location**: `recording-with-bot.md` §"Quick answer" (lines 14-25) and
§"Bug priority ordering for recording" (lines 27-50).

**Claim in spec**:

> Yes, recording captures bot audio if the FS recording bug runs after
> our TTS write-replace bug in the chain. We arrange priorities to
> make this the default.
> ...
> `record_session` (FS native) attaches at FS's default priority — which
> ends up near 700-750. So recording's read tap sees the same audio STT
> sees (caller's mic), and recording's write tap sees the channel write
> side after our TTS injection (priority 500 runs first). Result:
> recording captures the bot's voice.

**Reality**:

From `switch_ivr_record_session_event` in `signalwire/freeswitch`
`src/switch_ivr_async.c`, the recording bug is attached with:

```c
flags = SMBF_READ_STREAM | SMBF_WRITE_STREAM | SMBF_READ_PING;
```

No `SMBF_FIRST`. No priority. The bug appends to the tail of
`session->bugs` at the moment `record_session` is called. Per C-1, FS
walks bugs in linked-list order.

The "recording captures bot audio" outcome depends entirely on whether
the TTS `WRITE_REPLACE` bug was already in the chain at the moment the
recording bug was added, AND whether any subsequent bug attached with
`SMBF_FIRST` re-inserted at head.

If the operator's dialplan is:

```xml
<action application="record_session" data=".../>
<action application="lua" data="start_bot.lua"/>  <!-- attaches TTS bug -->
```

Then: record bug is added FIRST → appended to head. Then TTS bug is
added → appended to tail. The chain is: record, ..., TTS. The
WRITE_REPLACE happens AFTER record_session's write-tap consumes the
frame. **Recording sees pre-injection (silent) write side; recording
does not contain bot audio.**

If the operator reverses the order:

```xml
<action application="lua" data="start_bot.lua"/>
<action application="record_session" data=".../>
```

Then TTS bug is added first → chain is TTS. Record bug is added second
→ chain is TTS, record. The WRITE_REPLACE happens first, then record
tap reads the post-injection write side. **Recording contains bot
audio.**

So the entire premise of `recording-with-bot.md` is operator-dialplan-
ordering-dependent. The spec presents the second case as the default;
in practice operators routinely start recording *first* (compliance
"record from answer" mantra), then add bot. That's the W5-class bug:
**the design promises a property that requires non-default operator
behavior, while documenting it as default.**

Note also: `record_session` uses `SMBF_READ_STREAM | SMBF_WRITE_STREAM`.
The WRITE_STREAM bug callback is invoked AFTER all WRITE_REPLACE bugs
in the chain have run (FS's design). So if both TTS and record are in
the chain in any order, record's `WRITE_STREAM` callback receives the
post-injection frame. **This is consistent with the spec's outcome but
the *cause* is the WRITE_REPLACE-vs-WRITE_STREAM semantic, not bug
priority.** The spec's explanation is wrong; the outcome happens to be
correct only for the WRITE_STREAM case.

**Impact**:

- For mono-mixed recording via `record_session` (default FS), the
  outcome is actually correct because `record_session` uses WRITE_STREAM
  (which sees post-WRITE_REPLACE audio). The spec's "priority 500 runs
  first" reasoning is wrong but the conclusion happens to hold.
- For the module's `RECORDING_RELAY` purpose, the read-tap (priority
  "700") and write-tap (priority "750") need to be attached *after*
  the TTS bug (priority "500") is attached. This is order-dependent.
  An operator who starts `RECORDING_RELAY` BEFORE `StartTts` gets
  bot-less recording.

**Recommended fix**:

1. Rewrite §"Quick answer" of `recording-with-bot.md` to say:
   "Recording captures bot audio when the recording bug uses
   `SMBF_WRITE_STREAM` (not `SMBF_WRITE_REPLACE`) and the TTS bug is
   `SMBF_WRITE_REPLACE`. The WRITE_STREAM callback observes the
   channel's write frame *after* all WRITE_REPLACE bugs have run, so
   the ordering of attaches does not matter for this case."
2. For `RECORDING_RELAY` with `SMBF_WRITE_STREAM`, no add-order
   constraint exists (because WRITE_STREAM is post-WRITE_REPLACE).
   Document this explicitly.
3. The "L=caller, R=bot" pairer (stereo recording) needs to:
   - Take read-tap frame as L (caller's mic).
   - Take write-tap frame as R (post-injection write side, which is
     "what caller hears", i.e., bot + hold music + ... ).
   "Bot on R" is correct IFF "caller hears nothing but bot", which is
   the bot-only scenario. If there's IVR audio mixed with bot, R is
   the mix, not bot alone. Document this.
4. Drop all priority references. Replace with: "Recording write-tap
   uses `SMBF_WRITE_STREAM` (NOT `WRITE_REPLACE`) which guarantees
   post-injection observation."

---

### BLOCKER C-3 — Eavesdrop Layer-2 enforcement is non-functional

**Location**: `security-and-eavesdrop.md` §"Layer 2 — State handler on
eavesdrop channel" (lines 233-265) plus §"Hooking eavesdrop" lines
202-231.

**Claim in spec**:

> When `mod_dptools` spawns the eavesdrop session, it sets channel
> variable `eavesdrop_uuid` on the eavesdropper channel pointing to the
> target. We register a `switch_state_handler_table_t` with an
> `on_init` callback that checks every new channel:
> ```cpp
> const char* target_uuid = switch_channel_get_variable(chan, "eavesdrop_uuid");
> if (!target_uuid) return SWITCH_STATUS_SUCCESS;  // not an eavesdrop
> ```

**Reality** (from `signalwire/freeswitch`
`src/mod/applications/mod_dptools/mod_dptools.c` `eavesdrop_function` and
`src/switch_ivr_async.c` `switch_ivr_eavesdrop_session`):

`mod_dptools::eavesdrop_function` reads these variables from the
**eavesdropper** channel to control behavior:

- `eavesdrop_require_group`
- `eavesdrop_enable_dtmf`
- `eavesdrop_bridge_aleg` / `eavesdrop_bridge_bleg`
- `eavesdrop_whisper_aleg` / `eavesdrop_whisper_bleg`
- `eavesdrop_indicate_failed` / `eavesdrop_indicate_new`

It then calls `switch_ivr_eavesdrop_session(session, uuid, ...)` where
`uuid` is the data argument to the app (i.e., the target UUID, passed
to the eavesdrop app).

`switch_ivr_eavesdrop_session` itself attaches a media bug to the
**target session** with:

```c
switch_core_media_bug_add(tsession, "eavesdrop", uuid, eavesdrop_callback,
                          ep, 0,
                          read_flags | write_flags | SMBF_READ_PING |
                          SMBF_THREAD_LOCK | SMBF_NO_PAUSE | stereo_flag,
                          &bug);
```

**Critically**: neither `mod_dptools::eavesdrop_function` nor
`switch_ivr_eavesdrop_session` sets `eavesdrop_uuid` on the
**eavesdropper** channel. The "data" argument the dialplan passes to
`eavesdrop` is the target UUID, but it is NOT stored as a channel
variable on the eavesdropper.

The spec's state handler checks `switch_channel_get_variable(chan,
"eavesdrop_uuid")` on each new channel. This will return NULL for every
eavesdrop session because the variable is never set. The handler will
short-circuit to `SUCCESS` and **no policy enforcement happens.**

**Impact**:

- "Hard enforcement" — the headline assertion of the eavesdrop policy
  design — is a no-op. Every eavesdrop bypasses the deny policy.
- Audit events never fire (the `osw::events::EmitEavesdropAudit` call
  is unreachable).
- `policy=deny` silently degrades to `policy=allow`.
- The dialplan-snippet layer (Layer 1, §"Dialplan recommendation") is
  the only enforcement — and it relies on operator including the
  snippet. If forgotten: zero enforcement.

**Recommended fix**:

Three options, in order of robustness:

1. **Best**: instead of relying on a non-existent channel variable,
   register a state handler that walks each new channel and asks "does
   this channel have an active media bug on a bot-marked target?" The
   `switch_core_media_bug_*` API lets you enumerate bugs on a session.
   The eavesdrop bug has callback `eavesdrop_callback` (string name);
   you can match by callback name string. When a media bug named
   "eavesdrop" appears on a bot-marked session, fire the policy check.

   Wire this up via `switch_core_event_hook_add_state_change` or via
   monitoring CHANNEL_BRIDGE / CHANNEL_OUTGOING events: the eavesdrop
   session shows up as a B-leg bridge to the target.

2. **Acceptable**: wrap or replace `mod_dptools::eavesdrop` with the
   module's own eavesdrop application registered as `osw_eavesdrop`
   (FS allows multiple apps; deprecate `eavesdrop` for bot tenants via
   ACL). This shifts enforcement to the application gate rather than
   a state handler.

3. **Weakest**: register a state handler that watches every new bug
   add on bot-marked target channels, via `switch_event_bind` on the
   FS-internal `CHANNEL_HAS_BUG` event (if such exists; check FS source).
   If a bug named "eavesdrop" appears on a target channel whose owner
   has `osw_eavesdrop_policy=deny`, hang up the bug's session.

The current spec's Layer-2 plan **must** be replaced. Update
`security-and-eavesdrop.md` §"Layer 2" and §"Implementation".

Also: §"Testing → `eavesdrop_bypass_attempt_var_removed`" describes a
real bypass. The spec's note "actually no, we won't detect; document
this gap" admits the variable-removal attack works — but the larger
issue (C-3) is that the variable was never present to begin with.

---

### CRITICAL C-4 — Tier 1 backpressure can stall FS signaling

**Location**: `event-tiers.md` §"Tier 1 — Critical" lines 22-70,
particularly the back-pressure policy at lines 48-64, plus
`architecture.md` §"Failure modes" line 360.

**Claim in spec**:

> Tier 1 ring fills (4096 envelopes). ... Stop accepting new events
> into the ring; `switch_event_bind` callback BLOCKS (this back-
> pressures the FS event thread).
> ...
> "This is intentional. Blocking FS event delivery is bad, but losing
> a billing event is worse."

**Reality**:

FreeSWITCH's event facility runs in a small pool of dispatch threads
(default 1, configurable via `event-dispatch-threads` in `switch.conf.xml`).
Per FS docs, the dispatch thread invokes registered subscribers'
callbacks **synchronously and serially**. A subscriber that blocks for
seconds blocks the next event for all subscribers, including FS-internal
state machinery.

In a contact-center deployment, the event facility carries:

- `CHANNEL_*` events used by `mod_sofia` to track SIP dialog state.
- `RECORD_*` events used by `mod_recording` and CDR generation.
- `BACKGROUND_JOB` completions used by `bgapi`.
- Custom events from other modules.

If our `event_bind` callback blocks for "a few minutes" while Redis is
down, two things happen:

1. FS event facility queue grows. FS has internal backpressure (queue
   discards or process-level slowdown depending on version).
2. Other subscribers (including FS-internal) get starved. SIP signaling
   state-machine transitions that depend on event-bus delivery STALL.

The spec acknowledges this as "intentional" but does not quantify the
operator impact. At 5000 active SIP sessions:

- Every 5 seconds, ~5000 `SESSION_HEARTBEAT` events fire (Tier 3, but
  they share the same dispatch thread).
- Every new INVITE triggers ~5-10 events through the bus.
- A stalled dispatch thread means `CHANNEL_CREATE` for new INVITEs
  delays, which means SIP 100-Trying / 180-Ringing may delay → upstream
  retransmits → user-visible call setup failures.

The spec implies "lose billing > stall signaling". For a SaaS contact
center, "stall signaling" means *every new call fails*. That's worse
than losing a billing event for 5% of calls.

**Recommended fix**:

Replace the Tier-1 backpressure policy with one of:

1. **Spill to local disk** (recommended): when the Tier-1 ring is
   approaching full, spill to a local file (one file per minute,
   rotation). Drain to Redis when it's back. Bounded local disk
   buffer (configurable, default 1 GB). When local disk is full,
   THEN block — but the operator has been alerted for hours by then.
2. **Drop with a loud alarm**: lose at most N events under sustained
   Redis outage; surface via Health, audit event (sent via local file
   spool when Redis is down), and operator alert.
3. **Two-stage backpressure**: tolerable backpressure (50 ms blocking
   per event) up to ring-X-percent-full, then hard drop.

Whatever the choice, the spec should NOT say "block FS event thread
for minutes". That's a guaranteed outage during the next Redis
incident, and Redis incidents happen.

Also update `architecture.md` §"Failure modes" Redis-down row
accordingly.

---

### CRITICAL C-5 — Originate timeout + idempotency TTL race

**Location**: `control-api/spec.md` §"Originate" lines 124-196,
`OriginateRequest.timeout` (default 60s) and `idempotency_ttl_seconds`
(default 300s).

**Scenario**:

1. Client calls `Originate(request_id=R, timeout=60s)` at t=0.
2. Module enters idempotency cache at t=0 with `(tenant, R) → in-flight`.
3. `switch_ivr_originate` blocks the gRPC thread.
4. At t=60s, the originate is still in progress (slow PSTN).
5. gRPC deadline fires; client sees DEADLINE_EXCEEDED.
6. Client retries with the SAME `request_id` at t=60.5s.
7. Module hits the idempotency cache. What does it return?

The spec at lines 78-95 says:

> Repeat call within TTL with same `(tenant, request_id)` AND same
> method | Return cached response WITHOUT re-execution.

But there's no cached *response* yet — the cache contains a sentinel
"in-flight". The spec doesn't address this case. Three possible
implementations:

A. Cache is populated only on completion. The retry at t=60.5s misses
   the cache → re-executes Originate → **double-dial**. The customer
   gets two calls.
B. Cache stores in-flight markers. The retry waits for the original
   to complete → blocks for 60s more (now well past the retry's own
   deadline) → both fail.
C. Cache returns `ALREADY_EXISTS` for in-flight. The retry sees
   `ALREADY_EXISTS` (which per the spec means "different method";
   semantically confusing).

The spec does not say which.

Additionally: idempotency cache is wiped on module restart. Spec
acknowledges "false-negative window after a module restart". If FS
restarts mid-call and the orchestrator retries within 300s, the
original originate may still be in flight in FS (the module reload
doesn't kill active channels, depending on the FS restart mode) →
again double-dial.

**Recommended fix**:

1. Specify the in-flight semantics. Recommended:
   - Cache stores in-flight markers from the moment the request
     starts.
   - A retry that hits an in-flight marker is blocked on a condition
     variable until the original completes or a "shadow timeout"
     (configurable, default = `idempotency_ttl_seconds`) passes.
   - On shadow-timeout, the retry returns `ALREADY_EXISTS` with a
     clear message "Originate in flight; await original or use new
     request_id".
2. Document the restart false-negative gap: if the operator runs
   module reload, all in-flight markers are gone, but FS channels
   may persist (depending on reload semantics). Persist the cache
   to local disk if reload is supported — or refuse reloads while
   any in-flight Originate exists.

---

### CRITICAL C-6 — Control proto missing import for events.proto

**Location**: `proto/open_switch/control/v1/control.proto` lines 19-22
and 61-63 and 301-302.

**Claim**:

Line 62-63:

```protobuf
rpc SubscribeEvents(SubscribeEventsRequest)
    returns (stream open_switch.events.v1.EventEnvelope);
```

Line 301-302 (end of file):

```protobuf
// Forward declaration — the actual EventEnvelope is in events/v1/events.proto.
// Import handled by buf.gen.
```

**Reality**:

`buf.gen.yaml` does not generate imports. It generates output code for
existing imports. Protobuf reference resolution at compile time
requires `import "open_switch/events/v1/events.proto";` at the top of
the file using a fully-qualified type. Without it, `buf lint` and
protoc both fail with:

```
control.proto:62:14: "open_switch.events.v1.EventEnvelope" is not defined
```

The CI workflow `proto-lint` step uses `bufbuild/buf-action@v1` with
`lint: true`. This will fail.

**Recommended fix**:

Add at line 22 (after the other imports):

```protobuf
import "open_switch/events/v1/events.proto";
```

Remove the "forward declaration" comment at the bottom.

Verify by running `buf lint --path proto` locally.

---

### CRITICAL C-7 — Execute allows `transfer` which bypasses context ACL

**Location**: `control-api/spec.md` §"Execute" lines 281-307.

**Claim**:

> Default tenant allowed apps: `answer`, `set`, `playback`,
> `record_session`, `bridge`, `transfer`, `hangup`, `sleep`.
> ...
> Why we restrict apps: arbitrary `Execute` is full RCE on FS via the
> `system` or `bgapi` apps. Default allowlist excludes those.

**Reality**:

`transfer` (FS dialplan app) takes args `<extension> [<dialplan>]
[<context>]`. The third arg is a destination context. An attacker with
gRPC `Execute(app=transfer, args="666 XML internal")` transfers the
channel into context `internal`, regardless of what
`allowed_contexts` for the tenant lists.

The tenant ACL check on `Originate` validates the target context
against `allowed_contexts`, but `Execute` does NOT — the spec
(line 295-296) says "tenant must own channel; the app name must be in
the tenant's allowed-app list". No check on `transfer`'s context
argument.

Once the channel is transferred to a privileged context, the dialplan
in that context can do anything (execute `system`, `lua`, `playback`
arbitrary files, originate to expensive destinations, etc.).

**Recommended fix**:

1. Remove `transfer` from the default allow-app list, OR
2. Add a special handler for `transfer` that parses the args and
   re-validates the destination context against the tenant's
   `allowed_contexts`, rejecting if mismatch.
3. Same for `bridge` (similar concern: `bridge user/100@trusted-context`
   bypasses outbound ACL via `bridge_pre_execute_app` etc.).

Document in `security-and-eavesdrop.md` §"Threat model" that
context-targeting apps require the same ACL check as `Originate`.

---

### CRITICAL C-8 — Dockerfile.runner inconsistencies

**Location**: `deploy/docker/Dockerfile.runner` lines 16-49, and
`deploy/docker/compose.yaml.example` `cap_add: SYS_NICE`.

Three issues:

1. **`osw-builder` stage is declared but never used**: line 16 says
   `FROM open-switch/builder:${BUILDER_TAG} AS osw-builder` but the
   `COPY --from=osw-builder` calls at lines 32-33 reference it. The
   builder stage will be re-invoked, double-building. Not a correctness
   bug per se but a slow CI bug.

2. **`USER freeswitch` + `cap_add: SYS_NICE` requires file capabilities
   on the FS binary**: `SYS_NICE` is granted to the container's root
   user (and to file-cap-enabled binaries). The `freeswitch` user
   inside the container does not inherit `SYS_NICE` from the container
   capability set unless the FS binary has the file capability
   (`setcap cap_sys_nice+ep /usr/local/bin/freeswitch`). The runner
   Dockerfile does not set this. Result: when FS tries to bump its
   real-time priority (mod_sofia, media engine), it gets EPERM and
   falls back to non-realtime scheduling. Audio jitter follows.

3. **The runner's `apt-get install libhiredis1.1.0`** is hard-coded to
   the trixie minor version. The comment says "package name varies
   between trixie minor versions" — true, and the `|| true` fallback
   means the install can SILENTLY succeed without installing the
   library. Then `ldconfig` (line 33) succeeds against nothing. The
   first `mod_open_switch.so` load fails with "libhiredis.so.1.1.0:
   cannot open shared object". CI builds happen in a fresh trixie tag
   so this won't surface in CI.

**Recommended fix**:

1. For (1): if `osw-builder` is needed only for layer caching, hoist
   the `COPY --from=osw-builder` to use `--from=osw-builder` alias
   that targets a pre-built tag. If the goal is "single Dockerfile
   that builds and runs", use a multi-stage with the builder stage in
   the same Dockerfile. As written, the import is structurally wrong.

2. For (2): add to the runner image:
   ```
   RUN setcap 'cap_sys_nice,cap_net_raw+ep' /usr/local/bin/freeswitch
   ```
   Document in operator hardening that this is the minimum file cap
   set. Verify with `getcap`. Alternative: run the FS process as root
   (worse posture).

3. For (3): pin to a specific trixie minor version in the base image
   tag (e.g., `freeswitch:1.10.12-trixie-20260515`). The `|| true`
   fallback is a footgun. Replace with a verification line:
   ```
   RUN ldconfig && \
       ldd /usr/local/mod/mod_open_switch.so | grep -q 'libhiredis.so' || \
         (echo "FATAL: libhiredis missing"; exit 1)
   ```

---

### IMPORTANT findings (detail)

**I-1 — VOICEBOT_DUPLEX single-stream HOL blocking**:

`media-bridge.md` §"Multi-stream per call: voicebot duplex" describes
one bidirectional gRPC stream carrying both caller's read audio (mic)
and bot's write audio (TTS). The protocol uses `FromModule` /
`FromService` oneof messages. gRPC bidirectional streams are NOT
multiplexed at the HTTP/2 frame level — they're a single ordered byte
stream in each direction. A slow `FromService` reader (the module's
WRITE_REPLACE side) blocks gRPC's flow-control on the server's send
direction. If the server is sending TTS at 50fps and the module's
write-replace ring is full, the server's `Stream` handler blocks on
gRPC `Write`, which means its OTHER work also blocks.

**Recommended**: use two streams (one upstream, one downstream) when
purpose is `VOICEBOT_DUPLEX` to decouple flow control. The "single
stream" savings is one TCP connection × one TLS handshake — measured
in milliseconds per call, not microseconds per frame. Not worth the
HOL risk.

**I-2 — Channel pool idle teardown vs long calls**:

`media-bridge.md` §"gRPC channel pool" — "Channels are torn down on
module unload or when idle for >5 minutes." For a call with an IVR
menu where the bot is silent for 6 minutes (caller listens to a music
playlist), the channel dies and the next TTS burst pays a full TLS
+ HTTP/2 + ALPN round-trip (~50-150ms). For interactive bots, that's
audible silence.

**Recommended**: keep the gRPC channel alive as long as ANY active
bug references it. Tear down only when refcount hits zero AND has been
zero for >5 min.

**I-3 — Silent degradation on TTS stream loss**:

`media-bridge.md` §"Failure modes" says "Stream errors mid-call: tear
down bug; emit Tier-2 `bug_stream_lost`; channel continues without
that bug." For a VOICEBOT_DUPLEX use case, "channel continues without
TTS" means caller hears silence forever. The bot's orchestrator
(external) may not be polling the event bus.

**Recommended**: automatic reconnect attempts (configurable, default 3
attempts, exponential backoff 1-10s) before declaring stream lost.
Emit `bug_stream_reconnecting` events with attempt count. Only emit
`bug_stream_lost` after all attempts fail.

**I-4 — redis-plus-plus auth failure re-auth**:

`transport-adr.md` §"Connection management" says "Reconnect on error
(exponential backoff 100ms → 30s cap)." `redis-plus-plus` auto-
reconnects but does NOT automatically re-AUTH after reconnect unless
the connection-string carries the password and the library is configured
to send AUTH on every connect (which it is, via `ConnectionOptions::user`
and `password`). However, if Redis rejects AUTH mid-stream (e.g., ACL
key rotation, password change), the reconnect logic will reconnect →
AUTH fail → loop. The spec does not address this.

**Recommended**: explicit re-AUTH on reconnect using config-loaded
credentials; if AUTH fails persistently (>3 attempts), emit Tier 1
`osw::audit::redis_auth_failure` and stop trying for this sink (mark
as `HealthStatus::DEGRADED`).

**I-5 — Tier 3 drop metric self-amplification**:

`event-tiers.md` §"Tier 3 ephemeral" line 145-147 says:

> Drop immediately when ring full. Emit a Tier 1 event
> `osw::tier3_dropped` once per minute summarising the drop count.

If Tier 3 is dropping AT a rate where the "1/min" rate-limiter fails
(e.g., race condition in the rate-limit clock), Tier 1 fills with
`osw::tier3_dropped` events. Tier 1 may have its own backpressure
(see C-4) which then blocks the FS event thread, which then prevents
the rate-limiter from firing again (because the event bind callback
is blocked), which freezes Tier 3 drop counts at the threshold.

**Recommended**: implement the rate-limiter as a hard counter per-
minute resetting via `std::chrono::steady_clock` polled from the
shipper thread, not from a callback path that could be back-pressured.
Document the assumption.

**I-6 — Empty tenant_id collapses cache key**:

`security-and-eavesdrop.md` §"Idempotency security" — "Idempotency
cache is keyed by `(tenant_id, request_id)`. Different tenants get
different cache lines." In proto3, `string tenant_id` has zero-value
`""` if not set. Two attackers from different tenant contexts who both
omit `tenant_id` and both happen to use the same `request_id` UUIDv7
collide — both retries return the FIRST one's cached response.

**Recommended**: reject requests where `tenant_id` is empty with
`UNAUTHENTICATED` (or `INVALID_ARGUMENT` if the operator chose a
no-auth mode). Update `control-api/spec.md` §"RequestHeader" to make
`tenant_id` strictly required.

**I-7 — SetVariables reserved-prefix denylist is incomplete**:

`control-api/spec.md` §"SetVariables" lines 311-331. Reserved
prefixes: `osw_*`, `eavesdrop_*`, `record_*`.

Missing prefixes that affect security or recording:

- `record_*` is listed; check it covers all variants. FS has
  `RECORD_STEREO`, `RECORD_BRIDGE_REQ`, `recording_follow_transfer`,
  etc. The pattern matches if case-insensitive — verify.
- `playback_*` controls file playback paths (could play attacker-
  controlled file from `/etc/passwd`).
- `bridge_pre_execute_aleg_app` / `bridge_pre_execute_bleg_app`
  execute apps on bridge — RCE via app injection.
- `socket_*`, `audio_stream_*` — control external streaming.
- `hold_music`, `hold_music_b` — let attacker substitute hold music
  with arbitrary URL.
- `transfer_*` — control transfer behavior.
- `originate_*` — affect outbound dial.
- `endpoint_disposition` — affect routing decisions.
- `verto_*`, `sip_h_*` — affect signaling headers (SIP injection).

A prefix-denylist is the wrong approach. Switch to a per-variable
*allowlist* of safe-to-set variables, OR maintain a curated denylist
that an FS specialist reviews quarterly.

**I-8 — HangupMany order**:

`control-api/spec.md` §"HangupMany" lines 228-256. "Module processes
in batches" — order? Caller passes 1000 UUIDs; module processes
serial; deadline fires after 500. Response contains `hungup_uuids`
list of 500. Are those the FIRST 500 from `uuids`, or some scrambled
batch?

**Recommended**: specify FIFO processing (input order). Caller resumes
by taking `uuids[len(hungup_uuids):]` for the retry. Document this.

**I-9 — Bug callback exception handling per ABC type**:

`memory-management.md` §"Exception-safety boundary" says C-callable
entry points wrap in try/catch and return `SWITCH_STATUS_GENERR` or
the bug-callback equivalent `SWITCH_FALSE`. For media bug callbacks
(`switch_bool_t` return), FS interprets `SWITCH_FALSE` as "this bug
should be terminated" (the bug remover path runs). Returning
`SWITCH_FALSE` from `SWITCH_ABC_TYPE_INIT` aborts the bug attach.
Returning `SWITCH_FALSE` from `_READ` or `_WRITE` removes the bug
mid-call.

The spec's media-bug exception handler returns `SWITCH_FALSE` for
every type, which is correct (we want to remove a broken bug). But
the spec should clarify the operator-visible effect: a single
exception triggers bug removal, which means cancellation propagation
(C-3 path 2 in media-bridge.md). Document this clearly.

Also: returning `SWITCH_FALSE` on `SWITCH_ABC_TYPE_INIT` aborts the
add but does not call the destructor. The `MediaBugLease` RAII guard
should be fully constructed BEFORE the INIT callback fires — verify
this is the FS sequence.

**I-10 — seq allocation thread**:

`event-tiers.md` §"Sequencing guarantees" says `seq` is strictly
increasing per `(node_id, tier)`. The spec doesn't specify where seq
is allocated:

- If allocated on the `event_bind` callback (FS thread) BEFORE
  enqueue to the lock-free SPSC ring, then `seq` is monotonic at
  enqueue time. The shipper thread sees them in order.
- If allocated on the shipper thread at dequeue time, the seq is
  monotonic at SEND time. But if the ring has a logical race where
  events from different FS dispatch threads (yes, FS may have multiple
  dispatch threads) interleave, seq may not match the actual emission
  order.

**Recommended**: allocate `seq` at the event_bind callback via a
per-tier `std::atomic<uint64_t>::fetch_add(1, memory_order_relaxed)`.
Document this. It's a 1-2 ns op; not a hot-path concern.

**I-11 — SIGSEGV handler scheme**:

`architecture.md` §"Failure modes" — "We elect to ABORT the FS process
on module-internal SIGSEGV (set up a custom handler that aborts to
trigger container restart)."

A SIGSEGV handler running in the broken process is fundamentally
unreliable: heap may be corrupted; calling printf may crash; raising
SIGABRT is fine but only if the current stack is unwindable. FreeSWITCH
itself may install its own signal handlers; ordering of installation
matters.

**Recommended**: don't install a SIGSEGV handler in this module. FS
will install its own; let it run. For C++ unhandled exceptions, call
`std::set_terminate([]{ std::abort(); })` at module load. For graceful
poisoning detection, use AddressSanitizer or compile-time hardening
(stack protector, control-flow integrity).

**I-12 — gRPC/Protobuf ABI**:

The builder builds gRPC v1.74.0 from source against gRPC's bundled
abseil + protobuf. The runner copies `/dist/lib/*` (`libprotobuf.so`,
`libgrpc++.so`, `libabsl_*.so`) from builder and `ldconfig`s. FS
itself doesn't link protobuf (verified — FS is C, no proto in core).
So no direct conflict.

**HOWEVER**: if another FS module is loaded (e.g., `mod_grpc` from
webitel/freeswitch-mod-grpc, which is mentioned in the README
acknowledgements), and IT links a different gRPC version, both `.so`
files get loaded into the same process. Symbol resolution depends on
RTLD_LOCAL vs RTLD_GLOBAL of FS's module loader. FS uses RTLD_LOCAL
(per `switch_loadable_module.c`), so each .so has its own namespace.
With `-fvisibility=hidden` + `-Wl,--exclude-libs,ALL`, our gRPC
symbols don't leak. Should be OK but the `--exclude-libs` flag is
mentioned in `proposal.md` but not actually in `CMakeLists.txt`.

**Recommended**: add to CMake link flags:
```cmake
target_link_options(mod_open_switch PRIVATE
    "-Wl,--exclude-libs,ALL"
    "-Wl,-Bsymbolic"
    "-Wl,-Bsymbolic-functions"
)
```

Verify with `nm -D mod_open_switch.so | grep grpc` showing only the
module's own symbols, not gRPC internals.

**I-13 — Graceful drain Redis flush**:

`architecture.md` §"Graceful drain" step 8 has the open question:

> Flushes Redis sink (XADD with WAIT? — see transport-adr.md).

`WAIT N` in Redis blocks until N replicas ack. Not appropriate at
drain — could block forever if replicas are slow. Use synchronous
`XADD` (default redis-plus-plus sync mode) with a per-event timeout
of e.g., 500ms. Drain has a total timeout (`drain_timeout_seconds`
= 30); each event can take up to that minus elapsed.

**Recommended**: specify in `transport-adr.md`:
- Drain mode uses synchronous XADD (no pipelining).
- Per-event timeout = `drain_timeout_seconds * 0.5 / pending_count`.
- If timeout hits: log Tier 1 event indicating residual events
  spilled to local file (see C-4 fix).

**I-15 — Stereo pairer cross-thread misleading**:

`recording-with-bot.md` §"Sync of L and R for stereo relay" describes
a `StereoFramePairer` with timestamp pairing, 5ms tolerance, 25ms
fill timeout. The spec implies thread coordination.

**Reality**: per FS internals, read and write callbacks for a single
session fire on the SAME media thread (one channel ↔ one media thread
in 1.10.x's standard SCHED_RR scheduler). They alternate, not
concurrent. Pairer logic can be lock-free (single-threaded).

**Recommended**: clarify in spec "Pairer state is single-thread per
channel; no mutex required. The 5ms/25ms thresholds are for catching
drift in case FS interleaves frame timestamps unexpectedly across
codec re-negotiation events."

**I-16 — Integration test directory empty makes CI a false signal**:

`tests/integration/` contains only `.gitkeep`. CI workflow `ci.yml`
runs:

```
ctest --test-dir /usr/src/open-switch/build --output-on-failure -L integration
```

If no tests carry the `integration` label, ctest reports "no tests
match" and the exit code is `0`. The pipeline turns green. ASAN/LSAN
have nothing to detect leaks in.

**Recommended**: add a CI guard:

```yaml
- name: Assert integration test suite is non-empty
  run: |
    docker run --rm open-switch/builder:ci-${{ github.sha }} bash -c '
      cd /usr/src/open-switch/build
      n=$(ctest -L integration -N | grep "Test #" | wc -l)
      if [ "$n" -lt 5 ]; then
        echo "FATAL: integration suite has $n tests (expected ≥5)"
        exit 1
      fi
    '
```

The "expected ≥5" threshold rises as Phase 2 lands real tests.

---

## Cross-spec inconsistencies

1. **Bug-priority numbers conflict (related to C-1)**: `media-bridge.md`
   says STT is priority 200; `recording-with-bot.md` repeats this;
   `call-transcribe.md` says "Attaches a `SMBF_READ_STREAM` bug at
   priority 200". All wrong (no priority in FS). Fix coherently.

2. **`record_session` priority**: `recording-with-bot.md` says FS
   default lands "near 700-750". `media-bridge.md` says "Priority
   700: Recording (read tap), 750: Recording (write tap)" by *our*
   allocator. The two priorities (FS-native vs our-allocator-output)
   are reconciled implicitly. Per C-1/C-2, both are fiction; remove.

3. **Eavesdrop policy 'audit' tier**: `security-and-eavesdrop.md`
   line 180 says `audit` events are Tier 1; line 181 says `allow`
   events are Tier 2. But the audit event for `allow` is *the same
   subclass* (`osw::eavesdrop::allowed`). Tier classification depends
   on the policy-applied field, not on the subclass. The event
   routing config (XML) routes by subclass, not by field. So an
   `osw::eavesdrop::allowed` event will route to whatever tier the
   subclass matches → if the operator's config routes all
   `osw::eavesdrop::*` to Tier 1, then `allow` events also go Tier 1.

   **Fix**: split into three distinct subclasses
   (`osw::eavesdrop::denied`, `osw::eavesdrop::audit`,
   `osw::eavesdrop::allowed`) and explicitly route them to different
   tiers in the default `open_switch.conf.xml.sample`.

4. **`Originate` `timeout` default**: proto line 142-143 comment says
   "If zero, defaults to 60s". `control-api/spec.md` §"Originate"
   line 130-132 says default 60s. `proposal.md` Risk 5 says 60s.
   `architecture.md` §"Sequence" line 290 says "switch_ivr_originate
   (blocks gRPC thread for up to 60s)". Consistent. But:
   `architecture.md` §"Control plane" line 88-92 says "gRPC thread
   pool to be at least 2 × the max concurrent Originate rate". For
   60s × 100 RPS = 6000 in flight; thread pool = 12000 threads. Not
   realistic. Either Originate should be async (return job_id) or
   the gRPC thread pool sizing needs to be much smaller and the
   docs need to reflect that.

5. **`stream_id` opacity**: `control-api/spec.md` §"Media-control
   RPCs" line 386 says `stream_id` is opaque to the caller and used
   for the Stop RPC. But the same paragraph says "the module also
   tracks streams by `(channel_uuid, purpose)` so callers can Stop
   without the stream_id if they have those." Which is the contract?
   What if two `StartTts` are issued for the same channel (theoretically
   possible if the operator allows multiple TTS bugs)? Spec says
   `media-bridge.md` rejects priority collision, but with C-1 ruling
   priority out, there's no rejection. Specify uniqueness by
   `(channel_uuid, purpose)`.

6. **Compose YAML caps vs Dockerfile USER**: noted in C-8.

7. **gRPC v1.74 vs v1.69 README mismatch**: README line 64 says
   "Target: gRPC v1.69.x". `Dockerfile.builder` line 21 builds
   `GRPC_VERSION=v1.74.0`. Update README.

---

## Things the design got right

These are good and should NOT be changed during the revision pass:

- **AGPL-3.0 posture** — explicit, clear, no §13 loophole. README
  section "Relationship to open-tts" is good.
- **RAII helper set** — `SessionLock`, `EventGuard`, `MediaBugLease`,
  `XmlNode` are correctly written. Move semantics are right; double-
  free is prevented; destructor is `noexcept`.
- **Memory-management discipline** — ASAN+LSAN per PR, Valgrind
  nightly, clang-tidy gates, PR template checklist, code review
  required. This is genuinely thorough.
- **Three-tier event taxonomy** — Tier 1/Tier 2/Tier 3 categorization
  is sensible. The "if lost, what breaks?" lens is good.
- **Transport ADR rigor** — option matrix with weighted scoring,
  explicit owner alignment row, pluggable interface from day one.
  The decision is defensible; the interface lets future Kafka land
  without an ABI break.
- **mTLS + per-tenant API key** — layered auth, idempotency keyed on
  tenant, ACL on dial-context.
- **gRPC reactor preferred over CQ** for new services — modern, less
  error-prone.
- **Schema-evolution policy** — reserved tags, never reuse, schema
  version field. Standard practice, explicit here.
- **Hardening checklist for operators** — concrete, actionable.
- **`switch_core_session_strdup` for session-scoped data** — correct
  use of FS pools for lifetime-matching data.
- **Test plans per design doc** — every doc has a test plan section
  with named tests. Not all of these will be cheap to write but the
  thinking is there.

---

## Recommendations beyond the spec

1. **Add a "FreeSWITCH facts sheet"** to the design docs that
   documents FS internals the design depends on, with source-file
   citations. Examples:
   - Bug chain is single-linked, append-on-add, `SMBF_FIRST` heads.
   - Event dispatch threads default to 1; sub callbacks serialise.
   - `switch_ivr_originate` blocks the calling thread.
   - `switch_channel_get_variable` returns a pointer with channel
     lifetime; copy before storing.
   - Media bug callbacks fire on the media thread; read and write
     callbacks for one session interleave on the same thread.
   - FS module loader uses RTLD_LOCAL.

   This sheet prevents the three blocker findings here from
   recurring. If a Phase 2 implementation diverges from this sheet,
   review immediately. Maintain alongside the FS version in use.

2. **Add a "this design depends on FS source observed at hash X"**
   note to each design doc. When you bump FS version (e.g., 1.10.12
   → 1.10.13), re-verify the assumptions. FreeSWITCH has been known
   to silently change media-bug ordering between minor versions.

3. **Test against real FS, not a mock**. Phase 2 integration tests
   MUST link against the real FS shared lib and call into the real
   `switch_core_media_bug_add`. Mocks here will hide C-1 / C-2-class
   bugs.

4. **Set up a recurring "spec ↔ FS source" drift CI**. A nightly job
   that grep's the spec for FS function names and verifies the
   functions exist in the linked FS source with the documented
   signature. Cheap to write; catches version drift.

5. **For the bot eavesdrop case (C-3)**, consider implementing the
   policy at the `switch_ivr_eavesdrop_session` ENTRY POINT by
   wrapping it: register a new dialplan app `osw_eavesdrop` that
   does the policy check then delegates to `switch_ivr_eavesdrop_session`.
   Deprecate `eavesdrop` for tenants that have any bot-marked
   channels. This is cleaner than chasing FS internals.

6. **Document gRPC's thread model** in `architecture.md`. gRPC C++
   sync server spawns a thread per RPC by default. With 100 RPS of
   Originate at 60s each = 6000 threads, each with 8MB stack default
   = 48GB virt. The system will swap. Phase 2 must use the async
   server or aggressive thread-pool sizing. Spec needs concrete
   numbers.

7. **Replace `dlopen` boundary virtuals with C-API plug-in**: the
   `EventSink` virtual interface is intra-module (no dlopen needed),
   so C++ vtables work fine. But if you ever want operators to drop
   in their own `EventSink` `.so`, you'll need a C ABI. Document
   that V1 plug-ins are compile-time, not runtime.

8. **Run the Phase 1 review through one more pair of eyes** —
   specifically someone who has shipped a FreeSWITCH module to
   production. Many of the findings here are "I checked FS source
   and it said X". An FS production engineer would catch these
   reflexively. Don't accept Phase 1 sign-off until that review
   happens.

---

## Closing

Phase 1 has done excellent work on the broad shape: planes, RAII,
license, tier taxonomy. But it shipped three claims about FreeSWITCH
internals that are not true, and they're load-bearing. C-1 / C-2 /
C-3 must be fixed at the design level before Phase 2 starts; the
critical findings (C-4 .. C-8) before the first implementation
commit. The remaining important findings are well-suited to the
"Codex checkpoint" tasks at the bottom of `tasks.md`.

The W5 review missed a "writing audio to a control socket" bug; this
review's analogue is "ordering bugs by a priority that doesn't
exist". Both are the same failure mode: trusting the spec's mental
model of the underlying system without verifying against the system.
The fact that `tests/integration/` is empty (I-16) means CI cannot
catch this class of bug. That's the highest-leverage fix in Phase 2:
real FS integration tests that exercise the bug-chain order, the
eavesdrop hook, and the recording-with-bot scenario end-to-end.

I'm not satisfied. Revise and re-review.

Reviewer: Codex (gpt-5.5)
