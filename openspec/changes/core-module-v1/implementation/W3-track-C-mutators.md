# W3 Track C — Channel mutators (SetVariables + Hold + Unhold)

**Wave.** [W3 Control plane](W3-control-plane.md).
**Owner.** Sonnet sub-agent.
**Branch.** `implementation/wave3-track-c-mutators` (off `main`
after Track A merges).

Track C implements the three RPCs that mutate channel state in place
without changing the call topology. All three consume Track A's
`SessionGuard` helper.

---

## Files in scope

**Create.**
- `src/control/handlers/set_variables_handler.cc`
- `src/control/handlers/hold_handler.cc`
- `src/control/handlers/unhold_handler.cc`
- `tests/unit/control/set_variables_handler_test.cc`
- `tests/unit/control/hold_handler_test.cc`
- `tests/unit/control/unhold_handler_test.cc`

**Modify.**
- `src/control/handlers/unimplemented.cc` — remove the three handlers.
- `src/control/CMakeLists.txt` — add new TUs.
- `tests/unit/control/CMakeLists.txt` — register new tests.
- `openspec/changes/core-module-v1/FREESWITCH-FACTS.md` — append
  FF-026 + FF-027.
- `include/osw/raii/fs_api.h` — add wrappers for
  `channel_set_variable`, `hold_uuid`, `unhold_uuid`.
- `include/osw/raii/fs_mock.h` — extend mock to capture.

---

## RPC contract — SetVariables

Bulk-set channel variables on a single session.

**Success.** Returns `SetVariablesResponse { ok=true }`. Audit emit
`osw.control.set_variables { uuid, var_count }` — variable VALUES
are NOT logged (they can contain PII; only the count is auditable).

**Failure modes.**
- `INVALID_ARGUMENT`: empty uuid, empty `variables` map, or any
  variable name containing characters outside `[A-Za-z0-9_-]`.
- `NOT_FOUND`: uuid unknown.
- `RESOURCE_EXHAUSTED`: more than 64 variables in a single request
  (V1 bound — protects against abuse; V2 raises the limit).

Iterates the proto map, calling `switch_channel_set_variable(channel,
name, value)` once per pair. If any pair fails the helper logs and
continues — all-or-nothing semantics are V2.

## RPC contract — Hold

Put a channel on music-on-hold via `switch_ivr_hold_uuid`.

**Success.** Returns `HoldResponse { ok=true }`. Audit emit
`osw.control.hold { uuid, moh_class }`.

**Failure modes.**
- `INVALID_ARGUMENT`: empty uuid.
- `NOT_FOUND`: uuid unknown.
- `FAILED_PRECONDITION`: channel not in answered state (cannot hold
  a channel mid-setup or post-hangup). Verified via
  `switch_channel_test_flag(channel, CF_ANSWERED)`.

Optional `moh_class` defaults to NULL (FS picks the channel's
configured default MoH).

## RPC contract — Unhold

Take a channel off hold via `switch_ivr_unhold_uuid`.

**Success.** Returns `UnholdResponse { ok=true }`. Audit emit
`osw.control.unhold { uuid }`.

**Failure modes.**
- `INVALID_ARGUMENT`: empty uuid.
- `NOT_FOUND`: uuid unknown.
- `FAILED_PRECONDITION`: channel not currently on hold (verified via
  `switch_channel_test_flag(channel, CF_HOLD)`).

---

## FF entries

### FF-026 — `switch_channel_set_variable` lifetime + thread

Cite `/usr/local/include/switch_channel.h`. Document:
- FS copies both name and value into the channel pool. Caller's
  buffers can be freed immediately after the call.
- Safe to call from any thread as long as the read-lock on the
  session is held (FF-016).

### FF-027 — `switch_ivr_hold_uuid` / `switch_ivr_unhold_uuid`

Cite `/usr/local/include/switch_ivr.h`. Document:
- Both return `switch_status_t`. SUCCESS means the FS state machine
  accepted the request, NOT that the user-perceived state changed.
- `hold_uuid` accepts NULL moh_class — falls back to the channel's
  default.
- Idempotent: holding an already-held channel is a no-op SUCCESS;
  unholding a not-held channel returns FALSE without side effects.

---

## Test plan (FS-mock seam)

Standard happy + failure paths per handler. Plus:

- SetVariables with 0 vars (INVALID_ARGUMENT), 1 var (happy), 64 vars
  (happy — at the bound), 65 vars (RESOURCE_EXHAUSTED).
- SetVariables with invalid variable name characters
  (INVALID_ARGUMENT, no FS call made).
- Hold then Unhold sequence — verify both audit emissions and the
  mock's CF_HOLD flag tracking.

---

## Dependencies

- Track A's `SessionGuard`.
- Track A's FF-016 cite for the read-lock invariant.

---

## Verification gate

Same matrix as Tracks A + B. No new TSAN considerations (these are
single-session, single-RPC paths).

```
Co-Authored-By: Claude Sonnet <noreply@anthropic.com>
```
