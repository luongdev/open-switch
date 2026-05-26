# W3 Track B — Connect/Execute (Bridge + Execute + BlindTransfer)

**Wave.** [W3 Control plane](W3-control-plane.md).
**Owner.** Sonnet sub-agent.
**Branch.** `implementation/wave3-track-b-connect` (off `main` after
Track A merges).

Track B implements the three RPCs that connect existing sessions to
other sessions, dialplan apps, or dialplan extensions. All three
consume Track A's `SessionGuard` helper.

---

## Files in scope

**Create.**
- `src/control/handlers/bridge_handler.cc`
- `src/control/handlers/execute_handler.cc`
- `src/control/handlers/blind_transfer_handler.cc`
- `tests/unit/control/bridge_handler_test.cc`
- `tests/unit/control/execute_handler_test.cc`
- `tests/unit/control/blind_transfer_handler_test.cc`

**Modify.**
- `src/control/handlers/unimplemented.cc` — remove the three handlers'
  method bodies.
- `src/control/CMakeLists.txt` — add new TUs to `osw_control_fs`.
- `tests/unit/control/CMakeLists.txt` — register new test binaries.
- `openspec/changes/core-module-v1/FREESWITCH-FACTS.md` — append
  FF-023 + FF-024 + FF-025.
- `include/osw/raii/fs_api.h` — add thin RAII wrappers for
  `uuid_bridge`, `execute_application`, `session_transfer`.
- `include/osw/raii/fs_mock.h` — extend the mock to capture these.

---

## RPC contract — Bridge

Connect two answered channels via `switch_ivr_uuid_bridge`.

**Success.** Returns `BridgeResponse { ok=true }`. Audit emit
`osw.control.bridge { a_uuid, b_uuid }`.

**Failure modes.**
- `INVALID_ARGUMENT`: either uuid empty, or both uuids identical.
- `NOT_FOUND`: either uuid unknown (`SessionGuard::Locate` fails for
  either side).
- `FAILED_PRECONDITION`: either channel not in `CS_ROUTING` /
  `CS_EXECUTE` state. The handler reads channel state via
  `switch_channel_get_state(channel)` and rejects bridging during
  setup or teardown.

**Locking discipline.** The handler MUST lock both sessions in a
deterministic order to avoid AB-BA deadlock with concurrent Bridge
calls on the same pair in reverse. Use lexicographic uuid order:
acquire the lower-uuid SessionGuard first, then the higher-uuid one.
Document the ordering in the handler header comment.

## RPC contract — Execute

Run a dialplan application on a channel via
`switch_core_session_execute_application`.

**Success.** Returns `ExecuteResponse { ok=true }`. Audit emit
`osw.control.execute { uuid, app, args_redacted }` — the `args` field
is logged with secrets-redaction applied (no full string in the audit
trail; redact any obvious password / token patterns). The full app +
args string is fine in the channel itself.

**Failure modes.**
- `INVALID_ARGUMENT`: empty uuid, empty app name, or app name not in
  the FS allow-list (V1 ships a fixed allow-list of `playback`,
  `bridge`, `transfer`, `set`, `hangup`, `answer`, `play_and_get_digits`).
- `NOT_FOUND`: uuid unknown.
- `FAILED_PRECONDITION`: channel hung up before execute started.

V1 is synchronous — the request blocks until the app completes.
Async (fire-and-forget) is V2.

## RPC contract — BlindTransfer

Send a channel to a different dialplan extension via
`switch_ivr_session_transfer`.

**Success.** Returns `BlindTransferResponse { ok=true }`. Audit emit
`osw.control.blind_transfer { uuid, extension, dialplan, context }`.

**Failure modes.**
- `INVALID_ARGUMENT`: empty uuid, empty extension.
- `NOT_FOUND`: uuid unknown.

Optional fields default to NULL for FS (`dialplan` defaults to
"XML", `context` defaults to the channel's existing context). Pass
through accordingly.

---

## FF entries

### FF-023 — `switch_ivr_uuid_bridge` two-uuid locking order

Cite `/usr/local/include/switch_ivr.h`. Document:
- The helper locates both sessions internally then bridges. Bridging
  blocks until either party hangs up or the bridge times out.
- Our handler-side discipline: acquire SessionGuards in
  lexicographic uuid order BEFORE calling uuid_bridge, then release
  both before returning. This prevents AB-BA against concurrent
  inverse-pair Bridge calls.

### FF-024 — `switch_core_session_execute_application` blocking semantics

Cite `/usr/local/include/switch_core.h`. Document that the helper
blocks the calling thread until the app's main loop returns; V1
handler runs this on the gRPC thread (acceptable for the bounded set
of allow-listed apps). Note FF-016 (read-locked session) is held
across the entire call.

### FF-025 — `switch_ivr_session_transfer` arg-NULL semantics

Cite `/usr/local/include/switch_ivr.h`. Document NULL handling for
`dialplan` (defaults to "XML") and `context` (defaults to the
channel's current context). The `extension` arg MUST be non-NULL.

---

## Test plan (FS-mock seam)

Standard happy-path + every failure mode per handler. Plus:

- Bridge: deterministic locking-order regression. Two threads call
  Bridge(A,B) and Bridge(B,A) concurrently against the same pair;
  both must complete (no deadlock) and both must serialise (only one
  call lands the bridge, the other gets FAILED_PRECONDITION).
- Execute: allow-list enforcement — every disallowed app name returns
  INVALID_ARGUMENT without calling the FS API.
- BlindTransfer: optional-NULL pass-through verified by mock capture.

---

## Dependencies

- Track A's `SessionGuard` (consumed by all three handlers).
- Track A's `CallCause` (Bridge uses CAUSE_LOSE_RACE for the loser
  thread in the concurrent-bridge regression).
- Track A's FF-022 (already established by then).

---

## Verification gate

Same matrix as Track A. Also: run the concurrent-Bridge regression
under TSAN to verify the locking-order discipline holds.

Commit message convention: Conventional Commits, no `Co-Authored-By`
trailer (contributor is always @luongdev).
