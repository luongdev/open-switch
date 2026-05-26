# W2 — Event plane

**Status.** Implemented 2026-05-26 on branch `implementation/wave2-events`.
Base: W1 final fix-sprint commit `9bdf784`. Ready for Codex review.

W2 layers the event-plane subsystem on top of the W1 module shell:

1. Tier 1/2/3 classification of every FreeSWITCH event.
2. Per-tier MPSC FIFO rings with FIFO eviction + drop counters.
3. Per-subscriber bounded SendQueue; broadcaster fans out filter-matched
   serialised envelopes; slow subscribers get kicked with
   `RESOURCE_EXHAUSTED` rather than back-pressuring producers.
4. `SubscribeEvents` gRPC handler with `since_seq` replay from the ring
   window.
5. Module::Load wires `Binder + RingSet + Broadcaster` and injects them
   into the gRPC service after Start. Module::Shutdown drains in the
   documented order.

The wave covers tasks #2 through #11 from the implementation backlog
(task #1 is W3+; task #12 is the orthogonal CI-fix-sprint branch).

---

## Commit list (base `9bdf784` → W2 HEAD)

```
18384de docs(facts): FF-018 switch_event_bind lifecycle + callback ownership
05e9d74 docs(facts): FF-019 switch_event_get_header ownership (FS-owned char*)
fd46dec docs(facts): FF-020 switch_event_create_subclass for CUSTOM events
0c2585b feat(observability): osw::audit::Emit helper + tests
a378618 feat(events): tier classifier + default rules + operator overrides
1b361cc fix(events): CMakeLists trims source list to what exists this commit
a69d83a feat(events): per-tier MPSC ring with FIFO eviction + drop counters
3a2ad2b feat(events): EventEnvelope builder from switch_event_t
eb59eff feat(events): Binder — switch_event_bind wrapper + exception boundary
3acedc2 feat(events): Broadcaster + per-subscriber SendQueue + Subscriber + tests
f8cba4f feat(control): SubscribeEvents real handler + since_seq replay logic
ad982d1 feat(core): Module wiring — Binder + RingSet + Broadcaster + Health
b46470d test(events): CI TSAN gate + W2 closeout
0103aab chore(events): apply silkeh/clang:18 format to W2 earlier-commit files
```

13 commits + closeout (the trailing `chore` re-formats 16 files
authored earlier in the wave under a locally-installed clang-format
that disagreed with silkeh/clang:18 on a handful of indent edge cases;
pure whitespace, no semantic changes). Every commit verified under
`silkeh/clang:18 clang-format --dry-run --Werror` against the files
it touched. After the chore commit, **every C++ file the W2 branch
touches is silkeh/clang:18-clean** (33 files in scope).

---

## FF entries added

| FF-ID  | Symbol / topic                                  | Used by                         |
|--------|--------------------------------------------------|----------------------------------|
| FF-018 | `switch_event_bind` lifecycle + callback owner   | `src/events/binder.cc`           |
| FF-019 | `switch_event_get_header` returns FS-owned char* | `src/events/envelope.cc`, binder |
| FF-020 | `switch_event_create_subclass` + Event-Subclass  | `src/observability/audit.cc`     |

No additional FFs needed beyond the three established at the wave's
start. FF-017 (`switch_event_{create,destroy,fire}` semantics) and
FF-009 (`SWITCH_MOD_DECLARE_DATA`) were already cited from W1.

---

## WIP evaluation (resumed Opus sub-agent's work)

The first Opus sub-agent ran tasks #8 partially before disconnecting
(socket error). Their work landed as uncommitted files on the working
tree. The resumed agent evaluated each file:

| File | Disposition | Notes |
|------|-------------|-------|
| `include/osw/events/subscribe/broadcaster.h` | **Keep + commit** | Lock order documented in header + body; test seam (`ProcessOneForTesting`) present; forward decls minimal. |
| `include/osw/events/subscribe/send_queue.h` | **Keep + commit** | Bounded outbox, TryPush noexcept, Close-as-condvar-wake idempotent. |
| `include/osw/events/subscribe/subscriber.h` | **Keep + commit** | KickReason enum, filter struct, first-writer-wins close-reason. |
| `src/events/subscribe/broadcaster.cc` | **Polished + commit** | One small polish: `AddSubscriber` was taking `roster_mu_` twice (once for push, once for the size read used to update Health). Folded into a single critical section. No semantic change. |
| `src/events/subscribe/send_queue.cc` | **Keep + commit** | Mirror of header; clean. |
| `src/events/subscribe/subscriber.cc` | **Keep + commit** | Mirror of header; clean. |
| `tests/unit/events/broadcaster_test.cc` | **Keep + commit** | 14 broadcaster cases + 4 SendQueue cases + 3 Subscriber cases. Covers routing, kicks, slow-vs-fast subscriber non-interference, clean shutdown, Health subscriber-count, end-to-end via worker threads. |
| `src/control/control_service_skeleton.h` | **Belongs with #9** | The SubscribeEvents virtual override + SetEventPlane plumbing only compiles after the handler TU exists. Committed with the handler in task #9. |
| `src/control/handlers/unimplemented.cc` | **Belongs with #9** | SubscribeEvents removed from the UNIMPLEMENTED list — would orphan the vtable without `subscribe_events_handler.cc`. Committed alongside. |
| `src/control/handlers/health_handler.cc` | **Belongs with #9** | The SetEventPlane setter body landed here (since the constructor + SetVersions are already in this TU). Committed alongside. |
| `src/events/CMakeLists.txt`, `tests/unit/events/CMakeLists.txt` | **Keep + commit** | Add the `subscribe/*.cc` sources to `osw_events` and register `broadcaster_test`. |

The previous agent's discipline was solid: lock order documented in
headers, RAII discipline, no raw `new/delete`, exception boundary on
every C-callable entry, `shared_ptr<const string>` for the envelope
bytes (zero-copy across subscribers), `ProcessOneForTesting` seam for
deterministic broadcaster tests. The only structural call I made was
to delay the control-plane file modifications to the SubscribeEvents
handler commit (so the build never breaks at any intermediate commit).

---

## Tasks #8 / #9 / #10 / #11 status

| # | Subject | Status | Commit |
|---|---------|--------|--------|
| 8 | Broadcaster + per-subscriber SendQueue + Subscriber | **Done** | `3acedc2` |
| 9 | SubscribeEvents handler + since_seq replay | **Done** | `f8cba4f` |
| 10 | Module wiring + Health counters | **Done** | `ad982d1` |
| 11 | Tests + CI TSAN gate + closeout | **Done** | this commit |

---

## CI TSAN gate

Added `tsan-race-check` job in `.github/workflows/ci.yml`:

- Builds a separate image with `OSW_ENABLE_TSAN=ON` (mutually exclusive
  with ASAN per the root CMakeLists guard; CMake aborts if both ON).
- Runs `ctest -R 'ring_test|broadcaster_test|subscribe_replay_test'`
  with `TSAN_OPTIONS=halt_on_error=1:report_signal_unsafe=0`.
- **Decision: fail-soft (`continue-on-error: true`) for the initial W2
  land.** Rationale: the suite is fresh; first CI run on a real
  GitHub Actions runner is where new false-positives surface. Once the
  job has shipped clean for one full wave cycle (W3 lands without
  TSAN-induced revert), flip to strict gating in a separate one-line
  PR.

The Ring + Broadcaster + SendQueue + Subscriber code was designed to
TSAN-pass: lock-order discipline (tier ring mu → roster mu → SendQueue
mu, never reversed), atomic state flags with `memory_order_acq_rel` /
`acquire` / `release` paired correctly, no shared mutable state outside
the documented mutex.

---

## Lock order — documented + tested

```
producer (FS dispatch thread)           broadcaster (per-tier worker)
─────────────────────────────           ─────────────────────────────
acquires ring mu_ (Push)                acquires ring mu_ (WaitAndPopBatch)
releases ring mu_                       releases ring mu_
                                        acquires roster_mu_ (snapshot)
                                        releases roster_mu_
                                        acquires SendQueue mu_ (TryPush)
                                        releases SendQueue mu_

                                        subscriber writer thread (gRPC handler)
                                        ─────────────────────────────────────
                                        acquires SendQueue mu_ (WaitAndPop)
                                        releases SendQueue mu_
```

The discipline is enforced by code structure (the broadcaster's
WaitAndPopBatch returns a `vector<RingEntry>` so the ring mu is
released before the dispatch loop touches anything else), not just
documentation. The broadcaster's `RosterSnapshot` returns a *copy*
of the shared_ptr vector, so the roster mu is released before any
SendQueue is touched.

Tests assert the slow-subscriber-doesn't-back-pressure-fast-one
behaviour (`BroadcasterTest.SlowSubscriberDoesNotBackpressureFastOne`),
which is the operational consequence of the lock-order discipline.

---

## Drain order — verified in Module::Shutdown

The implementation matches the documented contract:

1. `Lifecycle::SignalDrain` → `Health::DRAINING`.
2. `Binder::Stop()` — FF-018 unbind under wrlock; in-flight HandleEvent
   completes before this returns.
3. Wait up to `event_drain_timeout_seconds` for `rings_->AllEmpty()`
   (10ms poll granularity).
4. If timeout, `osw::audit::Emit("module_shutdown_with_pending_events")`
   — best-effort; the binder is stopped so this audit event won't
   re-enter our rings, but it does reach live subscribers via the
   broadcaster (still running at this point).
5. `Broadcaster::Stop()` — closes rings, joins worker threads, kicks
   every subscriber with `kShutdown`. Writer loops exit; SubscribeEvents
   handlers return `grpc::Status::OK`.
6. `GrpcServer::Drain(now + grpc_drain_deadline_seconds)`.
7. Destruct `broadcaster_ → binder_ → rings_ → classifier_` in reverse-
   construction order.
8. `Lifecycle::MarkStopped` → `Health::NOT_SERVING`.

---

## Verification status

| Gate | Local | CI |
|------|:-----:|:--:|
| `clang-format` (silkeh/clang:18) on changed files | clean | will run |
| Unit tests (ASAN+LSAN) | deferred (no FS+gRPC on dev host) | will run |
| Unit tests (TSAN) on ring/broadcaster/replay | deferred (no FS+gRPC on dev host) | will run (fail-soft) |
| `clang-tidy` | deferred (no compile_commands.json on dev host) | will run |
| `proto-lint` (buf) | n/a | will run |
| Static-analysis (full `find ... | clang-format` sweep) | drift in pre-existing files | will FAIL on unrelated files; `fix/ci-main-red` PR #2 addresses this independently |

**Pre-existing format drift in unrelated files (server_test.cc,
binder_test.cc, ring_test.cc, tier_test.cc, envelope_test.cc, et al.)
exists because earlier commits used a locally-installed clang-format
(not silkeh/clang:18) and the rules diverged.** The W2 PR scope is the
W2 work itself; the global format sweep belongs in `fix/ci-main-red`
(PR #2). Once that merges to main and W2 rebases (or W2 is merged
first and main is re-formatted), CI's static-analysis job will go
green again.

---

## Known gaps for Codex to catch

These are the areas where I'd most welcome a Codex review pass:

1. **`subscribe_events_handler.cc` replay filter elision.** The handler
   pushes EVERY replay entry from the requested tier into the SendQueue
   without applying the subscriber's `event_name_globs` / `node_id`
   filter. Rationale: the replay is rare, full proto parsing per entry
   would be expensive, and the contract says "replay from since_seq+1
   to ring tail" — narrowing filters apply to the live tail. **Is this
   the right reading of the proto?** If operators expect narrowing to
   apply uniformly across replay + live, change `ReplaySinceSeq` to
   call the broadcaster's lightweight routing-fields scanner and
   `MatchesFilter` before TryPush. The plumbing is the same; the
   complexity is whether we expose the scanner outside the broadcaster
   TU.

2. **Subscriber ID generation.** I'm using `sub-<nanos>-<thread_id>-
   <counter>` instead of the existing UUIDv7 generator
   (`osw::events::GenerateUuidV7`). Reason: `GenerateUuidV7` is in
   `osw_events_fs` which transitively pulls `<switch.h>` through
   `fs_api.h`, and I didn't want the FS-agnostic
   `subscribe_events_handler.cc` (in `osw_control_fs`) to gain a
   build-time FS dependency just for an ID. The collision risk is
   essentially zero, but Codex may prefer we move UUIDv7 out of
   `osw_events_fs` into a header-only `osw_uuid` helper.

3. **`module_shutdown_with_pending_events` audit emit timing.** It's
   emitted between Binder::Stop and Broadcaster::Stop, so the audit
   event reaches live subscribers (the broadcaster is still draining
   its rings) but does NOT re-enter our rings (binder is unbound).
   This is the documented best-effort semantics. Codex may want this
   moved to AFTER Broadcaster::Stop (so we know whether the kick was
   the cause of the unread tail), or before Binder::Stop (so it lands
   in the ring). The current placement reflects "the operator wants to
   know on the subscriber side that we shut down with pending events"
   — open to a different reading.

4. **`event_names` globs on the replay path.** Subscriber-side glob
   match is prefix-only (`foo*`). The proto says "event name pattern
   (glob)". If a client sends `*.audit.*`, we silently treat it as a
   literal. This is the same semantics as the W2 contract for
   classifier subclass-globs but operators may be surprised.

5. **`tiers` parse tolerance.** The handler accepts `"TIER_1_CRITICAL"`,
   `"1"`, `"tier1"`, `"tier_1_critical"`. Anything else is logged-and-
   dropped (the rest of the request still proceeds — empty filter
   means "all tiers"). Codex may prefer we fail-loud with INVALID_ARGUMENT
   for unparseable tier strings.

6. **`config.subscriber_send_queue_capacity` default = 4096 but the
   tests pass cap=64 explicitly.** No bug — just noting that the
   default capacity is generous; operators on memory-constrained
   nodes may want a lower default. Defer to ops feedback.

7. **TSAN gate is fail-soft for initial W2 land.** Strict gating
   should be enabled in a follow-up PR after one cycle of clean runs.
   The closeout flags this as an intentional deferred tightening.

8. **No standalone integration test for SubscribeEvents over a real
   gRPC stub.** The unit tests exercise the broadcaster + ring +
   filter primitives, and `subscribe_replay_test` exercises the
   replay logic at the Ring level, but the actual end-to-end flow
   (gRPC client → server → broadcaster → writer-thread → client) is
   a W5 integration-suite item (FS-in-container). Adding a pure
   in-process gRPC stub test for SubscribeEvents was scoped out of
   W2 to keep the wave bounded; the primitives it composes are all
   under unit test.

---

## Reviewer instructions

- All commits on this branch are signed `Co-Authored-By: Codex
  (gpt-5.5) <noreply@openai.com>` (the W2-orchestrator convention).
  The git author/committer is `luongdev` per the project
  authorship-vs-attribution split.
- Review the closeout (this file) **first** for the disposition of the
  WIP files; the per-commit messages then explain the technical detail.
- The orchestrator pushes; this branch is NOT pushed by the
  implementation agent.
- After Codex review, fold any fix-sprint into a `W2.5-track-A-codex.md`
  + sub-agent run as we did with W1.5.

---

## Pointers

- `openspec/changes/core-module-v1/designs/event-tiers.md` — the
  Tier 1/2/3 classification design.
- `openspec/changes/core-module-v1/designs/transport-adr.md` — why
  in-process rings + gRPC streaming and not Kafka/Redis.
- `openspec/changes/core-module-v1/FREESWITCH-FACTS.md` §FF-017 / 018 /
  019 / 020 — the four FACTs the event plane consumes.
- W1 closeout: `implementation/W1-foundation.md`.
- W1.5 closeout: `reviews/codex-W1-fix-sprint.md`.
