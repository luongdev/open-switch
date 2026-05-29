# Security + eavesdrop policy

## Scope

This document covers:

1. The threat model of the module (what we defend against).
2. Authentication + authorization for the control plane.
3. The **eavesdrop policy on bot-participating calls** — the specific
   ask from the project owner about supervisors / agents listening in
   on a call that has an AI bot in it.
4. Audit event emission for security-relevant operations.

Cryptography for the event transport layer is covered in
[`transport-adr.md`](transport-adr.md) (gRPC mTLS for
`SubscribeEvents`; there is no in-module broker after F0).
Cryptography for the media transport layer (gRPC mTLS) is covered
here and reused from the control plane.

## Threat model

```text
Trust boundaries:

  Internet  ┌───────────────────────┐  LAN          ┌─────────────┐
   ─────────┤ Public SIP / WebRTC    │ ─────────────┤ Operator     │
            │ peers (callers, trunks)│              │ orchestrator │
            └────────┬───────────────┘              └──────┬───────┘
                     │ SIP/RTP                              │ gRPC mTLS
                     ▼                                      ▼
                ┌────────────────────────────────────────────────────┐
                │           FreeSWITCH process                       │
                │  ┌──────────────────────────────────────────────┐  │
                │  │      mod_open_switch.so                      │  │
                │  │  - control gRPC server (:50061)              │  │
                │  │  - event_bind callbacks                       │  │
                │  │  - media bug callbacks                        │  │
                │  │  - SubscribeEvents broadcaster                │  │
                │  └──────────────────────────────────────────────┘  │
                └────────────────────────────────────────────────────┘
                     │                       │              │
                     ▼ RTP                   ▼ gRPC mTLS    ▼ gRPC mTLS
              ┌─────────────┐         ┌──────────────────┐  ┌────────────────────┐
              │ External    │         │ Upstream         │  │ Event subscribers  │
              │ caller PSTN │         │ TTS/STT/Voicebot │  │ (operator-owned;   │
              └─────────────┘         └──────────────────┘  │ persist as chosen) │
                                                            └────────────────────┘
```

There is no broker (Redis, Kafka, etc.) inside the trust boundary
of the FreeSWITCH process. Subscribers connecting to
`SubscribeEvents` decide their own persistence target on their
side.

Adversaries by source and goal:

