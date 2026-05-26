# W3 — Control plane

**Status.** Planned 2026-05-26. To start once W2 (PR #3 `941ac77`+)
merges into `main` and codex review closes any blockers.

W3 implements the 9 channel-control RPCs currently stubbed by
`src/control/handlers/unimplemented.cc`:

| RPC            | Track | FS entry point                            |
|----------------|-------|-------------------------------------------|
| Originate      | A     | `switch_ivr_originate`                    |
| Hangup         | A     | `switch_channel_hangup`                   |
| HangupMany     | A     | (loop of Hangup)                          |
| Bridge         | B     | `switch_ivr_uuid_bridge`                  |
| Execute        | B     | `switch_core_session_execute_application` |
| BlindTransfer  | B     | `switch_ivr_session_transfer`             |
| SetVariables   | C     | `switch_channel_set_variable`             |
| Hold           | C     | `switch_ivr_hold_uuid`                    |
| Unhold         | C     | `switch_ivr_unhold_uuid`                  |

Three independent parallel tracks plus a shared helper layer that
Track A owns and lands first.

---

## Scope boundary (IN / OUT)

**IN**
- All 9 RPCs return real grpc::Status: OK on success, codes per the
  proto + status-codes design doc (`designs/status-codes.md`).
- Shared helper layer:
  - `osw::control::CallCause` — proto `Cause` enum ↔
    `switch_call_cause_t` two-way mapping.
  - `osw::control::SessionGuard` — RAII wrapper around `SessionLock`
    that also caches `switch_core_session_get_channel(session)` so
    handlers don't repeat that lookup. Moves-only, never copies.
  - `osw::control::OriginateOptions` — builder that materialises
    `OriginateRequest` fields into the `{var_pool, cid, dial_string,
    timeout, ...}` shape `switch_ivr_originate` expects.
- Audit emit (`osw::audit::Emit`) on every RPC that mutates channel
  state (Originate, Hangup, HangupMany, Bridge, Execute, BlindTransfer,
  SetVariables, Hold, Unhold). Subclass = `osw.control.<rpc_lower>`.
  Reuses the W2 audit seam — no new helper needed.
- Health counters per RPC (success / fail / latency-bucketed). Mirror
  the W2 events-side counters' style.
- Unit tests against the FS-mock seam (`include/osw/raii/fs_mock.h`)
  for every code path the handler exercises.
- Updated `src/control/handlers/unimplemented.cc` — remove the 9 W3
  methods from the unimplemented forward; only the V2-deferred RPCs
  remain (none in V1).

**OUT**
- Async / fire-and-forget Originate. V1 is synchronous only — the
  request blocks until the originated call answers, fails, or hits
  timeout. Async path is V2.
- Conference / park / eavesdrop / 3-way calls — V2.
- TTS playback inside Execute. Execute runs ARBITRARY dialplan apps
  (`playback`, `bridge`, `transfer`, etc.) but the W3 contract does
  not add app-specific helpers — they go via the generic
  `app + args` proto fields.
- Real-FS integration tests — they live in W5 alongside the rest of
  the FS-in-container suite. W3 lands unit tests with the FS-mock
  seam only.

---

## FF entries to add

W3 touches FreeSWITCH APIs the FACTS doc hasn't yet covered. Each
needs an entry with the **verified-on-v1.10.12** discipline:

| FF-ID  | Symbol / topic                                  | Track |
|--------|--------------------------------------------------|-------|
| FF-021 | `switch_ivr_originate` signature + ownership     | A     |
| FF-022 | `switch_channel_hangup` cause-code semantics     | A     |
| FF-023 | `switch_ivr_uuid_bridge` two-uuid locking order  | B     |
| FF-024 | `switch_core_session_execute_application` async  | B     |
| FF-025 | `switch_ivr_session_transfer` arg-NULL semantics | B     |
| FF-026 | `switch_channel_set_variable` lifetime + thread  | C     |
| FF-027 | `switch_ivr_hold_uuid` / `switch_ivr_unhold_uuid` | C     |

Each FF must cite a source-tree path under `/usr/local/include` and
a small excerpt of the upstream signature + caller contract. The
discipline matches FF-016 / FF-017 from W1+W2.

---

## Track split

### Track A — Lifecycle (Originate + Hangup + HangupMany)

**Owner.** Sonnet sub-agent.
**Branch.** `implementation/wave3-track-a-lifecycle`.
**Output.** `src/control/handlers/{originate,hangup,hangup_many}_handler.cc`
plus shared helpers (`osw::control::CallCause`, `SessionGuard`,
`OriginateOptions`) under `include/osw/control/` + `src/control/`.

Track A also lands FF-021 + FF-022.

### Track B — Connect/Execute (Bridge + Execute + BlindTransfer)

**Owner.** Sonnet sub-agent.
**Branch.** `implementation/wave3-track-b-connect`.
**Output.** `src/control/handlers/{bridge,execute,blind_transfer}_handler.cc`.
Depends on Track A's `SessionGuard` + `CallCause` (waits for A merge).

Track B also lands FF-023 + FF-024 + FF-025.

### Track C — Channel mutators (SetVariables + Hold + Unhold)

**Owner.** Sonnet sub-agent.
**Branch.** `implementation/wave3-track-c-mutators`.
**Output.** `src/control/handlers/{set_variables,hold,unhold}_handler.cc`.
Depends on Track A's `SessionGuard` (waits for A merge).

Track C also lands FF-026 + FF-027.

---

## Track order

```
A (lifecycle + helpers)  ─→  B (connect/execute)
                          ╲
                            ─→  C (channel mutators)
```

Track A merges first because B + C consume its helpers. B and C then
proceed in parallel — they touch disjoint handler files and never
share state.

---

## Verification gates (per merge into wave3 integration branch)

Same as W1/W2 (the unit-test gate now actually runs tests after the
W2.7 `ctest -L unit` label fix lands):

```bash
docker buildx build --build-arg OSW_ENABLE_ASAN=ON \
                    --build-arg OSW_BUILD_TESTS=ON \
                    --target fs-builder -f deploy/docker/Dockerfile.builder \
                    -t ci-asan .
docker run --rm ci-asan ctest --test-dir /usr/src/open-switch/build \
                              --output-on-failure -L unit
```

Plus clang-format (`silkeh/clang:18 --dry-run --Werror`), clang-tidy
(`-p build --warnings-as-errors='*'`), buf format + lint, and the
TSAN gate against `ring_test|broadcaster_test|subscribe_replay_test|binder_test`
(unchanged from W2).

---

## Definition of done

- All 9 RPCs return real status; `unimplemented.cc` no longer
  references them.
- 7 new FF entries cited from `/usr/local/include`.
- Unit-test coverage: each handler has tests for success, every
  documented failure path (NOT_FOUND, INVALID_ARGUMENT, FAILED_PRECONDITION,
  UNAVAILABLE, RESOURCE_EXHAUSTED), and the audit emission on the
  success path.
- Audit subclasses fire per-RPC; verified by FS-mock test counters.
- Health counters increment per-RPC (Success vs Error split).
- All commits pass the CI matrix: build amd64+arm64 + ASAN unit
  tests + TSAN race check + clang-format + clang-tidy + buf lint +
  markdownlint.
- Codex review at wave close — same shape as W1/W2 reviews
  (blockers/criticals/importants/nits).
