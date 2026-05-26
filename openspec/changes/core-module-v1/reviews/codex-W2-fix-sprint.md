# W2.5 Codex review fix sprint — closeout

**Date**: 2026-05-26
**Branch**: `implementation/wave2-events`
**Base**: `f696174` (W2 final commit reviewed by Codex)
**Tip**: `296919d`
**Commits added**: 10
**Verdict**: all BLOCKERS, all CRITICALS, 4/6 IMPORTANTS, 4/5 NITS closed; 2 IMPORTANTS + 1 NIT deferred with rationale.

The Codex W2 review (`reviews/codex-W2.md`) was NOT READY with 2 BLOCKERS, 3 CRITICALS, 6 IMPORTANTS, 5 NITS. This sprint closes the gating findings + the cheap I/N items in 10 separate commits on the same branch; no rebase, no force push.

---

## Findings closed

| ID  | Title                                                                  | Commit     |
|-----|------------------------------------------------------------------------|------------|
| C-1 | Apply subscriber filter narrowing during replay                        | `97cea93`  |
| B-1 | Atomic snapshot+AddSubscriber closes replay race                       | `037dff9`  |
| B-2 | Drop dead-lettered shutdown audit (FS-log only)                        | `e88a608`  |
| C-2 | Document prefix-wildcard glob limitation for V1                        | `f9eee3b`  |
| C-3 | Atomic event-plane bridges in ControlServiceSkeleton                   | `2a85260`  |
| I-1 | Fail-loud on all-unparseable SubscribeEvents tiers                     | `a6268ce`  |
| I-5 | RAII guard for Module::Load partial-init unwind                        | `ab9e70c`  |
| I-6 | Distinguish fresh ring from drained ring in SnapshotFromSeq            | `f10d8cb`  |
| N-1 | TSAN job now runs `binder_test`                                        | `c77fa59`  |
| N-2 | Seq uniqueness/completeness asserted in ConcurrentProducers            | `c77fa59`  |
| I-2 | Audit-channel quirks (module_loaded invisibility) documented           | `296919d`  |
| N-3 | memory-management.md lock-order updated with SendQueue + atomic add    | `296919d`  |
| N-5 | W2-events.md commit hash corrected (`0103aab` → `f696174`)             | `296919d`  |
| I-3 | event_names glob is prefix-only — covered by C-2 (proto + spec docs)   | `f9eee3b`  |

---

## Findings deferred

| ID  | Title                                              | Reason for deferral                                                                                                                                                                                                                |
|-----|----------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| I-4 | `event_drain_timeout_seconds` busy-poll → condvar  | ~30+ LOC across RingSet + Ring; needs a per-RingSet condvar broadcast on every pop. Functional behaviour is correct (it works; just noisy on TSAN). Defer to a focused W3 polishing commit when the drain path gets revisited.   |
| N-4 | `BuildEnvelope` `set_body(body)` strlen truncation | FS event bodies are text-only in practice (FF-019 confirms `event->body` is a `char*` with no separate length accessor available to us). A body with an embedded NUL would silently truncate. Flag for V2 if any binary-body event type lands; mention added to envelope.cc TODO inline if needed in a future polish pass. |

Two of the original six IMPORTANTs are deferred; the other four (I-1, I-2, I-3 via C-2, I-5, I-6) are closed in-sprint. One of the original five NITs (N-4) is deferred; N-1, N-2, N-3, N-5 are closed.

---

## Verification status

| Gate                                                                                                | Status     |
|-----------------------------------------------------------------------------------------------------|------------|
| `clang-format --dry-run --Werror` via `silkeh/clang:18` on every modified file in every commit      | clean      |
| Local build (cmake + gRPC)                                                                          | not attempted on dev host (W2 closeout already documents the no-FS/gRPC dev environment); CI is the real gate |
| Local unit tests (ASAN+LSAN)                                                                        | not attempted on dev host                                                                                |
| Local unit tests (TSAN, with strict mode)                                                           | not attempted on dev host                                                                                |
| Local `clang-tidy`                                                                                  | not attempted on dev host (no `compile_commands.json`)                                                  |

CI runs are the binding gate. Every commit passes the same silkeh/clang:18 format check the static-analysis CI job runs.

---

## Notable implementation choices

1. **B-1 fix (`AddSubscriberAtomic`).** Chose the "hold roster_mu_ across the closure" approach over a multi-ring-mu-then-roster lock acquisition because the latter is deadlock-prone with the worker's existing (ring → roster) order. The chosen approach inverts ring-vs-roster ordering for the handler (roster → ring) but workers never hold both at once, so no circular wait. The lock-order doc was updated to reflect this (N-3).
2. **B-2 fix (option C).** Renamed `module_shutdown_with_pending_events` → FS-log `module_shutdown_drain_timeout`. Operator visibility is preserved via the FS log + `Health.tierN_dropped_total`. No subscriber-facing API change beyond removing a subclass that gRPC subscribers never received anyway.
3. **C-3 fix (atomics + ordering).** `SetEventPlane` release-stores in the order `(max_subscribers, queue_capacity, rings, broadcaster)`. The handler acquire-loads `broadcaster_` first and bails on null — guaranteeing that a non-null broadcaster_ implies fully-published bridge state. Snapshot-into-locals at RPC entry so subsequent reads see a consistent set even if SetEventPlane is called again concurrently (it isn't in practice, but cheap discipline).
4. **C-1 helper extraction.** `ExtractRoutingFields` lives in `osw/events/subscribe/routing.h` now; broadcaster + handler both use the same scanner. The previous embedded implementation in `broadcaster.cc` is gone (replaced with the `#include`).
5. **B-1 test seam.** Added `Broadcaster::SetPostPopHookForTesting` — fires inside the worker between `WaitAndPopBatch` (non-empty) and `RosterSnapshot`. Used by the deterministic race test `AtomicAddSubscriberClosesReplayLiveTailGap` to expose the bug window without flaky timing. Production builds carry a default-empty hook (one mutex acquisition + load per non-empty batch); cost is negligible.

---

## What the orchestrator should re-check

- `Broadcaster::AddSubscriberAtomic` API surface in `include/osw/events/subscribe/broadcaster.h`. The lock-order rationale is documented in the doxygen on the method and in `designs/memory-management.md`.
- `subscribe_replay_test.cc::AtomicAddSubscriberClosesReplayLiveTailGap` — the deterministic race fixture uses the new post-pop hook. Worth eyeballing for "is this what the prompt asked for" before W3 spawns.
- `event-tiers.md` "Audit-channel quirks" section for the doc-vs-code consistency that the original B-2 finding flagged.
- The deferred I-4 / N-4 items are low-priority polish; not blockers for W3.

---

## TSAN strict gate

C-3 makes the event-plane bridges atomic. With that, the TSAN strict-mode follow-up the W2 closeout anticipated is now feasible: flip `halt_on_error=1` to strict (`exitcode=66`) in `.github/workflows/ci.yml` once the W2.5 sprint merges and one cycle of green TSAN runs is observed. The CI line currently runs `binder_test|broadcaster_test|subscribe_replay_test|ring_test` (N-1 added binder_test).