| Adversary | Goal | Defense |
|---|---|---|
| Public SIP peer | Crash FS / leak memory via malformed signaling | FS-side defense (we don't see SIP directly) |
| Public SIP peer | Get bot to say sensitive thing via injection | Tenant-scoped ACLs at orchestrator level; module doesn't add new attack surface here |
| Network attacker (LAN) | Read call audio in transit | gRPC mTLS for upstream, FS handles SRTP/SIPS for media legs |
| Network attacker (LAN) | Forge control commands | gRPC mTLS + API key + tenant ACL |
| Network attacker (LAN) | Subscribe to event stream without authorization | gRPC mTLS + API key on `SubscribeEvents`; subscriber identity logged at stream open and on `osw::audit::event_subscriber_connected`. No broker-side ACL surface to harden because there is no in-module broker. |
| Compromised tenant credentials | Originate fraudulent calls | Per-tenant rate limits + dial-context allow-list + audit log |
| Compromised tenant credentials | Eavesdrop on other tenants' calls | Per-tenant scoping on `eavesdrop` + `Bridge` commands; cross-tenant ops rejected |
| Insider (supervisor with valid creds) | Listen to bot calls bypassing policy | Eavesdrop guard (this doc); audit events go to Tier 1 |
| Compromised module | RCE on FS host | Reduce blast radius: drop privileges (FS already runs as `freeswitch` user), mTLS only outbound, memory-safe wrappers (see [`memory-management.md`](memory-management.md)) |

## Control plane: authentication + authorization

### Transport: gRPC over TLS / mTLS

Three modes, operator-configurable in `open_switch.conf.xml`:

| Mode | Server presents cert | Client presents cert | When to use |
|---|---|---|---|
| `none` | no | no | NOT recommended; dev only. Module logs a `WARN` if production env detected (`FS_PROFILE != lan`). |
| `tls` | yes | no | Module verified by client. Client identified by API key only. |
| `mtls` | yes | yes | Module verifies client cert against `grpc_tls_ca_file`. Cert CN / SAN used as the client identity (logged in audit). |

mTLS is the recommended production posture.

### Auth: API key (with optional per-tenant scoping)

In addition to (or instead of) mTLS client cert, every RPC must
include metadata `x-osw-api-key: <value>`. The module compares:

1. Global key `grpc_api_key` in module settings (if set).
2. Per-tenant key in `<acl><tenant>` block — must match
   `header.tenant_id` field of the request.

If a global key is set AND a tenant key matches, the tenant key wins
(more specific). If both fail, the RPC is rejected with
`UNAUTHENTICATED`.

### Authorization: per-tenant ACL

Each tenant has:

- `api_key` (or `api_key_hash` for hashed storage; default plain
  for simplicity, hash recommended for production).
- `allowed_contexts` — comma-separated FS dialplan contexts the
  tenant can target via Originate. Default: empty (no
  Originate allowed; operator must opt in).
- `allow_eavesdrop` — bool; if false, eavesdrop on bot calls is
  denied for this tenant regardless of module-default policy.
  Default: false.
- `rate_limit_per_second` — RPS cap on this tenant's control RPCs.
  Default: 100.

ACL is checked on every RPC after authentication. Failures emit an
audit event.

### Idempotency security

Idempotency cache is keyed by `(tenant_id, request_id)`. Even with
identical `request_id`, different tenants get different cache lines.
This prevents a tenant from replaying another tenant's request_id.

## Media plane: outbound gRPC mTLS

When the module connects to an upstream service (TTS/STT/voicebot),
TLS is recommended:

- `tls_ca_file` — to verify the upstream's cert.
- `tls_cert_file` + `tls_key_file` — module presents a client cert to
  the upstream service.
- `tls_server_name` — SNI / cert verification hostname.

Per-upstream config under `<media-upstreams>` in `open_switch.conf.xml`
(land with media-spec implementation phase).

## Event plane: subscriber identity

The module ships events only via gRPC SubscribeEvents (per the post-
Phase-1-fix-sprint design in
[`transport-adr.md`](transport-adr.md)). There is no Redis or other
in-module event transport in V1.

Each `SubscribeEvents` stream is subject to the same control-plane
auth (mTLS + API key) as any other RPC. The subscriber's identity
(from cert CN/SAN or API key alias) is logged at stream open and at
stream close, plus emitted as an audit Tier 1 event
`osw::audit::event_subscriber_connected`.

For per-tenant event scoping, subscribers filter `EventEnvelope.tenant_id`
on their side. The module does not currently restrict which tenants'
events a subscriber receives (V1.5 consideration).

## Eavesdrop policy on bot calls

### The problem

`mod_dptools` provides the `eavesdrop` app. A supervisor can dial
into the system and bridge to an active call as a silent listener
(or whisper-mode, or barge). Useful for QC, training, intervention.

When the active call has a BOT (our TTS injection), eavesdropping
the call means the supervisor hears:

- The caller's audio (potentially PII).
- The bot's audio (potentially business-sensitive: prompts revealed,
  branching logic inferred, scripts copied).

Risks:

1. **PII leak**: bot recites credit card / SSN / OTP.
2. **GDPR + recording-consent laws**: jurisdictions require notice +
   consent to listening to live conversations.
3. **Compliance**: PCI-DSS, HIPAA, etc.
4. **Business confidentiality**: bot prompt = trade secret.
5. **Insider risk**: supervisor goes rogue, listens in on customers.

### The policy

We provide three policy levels for "bot-call eavesdrop", configured
at module level and overridable per-tenant:

| Policy | Behavior on eavesdrop of bot call |
|---|---|
| `deny` (default) | Eavesdrop attempt fails; the eavesdropping channel is hung up. Audit event `osw::eavesdrop::denied` emitted at Tier 1. |
| `audit` | Eavesdrop succeeds; the eavesdropping channel proceeds. Audit event `osw::eavesdrop::audit` emitted at Tier 1 with supervisor identity. |
| `allow` | No enforcement. Audit event still emitted but at Tier 2. **NOT recommended.** |

Default for V1 is `deny` because the cost of an accidental data leak
is higher than the cost of restricting a feature.

### Implementation

#### Marking a call as bot-participating

When the module starts a TTS/voicebot media bug on a channel, it sets:

```cpp
switch_channel_set_variable(chan, "osw_bot_session", "true");
switch_channel_set_variable(chan, "osw_bot_purpose", "voicebot");  // or "tts"
switch_channel_set_variable(chan, "osw_eavesdrop_policy", "deny"); // from tenant ACL
```

