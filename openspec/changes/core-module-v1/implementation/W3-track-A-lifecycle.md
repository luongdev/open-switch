# W3 Track A — Lifecycle (Originate + Hangup + HangupMany)

**Wave.** [W3 Control plane](W3-control-plane.md).
**Owner.** Sonnet sub-agent (claude-sonnet).
**Branch.** `implementation/wave3-track-a-lifecycle` (off `main`
after the W2 merge lands).

Track A lands the lifecycle RPCs *plus* the shared helper layer that
Tracks B and C consume. It must merge before B and C start.

---

## Files in scope

**Create.**
- `include/osw/control/call_cause.h`
- `src/control/call_cause.cc`
- `include/osw/control/session_guard.h` (RAII session_locate + channel)
- `src/control/session_guard.cc`
- `include/osw/control/originate_options.h`
- `src/control/originate_options.cc`
- `src/control/handlers/originate_handler.cc`
- `src/control/handlers/hangup_handler.cc`
- `src/control/handlers/hangup_many_handler.cc`
- `tests/unit/control/call_cause_test.cc`
- `tests/unit/control/originate_handler_test.cc`
- `tests/unit/control/hangup_handler_test.cc`
- `tests/unit/control/hangup_many_handler_test.cc`

**Modify.**
- `src/control/handlers/unimplemented.cc` — remove Originate / Hangup
  / HangupMany method bodies (now in dedicated TUs).
- `src/control/CMakeLists.txt` — add new TUs to `osw_control_fs`.
- `tests/unit/control/CMakeLists.txt` — register new test binaries
  (use the existing `osw_add_unit_test` helper with `LABEL "control;unit"`).
- `openspec/changes/core-module-v1/FREESWITCH-FACTS.md` — append
  FF-021 (`switch_ivr_originate`) + FF-022 (`switch_channel_hangup`).
- `include/osw/raii/fs_api.h` — add thin RAII wrappers for the new
  FS entry points (`originate_session`, `channel_hangup`).
- `include/osw/raii/fs_mock.h` — extend the mock seam to capture
  originate + hangup invocations for the unit tests.

---

## Helper layer requirements

### `osw::control::CallCause`

Two-way mapping between proto `open_switch.control.v1.Cause` and
`switch_call_cause_t` (FS Q.850-derived enum). The proto enum is a
subset of the FS one — the helper must:

- Map every proto-defined cause to the exact FS value.
- Map unknown FS values to `Cause::CAUSE_UNSPECIFIED` (not crash).
- Map proto `CAUSE_UNSPECIFIED` to `SWITCH_CAUSE_NORMAL_CLEARING`
  (FS-side default for graceful hangup).
- Be a pure function — no global state, no allocation.

### `osw::control::SessionGuard`

Wraps `osw::SessionLock` (from `include/osw/raii/session_lock.h`) and
additionally extracts + caches `switch_core_session_get_channel(session)`.
Move-only. Three states:

- Constructed empty (`!Valid()`).
- Locked via `SessionGuard::Locate(uuid)` (returns the guard; check
  `Valid()`).
- Released via dtor or explicit `Reset()`.

`Channel()` returns `switch_channel_t*`, nullptr when `!Valid()`.

### `osw::control::OriginateOptions`

Builder that materialises an `OriginateRequest` into the parameter
shape `switch_ivr_originate` expects:

- `dial_string` (rendered from `request.destination` + optional gateway
  prefix per the proto comment),
- `caller_id_name` / `caller_id_number`,
- `timeout_seconds`,
- `variables` (map<string,string> → `{name=value}` chunks inserted as
  channel variables at the start of the dial string).

Returns a heap-allocated channel-variable list that the originate
helper owns and frees on completion.

---

## RPC contract — Originate

