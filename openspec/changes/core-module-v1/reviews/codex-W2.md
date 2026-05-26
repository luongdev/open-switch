# Codex W2 Events review — code + docs

**Date**: 2026-05-26
**Reviewer**: Codex (gpt-5.5)
**Branch reviewed**: `implementation/wave2-events` (HEAD `f696174`)
**Diff vs base `9bdf784` (W1 fix-sprint tip)**: ~30 files / ~+5000 lines / 14 commits
**Prior context**: W1 review (`reviews/codex-W1.md`) caught 3 BLOCKERS + 4 CRITICAL.
Same FACT discipline applies; every new `switch_*` call must cite an FF entry.

---

## Verdict

**NOT READY — 2 BLOCKERS, 3 CRITICALS, 6 IMPORTANTS, 5 NITS.**

The three new FACT entries (FF-018, FF-019, FF-020) **all verify cleanly
at v1.10.12** — the discipline that drove W1's pre-impl rounds held.
The ring/binder/broadcaster/send-queue core is well-architected: lock
order is documented and structurally enforced (`WaitAndPopBatch`
returns a vector before any subscriber-side work; `RosterSnapshot`
copies shared_ptrs before any SendQueue::TryPush), shared_ptr
zero-copy across subscribers is correct, FF-018 unbind-under-wrlock is
used correctly to make `Binder::Stop()` safe against concurrent
dispatch, and the slow-vs-fast subscriber non-interference test
asserts the contract.

The blockers are **correctness bugs in the SubscribeEvents handler**:
(B-1) a replay→live-tail seq gap race that violates the Tier-1 "must
persist" contract and the proto's "replay then continue with live
tail" promise; (B-2) a documented-vs-actual mismatch on the
`module_shutdown_with_pending_events` audit — the closeout claims it
reaches subscribers but the code unbinds the producer first, so the
audit is dead-lettered for our gRPC subscribers. Both can be fixed
without redesigning the wave; both must be fixed before merge because
they affect the contract operators see.

---

## FACT verification — FF-018, FF-019, FF-020

Fetched `signalwire/freeswitch` v1.10.12 raw source for every excerpt.

| FF | Claim | v1.10.12 source check | Verdict |
|---|---|---|---|
| **FF-018** | `switch_event_bind` callback signature + lifecycle + unbind-under-wrlock | `src/include/switch_types.h:2477` typedef `void(*switch_event_callback_t)(switch_event_t*)` matches verbatim. `src/switch_event.c:2060-2131` (`switch_event_bind_removable` + thin wrapper) matches verbatim — RWLOCK wrlock + EVENT_NODES[event] LIFO insert + auto-reserve subclass at lines 2073-2090. `switch_event_deliver` (`src/switch_event.c:400-422`) takes RWLOCK rdlock, walks nodes, calls `node->callback(*event)` with a single pointer (matches typedef), then `switch_event_destroy(event)` at line 422. `switch_event_unbind_callback` (`src/switch_event.c:2134-2171`) takes wrlock + frees `n`. All implications correctly stated: callback is borrowed, lifetime ≤ callback return, multi-threaded per FF-004, unbind-after-wrlock guarantees no further dispatch. | **VERIFIED** |
| **FF-019** | `switch_event_get_header` returns FS-pool-owned `char*`; lifetime ≤ event | Macro at `src/include/switch_event.h:172` expands to `switch_event_get_header_idx(_e, _h, -1)` (verbatim match). `switch_event_get_header_idx` at `src/switch_event.c:846-864` returns `hp->value` from the matching header node, or `event->body` for the magic `"_body"` lookup, or NULL. Implications correctly state: caller MUST NOT free, MUST copy synchronously, NULL means absent. | **VERIFIED** |
| **FF-020** | `switch_event_create_subclass` requires `SWITCH_EVENT_CUSTOM` (or CLONE) + non-NULL subclass; adds `Event-Subclass` header | `src/switch_event.c:747-787` (`switch_event_create_subclass_detailed`) matches verbatim. Line 756 enforces the type guard (`event_id != CLONE && != CUSTOM && subclass_name → GENERR`). Line 783 adds `Event-Subclass` via `switch_event_add_header_string`. The auto-reservation-on-bind path at `src/switch_event.c:2073-2090` was independently verified and matches the FF-018 excerpt; therefore the closeout's claim that "we bind to SWITCH_EVENT_ALL so reservation is unnecessary for our own receive path" is correct. | **VERIFIED** |