Channel variables are visible to other FS modules and dialplan.

#### Hooking eavesdrop

FreeSWITCH's `eavesdrop` app is in `mod_dptools`; it doesn't expose a
"pre-hook" we can plug into. **The prior draft of this section assumed
`mod_dptools::eavesdrop` sets a channel variable `eavesdrop_uuid` on
the eavesdropper channel; Phase 1 Codex finding C-3 verified against
FS source that this is FALSE.** The variable is never set. Any state
handler that reads it sees NULL and short-circuits. The previous
"Layer 2 hard enforcement" claim was a no-op.

The corrected enforcement strategy uses three complementary mechanisms.
None is bullet-proof in isolation; together they cover the realistic
operator scenarios.

**Layer 1 — Pre-app check via custom `osw_eavesdrop` app (PRIMARY)**:

The module registers its own dialplan application `osw_eavesdrop` that
wraps and replaces `eavesdrop` for bot-tenant dialplans. Behaviour:

```cpp
SWITCH_STANDARD_APP(osw_eavesdrop_function) {
  try {
    if (zstr(data)) {
      // data is the target UUID, same arg as mod_dptools::eavesdrop
      return;
    }
    osw::SessionLock target(data);
    if (!target) {
      osw::log::Warn("osw_eavesdrop: target {} not found", data);
      return;
    }
    const char* policy = switch_channel_get_variable(
        target.channel(), "osw_eavesdrop_policy");

    if (policy && std::string_view(policy) == "deny") {
      osw::events::EmitEavesdropAudit(
          session, target.channel(), "deny", /*decision=*/"hangup");
      switch_channel_hangup(
          switch_core_session_get_channel(session),
          SWITCH_CAUSE_POLICY_REJECTED);
      return;
    }
    if (policy && std::string_view(policy) == "audit") {
      osw::events::EmitEavesdropAudit(
          session, target.channel(), "audit", /*decision=*/"permitted");
    }
    // Delegate to FS native eavesdrop semantics via the public IVR API.
    switch_ivr_eavesdrop_session(
        session, data, /* require_group */ nullptr, ED_DEFAULT);
  } catch (const std::exception& e) {
    osw::log::Error("osw_eavesdrop: exception: {}", e.what());
  } catch (...) {
    osw::log::Error("osw_eavesdrop: unknown exception");
  }
}
```

Operator dialplans for bot tenants use `<action application="osw_eavesdrop"
data="${target_uuid}"/>` instead of `<action application="eavesdrop"
.../>`. This is the "primary enforcement" point — the policy check
happens BEFORE any eavesdrop bug is attached, so denying is clean
(no in-flight bug to dismantle).

The recommended (and the operator-hardening-checklist-mandatory)
posture: **ACL-restrict `eavesdrop` per tenant** so bot-tenant
dialplans cannot call raw `eavesdrop` at all. FS allows this via the
dialplan ACL (operator config).

**Layer 2 — `MEDIA_BUG_START` detector + fail-closed deny backstop**:

If an operator forgets Layer 1 or uses raw `eavesdrop` from a
non-bot-tenant dialplan that targets a bot channel, the module
detects the eavesdrop bug post-attach via the FS event bus and
**emits a Tier-1 audit event**. For `deny`, it also fails closed by
hanging up the bot-marked target channel immediately after audit. It
does NOT attempt to remove the FS-native eavesdrop bug. The reasons
are documented honestly below; the round-2 design that called
`switch_core_media_bug_remove_callback` from an event handler is
fundamentally unsound against vanilla FS v1.10.12 and is removed.

**Trigger.** The module subscribes to `SWITCH_EVENT_MEDIA_BUG_START`
events (FF-011 — `src/switch_core_media_bug.c:1014-1019` fires this
event every time `switch_core_media_bug_add` succeeds, with a
`Media-Bug-Function` header carrying the FS-internal function name).
The round-2 spec named `CHANNEL_CALLSTATE` as the trigger; that was
factually wrong (FF-005: `CHANNEL_CALLSTATE` fires on call-state
transitions only, not on bug attach).

**Handler.**