**Success.** Returns `OriginateResponse { uuid }`. The uuid is the
new session's UUID. Audit emit `osw.control.originate { uuid, dest,
cid }`.

**Failure modes.**
- `INVALID_ARGUMENT`: empty `destination`, malformed gateway prefix,
  timeout ≤ 0.
- `FAILED_PRECONDITION`: `switch_ivr_originate` returned a non-success
  status (e.g. CAUSE_USER_BUSY, CAUSE_NO_ANSWER); cause code surfaced
  in the gRPC status detail.
- `DEADLINE_EXCEEDED`: timeout reached before answer.
- `UNAVAILABLE`: FS unreachable (FS state ≠ ready) — the RAII helper
  returns null on `originate_session`.

Sync only in V1 (the request blocks until originate returns).

## RPC contract — Hangup

**Success.** Returns `HangupResponse {}`. Audit emit
`osw.control.hangup { uuid, cause }`.

**Failure modes.**
- `INVALID_ARGUMENT`: empty `uuid`.
- `NOT_FOUND`: `SessionGuard::Locate` returns invalid (UUID unknown).
- `FAILED_PRECONDITION`: channel already hung up (defensive — FS will
  no-op `switch_channel_hangup` on a dead channel; the helper returns
  the channel's current state to detect this).

## RPC contract — HangupMany

Loops Hangup for each uuid in the request. Returns
`HangupManyResponse { results }` where each result has `{ uuid,
ok, error_message }`. Never short-circuits — every uuid attempted.
Audit emit one event per uuid (mirrors Hangup).

---

## FF entries

### FF-021 — `switch_ivr_originate` signature + ownership

Cite `/usr/local/include/switch_ivr.h` for the signature:

```c
switch_status_t switch_ivr_originate(
    switch_core_session_t *session,
    switch_core_session_t **bleg,
    switch_call_cause_t *cause,
    const char *bridgeto,
    uint32_t timelimit_sec,
    const switch_state_handler_table_t *table,
    const char *cid_name_override,
    const char *cid_num_override,
    switch_caller_profile_t *caller_profile_override,
    switch_event_t *ovars,
    switch_originate_flag_t flags,
    switch_call_cause_t *cancel_cause,
    switch_dial_handle_t *dh);
```

Document:
- Caller's `session` arg is NULL for unattended originate (V1 path).
- `bleg` is set to the new session on success — caller owns the
  rwlock (must `switch_core_session_rwunlock` once done).
- `cause` is set to the result cause (Q.850-style).
- `ovars` is the variable list; the helper consumes ownership.

### FF-022 — `switch_channel_hangup` cause semantics

Cite `/usr/local/include/switch_channel.h`. Document that
`switch_channel_hangup(channel, cause)` is idempotent (a second call
on an already-hung-up channel returns SWITCH_STATUS_FALSE without
side effects). Caller must hold the read-lock on the session.

---

## Test plan (FS-mock seam)

For each handler:
- Happy path: assert handler returns OK + audit emit fired with the
  expected subclass + channel state mutation visible via the mock.
- Every documented failure mode: assert correct grpc::Status code +
  audit emit is NOT fired (we audit successful mutations only).
- Argument validation: empty uuid, negative timeout, oversized
  variable map.

CallCause test exercises the full mapping table both directions plus
the unknown-value sentinel.

---

## Verification gate

```bash
cd /tmp/open-switch
docker buildx build --platform linux/arm64 \
  --build-arg OSW_ENABLE_ASAN=ON --build-arg OSW_BUILD_TESTS=ON \
  --target fs-builder -f deploy/docker/Dockerfile.builder \
  --load -t ci-asan .
docker run --rm ci-asan ctest --test-dir /usr/src/open-switch/build \
                              --output-on-failure -L unit
```

All unit tests pass under ASAN+LSAN. clang-format + clang-tidy clean.
No new TSAN violations against the existing race-sensitive filter
(the new code is single-threaded per RPC).

Commit message convention: Conventional Commits (`feat:`, `fix:`,
`docs:`, `test:`, `chore:`). No `Co-Authored-By` trailer — the
contributor is always @luongdev. Example:

```
feat(control): Originate handler + originate_session RAII

Lands the Originate RPC against the FS-mock seam. Adds
osw::control::OriginateOptions builder + RAII wrapper around
switch_ivr_originate (FF-021).
```