**No fabricated FS semantics this wave.** The four-round pre-impl
exercise paid off again.

---

## BLOCKERS — must close before merge

### B-1 — Replay→live-tail seq GAP race in `SubscribeEvents` handler

`src/control/handlers/subscribe_events_handler.cc:337-345`:

```cpp
if (req->since_seq() > 0) {
    if (!ReplaySinceSeq(rings_, fr.replay_tiers, req->since_seq(), *sub)) {
        return KickReasonToStatus(sub->GetKickReason(), "during since_seq replay");
    }
}
// Add to roster (live-tail flow now reaches the subscriber).
broadcaster_->AddSubscriber(sub);
```

`ReplaySinceSeq` calls `Ring::SnapshotFromSeq` (non-destructive: copies
`shared_ptr<const string>` entries currently in the ring with
`seq > since_seq`). The snapshot returns entries `[A+1 .. B]` where `B
= current_max_seq` at snapshot time. The handler then pushes those
into the subscriber's SendQueue.

Between the snapshot and `AddSubscriber`, the broadcaster's per-tier
worker is still running. It can:
1. Pop entries from the ring (entries that were ALSO in the snapshot
   — no problem; the subscriber gets them via the manual replay
   push).
2. Pop NEW entries with `seq > B` that landed AFTER the snapshot
   (the broadcaster removes them from the ring and dispatches to
   existing subscribers).

Those new entries `[B+1 .. C]` (where `C` is the highest seq the
broadcaster popped before `AddSubscriber` ran) are **not** in the
snapshot (snapshot's max was `B`) and they are **not** in the ring
anymore (broadcaster already popped them). The newly-registered
subscriber starts receiving from the NEXT live event, `C+1`. Seqs
`B+1..C` are silently lost from that subscriber's stream.

Window: between `Ring::SnapshotFromSeq` (line 215 of handler) and
`AddSubscriber` (line 345). The handler also iterates `replay_tiers`
and pushes every entry into the queue — at high event rate this
window can easily span microseconds-to-milliseconds.

This contradicts:
- `proto/open_switch/control/v1/control.proto:304-308`:
  > replays from since_seq+1 to ring tail if still in the replay ring,
  > then continues with live tail.
- `designs/event-tiers.md:304`:
  > Replay ring from 1004 to ring tail, then continue live
- Tier-1 "must persist" semantics — losing a `CHANNEL_HANGUP_COMPLETE`
  or `CDR_REPORT` event from a billing subscriber is the failure mode
  this whole tier exists to prevent.

**Fix.** Take the snapshot's `current_max_seq` (already available in
`Ring::ReplaySnapshot.current_max_seq`). Pass it back from
`ReplaySinceSeq` to the caller. After `AddSubscriber`, take a second
snapshot with `since_seq = first_max_seq`, push any entries from that
gap window into the SendQueue. Live tail still flows for events
beyond the second snapshot's max. Worst case the subscriber sees a
duplicate if the broadcaster also dispatched to the new subscriber
between snapshots — clients can dedup on `seq` per (node, tier) which
the proto already establishes as the contract for resumption.

Alternative fix: hold `broadcaster_->roster_mu_` during the
snapshot+AddSubscriber pair atomically (would require a new API on
the broadcaster like `AtomicSnapshotAndAdd`). More invasive but
eliminates the race without dedup.

### B-2 — `module_shutdown_with_pending_events` audit is dead-lettered for gRPC subscribers, contradicting closeout doc + code comment

`src/core/module.cc:267-297` (the relevant span):

```cpp
// 2. Stop the producer side first.
if (binder_) {
    binder_->Stop();                          // <-- unbinds osw_event_handler
}
// 3. Wait up to event_drain_timeout_seconds for the rings to empty
//    ... if pending:
osw::audit::Emit("module_shutdown_with_pending_events", ...);
// 4. Stop the broadcaster.
if (broadcaster_) {
    broadcaster_->Stop();
}
```

And the code comment at lines 286-289:

```cpp
// Best-effort audit emit: the binder is stopped so this
// CUSTOM event will NOT re-enter our pipeline; it still
// reaches any subscriber currently connected via the
// broadcaster (which is still running at this point).
```

`W2-events.md` known-gap #3 makes the same claim:
> emitted between Binder::Stop and Broadcaster::Stop, so the audit
> event reaches live subscribers (the broadcaster is still draining
> its rings)