```cpp
void OnMediaBugStart(switch_event_t* ev) {
  try {
    const char* bug_fn = switch_event_get_header(ev, "Media-Bug-Function");
    if (!bug_fn || strcmp(bug_fn, "eavesdrop") != 0) return;

    const char* uuid = switch_event_get_header(ev, "Unique-ID");
    if (!uuid) return;
    osw::SessionLock sess(uuid);
    if (!sess) return;

    auto* chan = sess.channel();
    const char* osw_marked = switch_channel_get_variable(chan, "osw_bot_session");
    if (!osw_marked) return;  // not a bot call; ignore

    // Audit: an eavesdrop bug has just attached to a bot-marked target.
    // The bug is already in the chain by the time this event fires;
    // for deny policy we fail closed by hanging up the target channel.
    // We still rely on Layer 1 (osw_eavesdrop) / Layer 3
    // (raw-eavesdrop ACL) for clean pre-attach prevention.

    const char* policy = switch_channel_get_variable(chan, "osw_eavesdrop_policy");
    if (!policy) policy = "deny";
    const char* target_tenant =
        switch_channel_get_variable(chan, "osw_tenant");
    const char* eavesdropper_uuid =
        switch_event_get_header(ev, "Media-Bug-Target");
    // FS sets Media-Bug-Target to the `uuid` argument of
    // switch_core_media_bug_add, which for eavesdrop is the target
    // UUID — useful for cross-referencing.

    osw::events::EmitEavesdropAuditByDetection(
        /*target_chan=*/chan,
        /*policy=*/policy,
        /*eavesdropper_hint=*/eavesdropper_uuid,
        /*target_tenant=*/target_tenant);
    if (policy == "deny") {
      switch_channel_hangup(chan, SWITCH_CAUSE_CALL_REJECTED);
    }
  } catch (...) {
    osw::log::Error("OnMediaBugStart: unknown exception");
  }
}
```

**Why no bug removal:**

1. **The `eavesdrop_callback` symbol is `static` in
   `src/switch_ivr_async.c:2000`** (FF-003). Our module cannot
   acquire its address — neither at link time (linker can't see
   `static`) nor at run time (`dlsym(RTLD_DEFAULT, "eavesdrop_callback")`
   returns NULL). Any call to
   `switch_core_media_bug_remove_callback(sess, &eavesdrop_callback)`
   in our module is uncompilable / unimplementable.
2. **Even if we could reach the symbol**, FS's
   `switch_core_media_bug_remove_callback` gates removal on
   `cur->thread_id == switch_thread_self()` when the bug was added
   with `SMBF_THREAD_LOCK` (FF-002 — `src/switch_core_media_bug.c:
   1447` and lines 913-915). The FS-native eavesdrop bug is always
   attached with `SMBF_THREAD_LOCK` set (FF-003 excerpt:
   `read_flags | write_flags | SMBF_READ_PING | SMBF_THREAD_LOCK | ...`).
   Our event handler runs on an FS dispatch thread (FF-004), not
   the eavesdropper's dialplan thread; the thread-id check would
   skip the bug silently. The removal API would return success at
   the API level while doing nothing.
3. **Patching FS to either export the symbol or relax the
   thread-id check is feasible but out of scope for V1.** Carrying
   a custom FS patch is a sustained maintenance cost we are not
   taking on in V1. If it becomes necessary, it's a V1.5+ scope
   decision in its own OpenSpec change.

So Layer 2's honest contract is:

- **Detects** every FS-native eavesdrop attach to a bot-marked
  session. The detection window is one FS event-dispatch round-
  trip from the moment `switch_core_media_bug_add` calls
  `switch_event_fire(MEDIA_BUG_START)` to the moment our bound
  callback runs. Typical latency: sub-millisecond to single-digit
  milliseconds under load.
- **Emits a Tier-1 audit event** with target channel, target
  tenant, eavesdropper hint, and policy at detection time.
- **Does NOT remove the FS-native bug or hang up the eavesdropper.**
  For `deny`, Layer 2 fails closed by hanging up the bot target
  channel immediately after audit because vanilla FS does not
  expose a reliable module-side way to remove that bug post-attach.
  For `audit` / `allow`, it records the post-attach event and lets
  the eavesdrop continue.

Operators relying on Layer 2 alone are getting a fail-closed
backstop for deny policy, not a clean pre-attach rejection: a small
post-attach audio window exists before the target channel is hung
up. The hardening checklist treats Layer 1 (`osw_eavesdrop`
adoption) and Layer 3 (raw-`eavesdrop` ACL deprecation) as
MANDATORY for any tenant with a deny policy.

**Layer 3 — Dialplan ACL recommendation (DEFENSE-IN-DEPTH)**:

