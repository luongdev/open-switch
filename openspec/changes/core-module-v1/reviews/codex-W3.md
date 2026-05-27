# Codex W3 wave-level review

**Reviewer.** Codex (gpt-5.5), xhigh reasoning.
**Base.** `0cb0bae` (commit immediately before W3 implementation started).
**Head.** `8468fe3` (PR #8 merge — W3 wave-events).
**Scope.** Full W3 Control plane (Originate/Hangup/HangupMany/Bridge/Execute/BlindTransfer/SetVariables/Hold/Unhold + helpers + tests).

---

## Verdict

> The patch introduces a likely production link failure and several core
> RPC/runtime regressions, including Hangup reporting failures as errors
> and security-sensitive validation gaps. These issues would break or
> weaken documented control-plane behavior.

Nine findings. Five P1 (production-blocking, must close before any real
deploy), four P2 (functional / security issues to close in the fix
sprint).

---

## P1 — production-blocking

### P1-1 — Static link order for moved handlers

`src/control/CMakeLists.txt:47-49` + `src/CMakeLists.txt:67-82`

With the 6 W3B+W3C overrides now only in `osw_control_fs`, the
production `mod_open_switch` link still lists `osw_control_fs` before
`osw_control` in `src/CMakeLists.txt`; on GNU ld the handler archive is
scanned before `osw_control`'s vtable references are introduced, so
symbols such as `ControlServiceSkeleton::Originate` remain undefined.
The tests use `osw_control osw_control_fs` for this reason, but the
module target needs the same ordering or a link group.

**Status.** Fixed in `fix/mod-link-whole-archive` —
`$<LINK_LIBRARY:WHOLE_ARCHIVE,osw_control_fs>` forces all handler .o
files into the .so unconditionally. Verified via `nm /dist/mod/mod_open_switch.so`
(all 9 ControlServiceSkeleton overrides present as local text symbols);
end-to-end smoke (`docker compose up` + Go client) passes Health,
SubscribeEvents stream, Originate cause propagation, Hangup
NOT_FOUND, Bridge INVALID_ARGUMENT, Execute allow-list.

### P1-2 — Treat successful Hangup as success

`src/control/handlers/hangup_handler.cc:88-92`

`switch_channel_perform_hangup` returns `channel->state` *after* it
sets a live channel to `CS_HANGUP`, so any normal Hangup against real
FreeSWITCH reaches this branch and returns `FAILED_PRECONDITION` after
already hanging up the call. The success path and audit are therefore
skipped for live channels; check state before calling or treat the
first hangup call as OK.

**Fix sketch.** Read `switch_channel_get_state(ch)` BEFORE
`switch_channel_hangup()`. If already `>= CS_HANGUP`, return
`FAILED_PRECONDITION` (truly already dead). Otherwise call hangup and
return OK. Audit and log on the success path.

### P1-3 — Count real HangupMany successes

`src/control/handlers/hangup_many_handler.cc:68-74`

Same FS return semantics: a live channel that was just hung up returns
`CS_HANGUP`, so `HangupOne` reports false and
`HangupManyResponse.hungup_uuids` stays empty even though the calls
were hung up. In real batches this makes every successful hangup look
like a skipped/already-dead UUID.

**Fix sketch.** Mirror P1-2 — pre-read state, only flip to dead AFTER
verifying it was alive at call time. Then count it in `hungup_uuids`.

### P1-4 — Reject risky Execute apps by default

`src/control/handlers/execute_handler.cc:61-69`

When Execute is exposed with this fixed allow-list, allowing
`transfer`, `bridge`, and `play_and_get_digits` lets callers drive
destinations/contexts or read channel variables through raw dialplan
args, bypassing the dedicated Bridge/Transfer validation and the
documented default exclusions for these risky apps. Exclude them by
default or add the required per-app validation before invoking FS.

**Fix sketch.** Drop `transfer`, `bridge`, `play_and_get_digits` from
the V1 allow-list (operators who need them call Bridge / BlindTransfer
RPCs, which apply state and target validation). Keep `playback`,
`set`, `hangup`, `answer`. If `play_and_get_digits` is needed, ship it
as a dedicated RPC in V2 with arg-shape validation.

### P1-5 — Enforce reserved variable denylist

`src/control/handlers/set_variables_handler.cc:95-97`

With only character validation, security-sensitive names such as
`api_on_answer`, `exec_after_bridge_app`,
`bridge_pre_execute_bleg_app`, or `sip_h_X` pass and are written to
the channel. The control API spec requires rejecting these reserved
prefixes; otherwise SetVariables can install FS hooks or SIP headers
that bypass policy.

**Fix sketch.** Add a denylist of prefixes:
`api_`, `exec_`, `bridge_pre_execute_`, `bridge_post_bridge_`,
`hangup_after_bridge_`, `sip_h_`, `_record_`, `record_`, `wait_for_`,
`api_on_*`. Reject with `INVALID_ARGUMENT` before any FS call.

---

## P2 — critical (functional / security)

### P2-6 — Keep ownership of originate variables

`src/control/handlers/originate_handler.cc:90-92`

`switch_ivr_originate` does not take ownership of caller-supplied
`ovars`; it destroys only internally-created or duplicated events
where `var_event != ovars`. Releasing the event here leaks every
OriginateRequest that includes variables after the synchronous call
returns, so keep it owned by `OriginateOptions` or destroy the
released event after the call.

**Fix sketch.** Keep `OriginateOptions` owning the `switch_event_t*`
via RAII; let its destructor `switch_event_destroy` it after
`switch_ivr_originate` returns. Do NOT `.release()` it.

### P2-7 — Populate BridgeResponse on success

`src/control/handlers/bridge_handler.cc:152-152`

On successful Bridge calls, `resp->bridged_uuid` is never set even
though the proto/spec define it as the bridged B-leg UUID. Clients
that rely on the response to confirm or resume a bridge receive an
empty string despite an OK status.

**Fix sketch.** `resp->set_bridged_uuid(req->leg_b_uuid())` on the
success path (per proto comment, the field is the "bridged B-leg").

### P2-8 — Redact keys that contain secret words

`src/control/handlers/execute_handler.cc:92-93`

The regex only redacts keys exactly named `password`, `token`, or
`secret`; common keys such as `api_secret_key=...` or
`password_hash=...` are emitted unchanged in `args_redacted`. Since
these args are audited, match the full key up to `=` and redact
whenever it contains a sensitive substring.

**Fix sketch.** Pattern becomes
`R"((\S*?(?:password|token|secret)\S*)=(\S+))"` (icase). Then
`$1=[REDACTED]` replaces value when key contains any of the words
anywhere.

### P2-9 — Apply Hangup variables before hanging up

`src/control/handlers/hangup_handler.cc:72-73`

The `variables` map in `HangupRequest` is documented to be set before
hangup for CDR/on_reporting enrichment, but this handler goes straight
from parsing the cause to `ChannelHangup`. Any request that supplies
variables loses them before FreeSWITCH emits the hangup/reporting
events.

**Fix sketch.** After SessionGuard locate + channel non-null, iterate
`req->variables()` and call
`osw::raii::fs::ChannelSetVariable(ch, k, v)` (the W3C helper) for
each pair, THEN call `ChannelHangup`.

---

## Summary counts

| Severity | Count | Fixed so far |
|----------|-------|--------------|
| P1       | 5     | 1 (P1-1)     |
| P2       | 4     | 0            |
| **Total** | **9** | **1**        |

P1-1 closed in `fix/mod-link-whole-archive`. The remaining 8 land in
the W3 fix sprint that follows this review.