**This is wrong.** `osw::audit::Emit` calls `switch_event_fire` (via
`EventGuard::fire()`) which hands the event to FS's dispatch facility.
FS's dispatcher calls `switch_event_deliver` which walks
`EVENT_NODES[]` and invokes registered callbacks. Our
`osw_event_handler` is the **only** path from FS into our rings —
and it was unbound by `Binder::Stop()` two steps earlier (FF-018
wrlock guaranteed no further dispatch by the time Stop returned).
The audit event therefore does not enter our ring. The broadcaster
has nothing new to dispatch. **Our gRPC subscribers receive nothing.**

The audit does still reach FS-native log subscribers (mod_logfile,
mod_console, an event_socket listener) via FS's own dispatch — but
**not** our SubscribeEvents subscribers. The purpose of this audit
(operators monitoring drain timeouts via the events stream) is
defeated.

Same failure mode as W1 B-3 (doc-vs-code mismatch on the very thing
the doc highlights). Doubly important here because the W2 contract
exposes `osw.audit.*` as a first-class tier-1 channel that
operators are expected to subscribe to.

**Fix options** (pick one and update doc):

a. **Move the audit emit BEFORE `Binder::Stop()`** — the audit then
   enters the ring via our own binder, lands in tier-1 (per the
   classifier), and the still-running broadcaster delivers it to
   subscribers. New events from FS could still race in, but that's
   the existing semantics. This is the lowest-friction fix and matches
   the documented intent.

b. **Add a `Broadcaster::InjectSynthetic(Tier, envelope_bytes)`
   direct-inject API** — bypasses the ring + binder, pushes directly
   into per-subscriber queues from the module's Shutdown context.
   More invasive; preferred only if the audit is for events that
   logically MUST be tier-1 marked even if the binder is gone.

c. **Document honestly**: "emitted to FS log subscribers; gRPC
   subscribers do NOT receive `module_shutdown_with_pending_events`."
   Strictly worse for operators but at least the doc and code match.

Whichever, the code comment at module.cc:286-289 must be rewritten
to reflect reality, and `W2-events.md` known-gap #3 must be updated.

---

## CRITICAL — must address (fix in W2.5 or merge with TODOs)

### C-1 — Filter narrowing does NOT apply during replay; asymmetric with live tail

`src/control/handlers/subscribe_events_handler.cc:226-258`,
specifically lines 229-242:

```cpp
// We do NOT check the subscriber filter here at the routing-
// fields level — the replay path is rare and a full
// ParseFromString is acceptable. Instead, the writer-loop
// filter is implicit: we push EVERY entry from the requested
// tier into the queue, and the client receives them. If the
// client narrowed event_names/node_id, that filter applies
// to the LIVE tail (the broadcaster filters before push),
// not to the replay window.
```

The agent's gap #1 framed this as a question. The proto comment on
`SubscribeEventsRequest.event_names` says "Filter by event name
pattern (glob). Empty = all events." — no exception for replay. A
client that connects with `event_names=["CHANNEL_HANGUP_COMPLETE"]`,
`since_seq=100` will receive ALL tier-1 events with seq > 100 during
replay (potentially hundreds of `CHANNEL_BRIDGE`, `CDR_REPORT`,
`RECORD_START`, etc.), then only matching events live. This is
surprising and potentially crashes the subscriber queue: if the
client's `send_queue_capacity` is smaller than the ring's
post-`since_seq` slice, replay overflows kick the subscriber with
`kQueueFull` before they ever see a live event (line 243-253).

The handler already extracts routing fields for the broadcaster path
via `ExtractRoutingFields` (broadcaster.cc:154-210). Lift that helper
into a shared header (or a `replay_filter_utils.h` TU) and call it
from `ReplaySinceSeq` before `TryPush`. The replay-path cost claimed
in the comment ("a full ParseFromString is acceptable") is moot
because `ExtractRoutingFields` is a lightweight manual scan, not a
full parse.

### C-2 — Subscriber filter cannot match by `subclass_name`; audit channel is unfilterable in practice

`include/osw/events/subscribe/subscriber.h:71-82` defines
`SubscriberFilter` with `tiers`, `event_name_globs`, `node_id` —
NO `subclass_globs`. In `BuildEnvelope` (envelope.cc:262), the
event_name field is set to the FS `Event-Name` header, which for
CUSTOM events is literally `"CUSTOM"`. The CUSTOM event's identity
lives in the `subclass_name` field. The broadcaster's
`ExtractRoutingFields` doesn't even extract subclass_name (field
tag 4) — only tags 2/3/5.