Operator hardening doc recommends:

```xml
<!-- Reject raw 'eavesdrop' app from bot-tenant contexts -->
<context name="bot-tenant-acme">
  <extension name="block-raw-eavesdrop">
    <condition field="${current_application}" expression="^eavesdrop$">
      <action application="log" data="WARNING raw eavesdrop attempted"/>
      <action application="hangup" data="POLICY_REJECTED"/>
    </condition>
  </extension>
  <!-- The rest of the dialplan; uses osw_eavesdrop where needed -->
</context>
```

This catches operator typos that would otherwise rely on Layer 2's
detection.

### Limitations (honest disclosure)

- **Layer 1 requires operator adoption**: if the operator doesn't
  install the `osw_eavesdrop` app or doesn't update the dialplan,
  Layer 1 doesn't run. Operator-hardening checklist enforces this.
- **Layer 2 is post-attach at v1.10.12**. The module emits an
  audit event when an FS-native `eavesdrop` bug attaches to a
  bot-marked session. For `deny`, it then hangs up the bot target
  channel as a fail-closed backstop; it still cannot remove the bug
  itself or hang up the eavesdropper reliably from outside the
  eavesdropper's dialplan thread. Per FF-002 (thread-id gate) and
  FF-003 (static `eavesdrop_callback`), removal from outside that
  thread is unimplementable against vanilla FS v1.10.12. Round 2's
  "Layer 2 removes the bug on policy=deny" design was factually
  unsound and has been removed.
  - Audio can flow from the moment of attach until the target
    channel hangup is processed, so Layer 2 is not a substitute for
    pre-attach denial.
  - The audit record is the forensic / SIEM signal; `deny` also
    triggers the target-channel fail-closed action.
  - For true real-time prevention, Layer 1 + Layer 3 are MANDATORY.
- **Both layers can be bypassed** by an operator with root or by an
  FS admin using `uuid_audio` / `uuid_bug` / custom modules. We
  treat that as out-of-scope (host security is the operator's
  responsibility).
- **The previous draft of this section is preserved in git history
  for traceability of the C-3 / N2 corrections**. The accumulated
  diff shows: round-1 design was "state handler reads
  `eavesdrop_uuid`" (variable never set by FS, no-op); round-2
  design was "CHANNEL_CALLSTATE handler removes the bug" (event
  doesn't fire on attach AND removal is gated by thread-id and
  symbol linkage); round-3 design is "MEDIA_BUG_START handler
  emits audit only".

### What the audit event contains

The event subclass differs by which layer emitted it:

- `osw::eavesdrop::denied` — Layer 1 (`osw_eavesdrop` app) rejected
  the attempt with policy=deny. The eavesdropper was hung up with
  `POLICY_REJECTED`. **No bug attached, no audio leaked.**
- `osw::eavesdrop::audit` — Layer 1 allowed the attempt with
  policy=audit; eavesdrop proceeded normally. Audio is flowing
  legitimately under audit.
- `osw::eavesdrop::allowed` — Layer 1 allowed the attempt with
  policy=allow.
- `osw::eavesdrop::detected_post_attach` — **Layer 2** detected an
  FS-native eavesdrop bug attach via `MEDIA_BUG_START` on a
  bot-marked session that did not go through Layer 1. The bug is
  already attached and audio is flowing. The audit record exists;
  module-side prevention does NOT.

```
Event-Name: CUSTOM
Event-Subclass: osw::eavesdrop::denied
                  | osw::eavesdrop::audit
                  | osw::eavesdrop::allowed
                  | osw::eavesdrop::detected_post_attach
Unique-ID: <eavesdropper-channel-uuid OR target-channel-uuid for layer-2>
target_uuid: <target-channel-uuid>
target_tenant: <target-tenant-id>
target_bot_purpose: <"voicebot" | "tts" | ...>
supervisor_identity: <if known: SIP From URI / cert CN / API actor>
supervisor_ip: <SIP source IP, if known>
policy_applied: <"deny" | "audit" | "allow">
decision: <"hangup" | "permitted" | "detected_only" | "detected_hangup_target">
emitted_at: <timestamp>
layer: <"1_pre_attach" | "2_post_attach_detection">
```

Tier 1 (durable, billing-grade audit) for all four subclasses. If
the operator's tenant ACL sets `allow_eavesdrop=true`, the
`policy_applied` is the more permissive of (module-default,
tenant-allow), and the audit still fires.

