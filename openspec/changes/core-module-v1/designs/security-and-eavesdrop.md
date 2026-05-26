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
[`transport-adr.md`](transport-adr.md) (Redis TLS + ACL).
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
                │  └──────────────────────────────────────────────┘  │
                └────────────────────────────────────────────────────┘
                     │                                      │
                     ▼ RTP                                  ▼ gRPC mTLS
              ┌─────────────┐                       ┌──────────────────┐
              │ External    │                       │ Upstream         │
              │ caller PSTN │                       │ TTS/STT/Voicebot │
              └─────────────┘                       └──────────────────┘
                                                            │
                                                            ▼ Redis TLS
                                                     ┌────────────────┐
                                                     │ Redis cluster  │
                                                     └────────────────┘
```

Adversaries by source and goal:

| Adversary | Goal | Defense |
|---|---|---|
| Public SIP peer | Crash FS / leak memory via malformed signaling | FS-side defense (we don't see SIP directly) |
| Public SIP peer | Get bot to say sensitive thing via injection | Tenant-scoped ACLs at orchestrator level; module doesn't add new attack surface here |
| Network attacker (LAN) | Read call audio in transit | gRPC mTLS for upstream, FS handles SRTP/SIPS for media legs |
| Network attacker (LAN) | Forge control commands | gRPC mTLS + API key + tenant ACL |
| Network attacker (LAN) | Forge events into Redis | Redis ACL + TLS; module connects with auth credentials |
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

**Layer 2 — Bug-attach detector on bot-marked sessions (BACKSTOP)**:

If an operator forgets Layer 1 or uses raw `eavesdrop` from a
non-bot-tenant dialplan that somehow targets a bot channel, the module
detects the eavesdrop bug post-attach. Implementation:

The module subscribes to `CHANNEL_CALLSTATE` events (which FS fires
when a channel changes state, including when it becomes a SIP B-leg
to an eavesdrop session). On each event for a bot-marked target
session, the module walks `session->bugs` (via
`switch_core_media_bug_pop` is not safe; we use a read-only walker
or `switch_core_media_bug_count` to detect a bug whose function name
is `"eavesdrop"`):

```cpp
void OnChannelCallstateEvent(switch_event_t* ev) {
  try {
    const char* uuid = switch_event_get_header(ev, "Unique-ID");
    if (!uuid) return;
    osw::SessionLock sess(uuid);
    if (!sess) return;
    auto* chan = sess.channel();
    const char* osw_marked = switch_channel_get_variable(chan, "osw_bot_session");
    if (!osw_marked) return;  // not a bot call; skip

    // Is there an eavesdrop bug on this channel right now?
    // switch_core_media_bug_count counts bugs by 'function' name.
    int count = switch_core_media_bug_count(sess.get(), "eavesdrop");
    if (count == 0) return;  // no eavesdrop attached

    const char* policy = switch_channel_get_variable(chan, "osw_eavesdrop_policy");
    if (!policy) policy = "deny";  // default

    osw::events::EmitEavesdropAuditByDetection(chan, policy, count);

    if (std::string_view(policy) == "deny") {
      // Cannot safely hangup the eavesdropper's session from here
      // (we don't have its UUID directly — the bug callback owns it,
      //  and racing the bug's own teardown can crash FS). We DO
      // hangup the target's eavesdrop relationship by removing the
      // bug:
      switch_core_media_bug_remove_callback(
          sess.get(), eavesdrop_callback_fn);
      // This causes the eavesdrop bug's callback to receive CLOSE
      // and the eavesdropper session naturally exits the eavesdrop
      // app, returning to its dialplan. FS handles teardown.
    }
  } catch (...) {
    osw::log::Error("OnChannelCallstateEvent: unknown exception");
  }
}
```

`switch_core_media_bug_remove_callback` is the safe way to detach a
bug by callback function — it doesn't race the in-flight callback
because FS holds the channel media lock during removal.

This is the detection-and-disconnect backstop. Slower than Layer 1
(reactive, after attach), but covers the case where an operator
misses the `osw_eavesdrop` adoption.

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
- **Layer 2 is reactive**: there's a brief window (one event
  callback round-trip, typically < 50 ms) between bug attach and
  policy enforcement. An audit event records the attempt, but the
  eavesdropper may have heard up to ~50 ms of audio.
- **Both layers can be bypassed** by an operator with root or by an
  FS admin using `uuid_audio` / `uuid_bug` / custom modules. We
  treat that as out-of-scope (host security is the operator's
  responsibility).
- **The previous draft of this section is preserved in git history
  for traceability of the C-3 correction**.

### What the audit event contains

```
Event-Name: CUSTOM
Event-Subclass: osw::eavesdrop::denied   (or ::audit, or ::allowed)
Unique-ID: <eavesdropper-channel-uuid>
target_uuid: <target-channel-uuid>
target_tenant: <target-tenant-id>
target_bot_purpose: <"voicebot" | "tts" | ...>
supervisor_identity: <if known: SIP From URI / cert CN / API actor>
supervisor_ip: <SIP source IP>
policy_applied: <"deny" | "audit" | "allow">
decision: <"hangup" | "permitted">
emitted_at: <timestamp>
```

Tier 1 (durable, billing-grade audit). If the operator's tenant ACL
sets `allow_eavesdrop=true`, the policy_applied is the more
permissive of (module-default, tenant-allow), and the audit still
fires.

### What we explicitly do not protect against

- **Operator-side dialplan that calls raw `eavesdrop`**: Layer 2
  (bug-attach detector) catches this within ~50 ms of attach;
  audit event fires; bug is removed if policy=deny. The brief
  pre-detection window is documented in Limitations above.
- **Operator-side dialplan that REMOVES the
  `osw_eavesdrop_policy` variable before eavesdrop**: in principle
  possible. We treat this as malicious operator action and don't try
  to defend.
- **FreeSWITCH-internal eavesdrop via `uuid_audio` or `uuid_bug`**:
  these advanced commands can attach to a channel. They are typically
  used by admins, not exposed externally. We don't try to defend
  against the FS admin who has full local access.
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
| `eavesdrop_layer2_raw_eavesdrop_detected` | Dialplan uses raw `eavesdrop` on bot session: CHANNEL_CALLSTATE handler detects bug within 50 ms, emits Tier 1 audit, removes bug via `switch_core_media_bug_remove_callback` if policy=deny. |
| `eavesdrop_layer2_policy_audit_no_remove` | Raw `eavesdrop` + policy=audit: bug stays attached; audit event fired; no remove call. |
| `eavesdrop_var_removed_after_attach` | Operator removes `osw_eavesdrop_policy` channel variable after eavesdrop attaches. Layer 2 reads NULL → falls back to default policy (deny). Documented behaviour. |
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
| `osw::eavesdrop::denied` / `audit` / `allowed` | See above |
| `osw::audit::redis_credential_used` | First successful Redis auth |

The audit event payload includes (where applicable):

- Authenticated identity (mTLS CN, API key alias — never the key itself).
- Source IP / port.
- Tenant ID.
- Target UUID(s).
- Request ID.
- Result code.

## Hardening checklist for operators

`docs/HARDENING.md` (lands with Phase 2) will codify:

- [ ] mTLS enabled for control gRPC; client certs distributed via PKI.
- [ ] API keys rotated quarterly.
- [ ] Redis ACL minimised (XADD/PUBLISH only); separate user.
- [ ] Redis TLS in cross-host setups.
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
  `constant_time_compare` for API key strings; mTLS is preferred over
  API-key-only.
- **Replay of audit events** by a Redis-stream consumer impersonator:
  consumers must verify the producer; we don't sign envelopes V1.
  Recommended posture: read-only Redis ACL for consumers, plus
  consumer-side dedup by `event_id`.

## Future work (V1.5+)

- Signed event envelopes (HMAC with rotating key) for tamper-evident
  audit trails.
- OAuth2 / OIDC for control plane (instead of API keys).
- Per-method gRPC ACL (e.g., tenant A can Originate but not Hangup).
- Anomaly detection on eavesdrop frequency per supervisor.
- Audit-log streaming to SIEM via syslog/RFC 5424 in addition to Redis.