Consequence: a client that wants to subscribe to `osw.audit.*` events
specifically (the W2-advertised "audit channel") has no way to filter
them out from the other CUSTOM events flowing through the system
(`sofia::register`, `sofia::profile_start`, third-party module
emissions). They either receive all CUSTOM events under
`event_names=["CUSTOM"]` or receive nothing.

The closeout doc emphasizes the audit channel as a first-class
feature (W2 tier-1 routing for `osw.audit.*` via classifier glob).
That feature is half-broken at the wire boundary.

**Fix.** Add `repeated string subclass_globs = 6` to
`SubscribeEventsRequest`, add `subclass_globs` to `SubscriberFilter`,
add field-tag-4 extraction to `ExtractRoutingFields`, and add
subclass matching to `Subscriber::MatchesFilter`. Mirror the
prefix-glob semantics already in place for `event_name_globs` (and
note in the proto that it's prefix-only — see N-1 below).

### C-3 — Plain pointer reads of `broadcaster_` / `rings_` in `SubscribeEvents` handler are a data race (will trip TSAN on strict)

`src/control/control_service_skeleton.h:124-127` declares
`broadcaster_`, `rings_` as plain raw pointers. They are written by
`SetEventPlane` (called from `Module::Load` after `grpc_server_->Start()`)
and read by every `SubscribeEvents` RPC handler:

- subscribe_events_handler.cc:312: `if (broadcaster_ == nullptr)`
- subscribe_events_handler.cc:320: `broadcaster_->SubscriberCount()`
- subscribe_events_handler.cc:338: `ReplaySinceSeq(rings_, ...)`
- subscribe_events_handler.cc:345: `broadcaster_->AddSubscriber(sub)`

Module::Load step 6 (`grpc_server_->Start()`) and step 7
(`grpc_server_->SetEventPlane(...)`) are non-atomic with respect to
RPC arrivals: any SubscribeEvents RPC that wins the race between
those two steps reads `broadcaster_ == nullptr` (the default-init
value from the C++ initializer at line 124) while
`SetEventPlane`-on-another-thread is writing the new value. On
x86-64 aligned 8-byte loads/stores are atomic in practice; the C++
memory model does NOT guarantee that. TSAN strict mode will flag this
on every run.

The fail-soft TSAN policy (closeout §"CI TSAN gate") hides this
finding for now, but flipping to strict (the closeout's stated
post-W2 plan) will fail until this is fixed.

**Fix.** Change the four members in `control_service_skeleton.h:124-127`
to `std::atomic<events::Broadcaster*>` and `std::atomic<events::RingSet*>`
(the integers can stay plain since they're written once during
SetEventPlane and never racing meaningfully with reads). Use
`load(memory_order_acquire)` in the handler, `store(memory_order_release)`
in `SetEventPlane`.

---

## IMPORTANT — should address in W2.5

### I-1 — Unparseable tier string silently fails open (gap #5 confirmed; agent flagged it)

`src/control/handlers/subscribe_events_handler.cc:157-169`:

```cpp
for (const auto& t_str : req.tiers()) {
    if (auto t = ParseTierString(t_str); t.has_value()) {
        out.filter.tiers.insert(*t);
        out.replay_tiers.push_back(*t);
    } else {
        osw::log::Warn(kSubsystem, "ignoring unparseable tier '%s'", t_str.c_str());
    }
}
if (out.replay_tiers.empty()) {
    out.replay_tiers = {Tier::k1Critical, Tier::k2State, Tier::k3Ephemeral};
}
```

A client that asks for only `tiers=["TIER_42_INVALID"]` ends up with
`filter.tiers` empty + `replay_tiers` defaulted to ALL THREE tiers
— fail-open. The client believes they're getting one tier; the
server hands them everything. Severity is medium (subscribers can be
overwhelmed unexpectedly; quotas/billing might be wrong).

**Fix.** Track whether ANY tier strings were supplied; if at least
one was supplied but ALL failed to parse, return `INVALID_ARGUMENT`.
If `req.tiers()` is empty (operator opted in to "all tiers"), keep
the current default. Add a unit test.

### I-2 — Audit emit at successful `Module::Load` is invisible to subscribers (different from B-2 but same root cause class)

`src/core/module.cc:231-234` emits `module_loaded` immediately AFTER
`grpc_server_->SetEventPlane(...)`. At that moment, **zero**
subscribers exist (gRPC was started a few lines earlier; clients
haven't had time to connect). The audit lands in the tier-1 ring and
sits there until evicted. It's logged for FS log subscribers but
the gRPC `osw.audit.module_loaded` is never observed by anyone in
practice.

This is by design for a fresh load. But on a reload sequence (FS
`unload mod_open_switch; load mod_open_switch` while a Tier-1
subscriber is reconnecting with `since_seq` resume), the subscriber
might catch `module_loaded` if their reconnect timing is right and
their since_seq is still in the new ring's window — but those rings
are *new* rings with empty history, so the subscriber's old since_seq
will report evicted (RESOURCE_EXHAUSTED), they reconnect without
since_seq, and they're on live tail from after `module_loaded`.

**Document this** in `event-tiers.md` and the W2 closeout:
"`module_loaded` is fired during Module::Load before any subscriber
can attach; subscribers should treat the absence of `module_loaded`
in their stream as normal." Otherwise downstream may build flaky
"did the module restart" detectors on this signal.

### I-3 — `event_names` glob support is prefix-only, not actual glob (agent's gap #4)

`proto/open_switch/control/v1/control.proto:302-303`:
> Filter by event name pattern (glob). Empty = all events.

`src/events/subscribe/subscriber.cc:72-82` implements prefix-only:
`if (!g.empty() && g.back() == '*') ...` — anything else is exact
match. A client sending `event_names=["CHANNEL_*"]` works; a client
sending `["*HANGUP*"]` does not match anything (treated as literal).

Operators reading "glob" will expect at least `*` anywhere and `?`,
possibly `[abc]` character classes. The current behaviour is closer
to "wildcard prefix" than "glob".

**Fix.** Either:
- Update the proto comment to "prefix wildcard (`prefix*`)" and add
  a sentence in `event-tiers.md` to match.
- Or implement actual glob matching (use a small library — there's
  no need for full regex, just `*` and `?` should suffice).

The first option is honest and low-risk; do it now and leave glob
expansion for V2.

### I-4 — `event_drain_timeout_seconds` countdown is busy-polled (10ms sleep granularity)

`src/core/module.cc:281-283`:

```cpp
while (!rings_->AllEmpty() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
```

`rings_->AllEmpty()` acquires 3 mutexes per poll (one per tier).
With `event_drain_timeout_seconds = 10`, that's 1000 polls × 3
mutexes = 3000 mutex acquisitions during shutdown, with each poll
contending against the broadcaster's pop path. Functionally correct
but wasteful and noisy on TSAN.

**Fix.** Add a condvar to RingSet (or per-Ring) that the Ring
notifies on every pop, and the drain wait uses `wait_for(deadline)`
on it. Or: have `Ring::WaitAndPopBatch` decrement a shared atomic
counter that the drain code can spin on with `wait_for` semantics.
Not urgent; this is a small constant cost on a path that runs once
per process lifetime.

### I-5 — Module::Load failure path between `binder_->Init()` and `broadcaster_->Start()` leaves binder bound

`src/core/module.cc:215-216`:

```cpp
broadcaster_ = std::make_unique<events::Broadcaster>(rings_.get(), &health_);
broadcaster_->Start();           // <-- if std::thread ctor throws, jump to catch
```

If `Broadcaster::Start()` throws (e.g. `std::system_error` from
thread creation under resource exhaustion), the catch at line 246-252
returns false without calling `binder_->Stop()`. `binder_` is still
alive with `active_=true` (Init succeeded). FS continues delivering
events to our handler, which pushes into rings whose Broadcaster is
half-constructed. Subsequent events leak into the ring and rot until
process shutdown.

The Module singleton's destructor (process exit) will call
`binder_.reset()` which invokes the Binder destructor, which calls
`Stop()` defensively. So no UB — but the window between failure
and process exit can be hours.

**Fix.** Wrap steps 7-9 of `Module::Load` in a local RAII guard that
calls `binder_->Stop()` (and `broadcaster_->Stop()` if reached) on
unwind unless `commit()` is invoked at the end. Standard
half-constructed-state cleanup pattern.

### I-6 — `Ring::SnapshotFromSeq` for an empty ring cannot distinguish "fresh ring" from "ring evicted everything"

`src/events/ring.cc:86-95`:

```cpp
if (q_.empty()) {
    snap.found_in_window = true;
    return snap;
}
```

An empty ring is always "in window". This is correct for cold-start
when no events have ever been pushed. It is **incorrect** when the
ring previously held events with seqs up to some `M` but they have
all been evicted (e.g., a quiet tier where events fall off the end
faster than they arrive on a slow-event-rate system, or after a
deliberate ring-cap reduction). A subscriber reconnecting with
`since_seq = M-100` gets `found_in_window = true` with empty
entries, attaches at live tail, and silently misses 100 events.

**Fix.** Track the max seq ever pushed (per Ring) in an atomic
counter set inside `Push()`. `SnapshotFromSeq` then checks
`since_seq < max_seq_ever_pushed && q_.empty()` → return
`found_in_window=false`. Cheap, single atomic store on push.

---

## NITS

### N-1 — TSAN job omits `binder_test`, which is the most concurrency-heavy unit test in the suite

`.github/workflows/ci.yml:160`:
> `-R 'ring_test|broadcaster_test|subscribe_replay_test'`

`binder_test.cc:230` (`ConcurrentProducersAllEventsAccountedFor`)
spawns 8 producer threads × 64 events each through the binder's
HandleEvent (which exercises the per-tier atomic seq + Ring::Push
under contention — the exact MPSC path FF-004 mandates). Excluding
it from the TSAN job leaves the binder's concurrency path
unexercised. The test exists and is fast; add it to the `-R` set.

### N-2 — `ConcurrentProducersAllEventsAccountedFor` checks only event count, not seq uniqueness/completeness

`tests/unit/events/binder_test.cc:230-256`. The test asserts that 512
events were emitted and 512 are in the ring with zero drops — but it
doesn't verify that the seqs are `{1..512}` with no gaps. A subtle
atomic-ordering bug in `next_seq_.fetch_add` (impossible with the
current code, but easy to introduce in a future refactor) could be
missed.

**Fix.** After joins, drain the ring with `TryPop` in a loop,
collect the seqs into a vector, sort, assert it equals `{1..total}`.

### N-3 — Design doc `memory-management.md:467-473` lock-order list does NOT mention the SendQueue mu added in W2

The doc enumerates "Event ring mutex (per-tier) → Stream registry
mutex → Channel-state mutex". The W2 closeout (correctly) clarifies
the order as "tier ring mu → roster mu → SendQueue mu". SendQueue
mu is a per-subscriber sub-component of "stream registry" in the
design doc's vocabulary, but a reader of the design doc would not
infer the SendQueue ordering rule. Add a one-line "(W2: per-subscriber
SendQueue mu sits below stream registry mu)" entry.

### N-4 — `BuildEnvelope` `set_body(body)` with embedded NULs would truncate

`src/events/envelope.cc:319-322`:

```cpp
const char* body = ::osw::raii::fs::EventGetBody(ev);
if (body != nullptr && body[0] != '\0') {
    env->set_body(body);
}
```

`EventEnvelope.body` is `bytes`, but `set_body(const char*)` uses
`strlen()`. An FS event with a binary body containing embedded NUL
bytes will be silently truncated. In practice FS event bodies are
text (SIP bodies, dialplan output, etc.) and this never bites, but
a future event type with binary payloads would silently lose data.

**Fix.** Track the body length (FF-019 doesn't expose a body length
accessor; the workaround is `strlen(body)` and document the
text-only assumption — or add a body-length wrapper to fs_api.h
that reads `event->body_size` if present in the FS struct).
Low priority; flag for V2 reckoning if any future event type has
binary bodies.

### N-5 — Closeout doc lists a 14th commit hash (`0103aab`) that does not appear in `git log 9bdf784..f696174`

`implementation/W2-events.md:40`:
> `0103aab chore(events): apply silkeh/clang:18 format ...`

The actual git log shows `f696174` as the chore commit (same subject
line). The closeout was likely written before a rebase or commit-hash
amend. Non-substantive; just update the doc to match `git log`.

---

## What was checked and is clean

- All three FF entries verified against v1.10.12 source — no
  fabricated semantics, no off-by-one line cites.
- Binder shim's slot publish/clear ordering is correct: published
  BEFORE bind (so racing dispatch finds a valid Binder) and cleared
  AFTER unbind (so FF-018 wrlock guarantees in-flight callbacks have
  returned). The double-bind CAS in `Init()` correctly refuses a
  second Binder instance.
- The exception boundary in `osw_event_handler` is full-body try/catch
  with both `std::exception` and `...` catches; matches W2 contract
  rule #6. Verified that `kSubsystem` (anonymous namespace) is
  reachable from the `extern "C"` shim (same TU).
- Lock-order discipline in Broadcaster is **structurally enforced**,
  not just documented: `WaitAndPopBatch` returns a `vector<RingEntry>`
  (ring mu released before the function returns), `RosterSnapshot`
  returns a `vector<shared_ptr<Subscriber>>` (roster mu released
  before the dispatch loop), and `TryPush` is called inside the
  dispatch loop without holding either ring or roster mu. Reversed
  acquisition is impossible.
- `RingEntry::envelope_bytes` is `shared_ptr<const std::string>` —
  the `const` enforces zero-mutation after publish; multiple
  subscribers share the same buffer.
- Arena-allocated proto inside the binder callback (arena destroyed
  when callback exits) ensures one allocation per event for the
  envelope construction; the serialised bytes are a separate
  `shared_ptr<string>` that survives the arena's destruction. Correct.
- Slow-vs-fast subscriber test
  (`BroadcasterTest.SlowSubscriberDoesNotBackpressureFastOne`) asserts
  the operational contract that the lock-order discipline exists to
  guarantee.
- `Ring::Close()` is idempotent (atomic flag + condvar broadcast);
  `SendQueue::Close()` likewise; `Subscriber::RequestClose` is
  first-writer-wins on kick reason via CAS (verified in
  `SubscriberTest.KickReasonFirstWriterWins`).
- `SubscribeReplay tests` cover the Ring-level `SnapshotFromSeq`
  semantics thoroughly (7 cases including the evicted-since-seq path,
  edge-case at-max-seq, above-max-seq).
- `Module::Shutdown` drain order matches the documented sequence
  EXCEPT for the audit-emit issue called out as B-2.
- No raw `new`/`delete`/`malloc`/`free` in any W2 source file —
  verified via grep across `src/events/` + `src/control/handlers/` +
  `src/observability/audit.cc`.
- `osw_control_fs` static library cleanly isolates FS-dependent
  control handlers from `osw_control`; the handler-test gap noted as
  task #8 in the closeout is a real consequence of this split (the
  handler is only buildable in FS-linked TUs) and is acceptable for
  W2 given the W5 integration-test plan.