### What we explicitly do not protect against

- **Operator-side dialplan that calls raw `eavesdrop` on a
  bot-marked session**: Layer 2 detects the bug attach via
  `MEDIA_BUG_START` and emits a Tier-1 audit event with the policy
  applied. **The bug is NOT removed** — see the Layer-2 section
  above for why removal is unimplementable against vanilla FS
  v1.10.12. For `deny`, the module hangs up the bot target channel
  after audit as a fail-closed backstop; audio can still be exposed
  during the post-attach event-dispatch window. Operators relying
  on this prevention scenario MUST adopt Layer 1 (`osw_eavesdrop`
  app) and Layer 3 (raw-`eavesdrop` ACL block) — the hardening
  checklist treats both as MANDATORY for any tenant with a deny
  policy.
- **Operator-side dialplan that REMOVES the
  `osw_eavesdrop_policy` variable before eavesdrop**: in principle
  possible. We treat this as malicious operator action and don't try
  to defend. The audit emitted by Layer 2 will fall back to the
  module default policy in its `policy_applied` field, so the
  forensic trail still records what should have happened.
- **FreeSWITCH-internal eavesdrop via `uuid_audio` or `uuid_bug`**:
  these advanced commands attach bugs whose `Media-Bug-Function` is
  `"audio_buffer"` / `"uuid_bug"` etc., not `"eavesdrop"`. Our
  Layer 2 string-matches on `"eavesdrop"` specifically and ignores
  the rest. Defending against admin-side bug injection is
  out-of-scope (host security is the operator's responsibility).
- **Recording**: distinct from eavesdrop. Recording is governed by
  the operator's recording policy (start record_session or
  StartRecordingRelay). We don't gate that.

### Per-tenant override

In the tenant ACL block:

```xml
<acl>
  <tenant id="qc-team">
    <param name="api_key" value="qc-..."/>
    <param name="allow_eavesdrop" value="true"/>
    <param name="eavesdrop_policy" value="audit"/>
  </tenant>
</acl>
```

QC team can eavesdrop with audit. Other tenants get the module
default (`deny`).

### Testing

| Test | What it proves |
|---|---|
| `eavesdrop_denied_default` | Default policy: eavesdrop on bot call → eavesdropper hangs up with POLICY_REJECTED + audit event Tier 1 emitted |
| `eavesdrop_audit_mode` | Set policy=audit; eavesdrop succeeds + audit event Tier 1 emitted with supervisor identity |
| `eavesdrop_allow_mode` | Set policy=allow; eavesdrop succeeds + audit event Tier 2 emitted |
| `eavesdrop_non_bot_unaffected` | Eavesdrop on a call without bot → no policy enforcement, no audit event |
| `eavesdrop_layer1_osw_eavesdrop_deny` | Dialplan calls `osw_eavesdrop` on bot session with policy=deny: eavesdropper hangs up with POLICY_REJECTED + Tier 1 audit event before any bug attaches. |
| `eavesdrop_layer1_osw_eavesdrop_audit` | Same flow with policy=audit: eavesdrop proceeds (delegates to `switch_ivr_eavesdrop_session`) + Tier 1 audit event with supervisor identity. |
| `eavesdrop_layer2_media_bug_start_detected` | Dialplan uses raw `eavesdrop` on bot session: MEDIA_BUG_START handler matches `Media-Bug-Function == "eavesdrop"` on a session with `osw_bot_session=true`, emits Tier-1 audit event with policy at detection time, eavesdropper identity hint from `Media-Bug-Target`, and target tenant. For policy=deny, Layer 2 fails closed by hanging up the bot-marked target channel. Bug is NOT removed (FF-002 + FF-003 make removal unsound). |
| `eavesdrop_layer2_policy_audit_emits` | Raw `eavesdrop` + policy=audit: audit event fired with `policy_applied="audit"` and no fail-closed hangup. Prevention burden remains on Layer 1 / Layer 3 for tenants where audit is not acceptable. |
| `eavesdrop_layer2_no_remove_called` | Raw `eavesdrop` + policy=deny: assert the module does NOT call `switch_core_media_bug_remove_callback`; it uses target hangup as the fail-closed remediation. The remove-callback function exists in our compile unit only for module-owned bugs; FS-native eavesdrop bugs cannot be removed via this path (FF-002 thread-id gate + FF-003 static symbol). |
| `eavesdrop_var_removed_after_attach` | Operator removes `osw_eavesdrop_policy` channel variable after eavesdrop attaches. Layer 2 reads NULL → falls back to default policy (deny) in the emitted audit record. The audit record is the only enforcement signal at this layer; documented behaviour. |
| `tenant_override_allow` | Tenant ACL allow_eavesdrop=true overrides module default deny |
| `audit_event_routed_tier1` | Audit event lands on Tier 1 sink, not Tier 2 |
| `audit_supervisor_identity_logged` | When mTLS client cert is in use, CN/SAN is in audit event |

## Audit event taxonomy

All security-relevant events emit Tier 1 CUSTOM events with
`Event-Subclass` namespaced under `osw::audit::*`:

| Subclass | Trigger |
|---|---|
| `osw::audit::auth_failure` | RPC rejected by API key / mTLS |
| `osw::audit::acl_deny` | RPC rejected by per-tenant ACL (e.g., context not allowed) |
| `osw::audit::rate_limit` | RPC rejected by rate limiter |
| `osw::audit::originate` | Originate call (success or failure) |
| `osw::audit::hangup_admin` | Hangup invoked via gRPC (vs caller-initiated) |
| `osw::audit::config_reload` | SIGHUP / hot reload |
| `osw::audit::module_load` / `module_unload` | Module lifecycle |
| `osw::eavesdrop::denied` / `audit` / `allowed` / `detected_post_attach` | See above |
| `osw::audit::event_subscriber_connected` | New `SubscribeEvents` stream opened; carries authenticated identity + filter scope |
| `osw::audit::event_subscriber_disconnected` | Stream closed (graceful or kicked); carries the kick reason if applicable |

The audit event payload includes (where applicable):

- Authenticated identity (mTLS CN, API key alias — never the key itself).
- Source IP / port.
- Tenant ID.
- Target UUID(s).
- Request ID.
- Result code.

## Hardening checklist for operators

`docs/HARDENING.md` (lands with Phase 2) will codify:

- [ ] mTLS enabled for control gRPC (and `SubscribeEvents`, which
      shares the same listener); client certs distributed via PKI.
- [ ] API keys rotated quarterly.
- [ ] Subscriber-side persistence target (Kafka, Redis Streams,
      S3, file) is hardened to the operator's standard for that
      store — these stores are OUTSIDE the module's trust boundary
      and not covered by this checklist beyond "they exist".
- [ ] Eavesdrop policy = deny module-default; per-tenant overrides
      reviewed quarterly.
- [ ] Audit events routed to immutable storage (S3 versioned bucket
      or similar) within 60 seconds of emission.
- [ ] FS host has SELinux / AppArmor profile restricting filesystem
      access.
- [ ] FreeSWITCH runs as non-root `freeswitch` user (default in our
      Docker images).
- [ ] Container runs with `--cap-drop=ALL` plus only the required
      caps from open-gateway README.
- [ ] Module Docker image is the official `ghcr.io/luongdev/open-switch`
      image, not a third-party rebuild.

## Threats accepted (residual risk)

- **Compromised FS host**: full game over. We accept this; the
  defense is host security, not module security.
- **Side-channel on bot-call audio** (e.g., a malicious bug attached
  via `uuid_audio_bug` by an admin): out of scope.
- **Timing attacks** on the API key comparison: V1 uses
  `CRYPTO_memcmp` (from OpenSSL) for API key comparison (Phase 1
  Codex finding N-4); mTLS is preferred over API-key-only. The
  implementation MUST NOT roll its own constant-time comparison.
- **Replay of audit events** by a malicious subscriber-side consumer:
  the module does not sign envelopes in V1. Subscribers that
  re-emit envelopes downstream MUST verify the original gRPC
  connection identity at their boundary. Recommended posture:
  consumer-side dedup by `event_id` plus subscriber identity
  pinning in the operator's downstream audit pipeline.

## Future work (V1.5+)

- Signed event envelopes (HMAC with rotating key) for tamper-evident
  audit trails.
- OAuth2 / OIDC for control plane (instead of API keys).
- Per-method gRPC ACL (e.g., tenant A can Originate but not Hangup).
- Anomaly detection on eavesdrop frequency per supervisor.
- Audit-log streaming to SIEM via syslog/RFC 5424 in addition to
  the gRPC `SubscribeEvents` stream (today operators run a small
  subscriber that forwards Tier-1 events to syslog).