---

## What the orchestrator should personally re-check before merging

1. **The replay→live-tail gap fix (B-1)**, when applied, should add a
   unit test that simulates the race deterministically — push N
   events into the ring, take snapshot, push M more events (which
   the test forces the broadcaster to pop+dispatch), THEN
   AddSubscriber, then push another K, and assert the subscriber
   received exactly N+M+K events with contiguous seqs. The current
   `ReplayThenLiveTailDeliversBothToSubscriber` test does NOT exercise
   the gap window because it doesn't run the broadcaster between
   snapshot and AddSubscriber.

2. **The B-2 audit-emit fix** must update both the code AND the
   closeout doc AND the comment at module.cc:286-289. W1 B-3 was
   exactly the same failure mode and dragged into the fix-sprint;
   verifying all three are in sync at merge time prevents a repeat.

3. **TSAN strict-mode policy.** The closeout's "fail-soft for one
   wave cycle" is reasonable, but ensure the W3 spawn requirements
   include "flip TSAN to strict gate" as an explicit pre-merge gate
   for the W2.5 fix-sprint. C-3 (atomic raw pointers) needs to land
   in W2.5 for that to be feasible.

4. **The closeout doc's claim** at `W2-events.md:298-303` ("Drain
   order — verified in Module::Shutdown") is correct AS DESCRIBED, but
   the audit-emit-timing step (step 4) is the lie inside that
   correctness claim. Fixing B-2 cleans this up too.

---

**Status**: NOT READY. The two BLOCKERS are correctness issues on
named contract surfaces (Tier-1 replay continuity; audit-channel
delivery during shutdown). Both have clear fixes that fit inside a
W2.5 fix-sprint. The 3 CRITICALS are all fixable in the same sprint;
C-3 unblocks strict TSAN gating which is the natural follow-up the
closeout already anticipated.
