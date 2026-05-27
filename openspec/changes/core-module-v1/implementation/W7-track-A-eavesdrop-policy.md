# W7 Track A — Eavesdrop policy (Layer 1 + Layer 2 + channel-var marker)

## Owner / model

Sonnet sub-agent. Implementation work is mechanical compared to the
design (which is fully captured in
[`designs/security-and-eavesdrop.md`](../designs/security-and-eavesdrop.md)).
The sub-agent reads this brief as its authoritative scope; it does
NOT redesign anything.

## Scope

Implement the V1 eavesdrop policy on bot-participating calls. Three
mechanisms work together:

1. **Layer 1 — `osw_eavesdrop` dialplan app (PRIMARY enforcement)**:
   replaces raw `eavesdrop` for bot-tenant dialplans. Reads the target
   channel's `osw_eavesdrop_policy` variable, hangs up on `deny`,
   audits on `audit`, delegates to `switch_ivr_eavesdrop_session` on
   `audit` / `allow`. Emits Tier-1 audit before any bug attaches.
2. **Layer 2 — `MEDIA_BUG_START` detector (DETECTION-ONLY backstop)**:
   binds the FS event; if the bug function is `"eavesdrop"` and the
   target channel has `osw_bot_session=true`, emits a Tier-1 audit
   event. Does NOT remove the bug — see "Why detection only" below.
3. **Channel-var marker on `StartTts` / `StartVoicebot`**: when one of
   the two RPCs attaches an INJECT bug, set the marker channel
   variables. This is what Layer 1 / Layer 2 read.

Files in scope:

```text
include/osw/security/eavesdrop_app.h           (new — public API for app registration)
include/osw/security/eavesdrop_detector.h      (new — public API for MEDIA_BUG_START handler binding)
include/osw/security/eavesdrop_policy.h        (new — parse "deny"/"audit"/"allow" + helpers)
src/security/eavesdrop_app.cc                  (new — Layer 1 app implementation)
src/security/eavesdrop_detector.cc             (new — Layer 2 event handler)
src/security/eavesdrop_policy.cc               (new — string ↔ enum + tenant ACL lookup)
src/security/CMakeLists.txt                    (new — osw_security + osw_security_fs libs)

src/control/handlers/start_tts_handler.cc          (PATCH — set channel vars)
src/control/handlers/start_voicebot_handler.cc     (PATCH — set channel vars)

src/mod_open_switch.cc                         (PATCH — Load step + Shutdown step)

proto/open_switch/control/v1/config.proto      (PATCH — module-level eavesdrop_policy + per-tenant allow_eavesdrop)

include/osw/config/module_config.h             (PATCH — add EavesdropPolicy field + Validate clamp)
src/config/module_config.cc                    (PATCH — XML parse + Validate)

src/events/audit.cc                            (PATCH — register 4 new subclasses if not already wildcarded)

openspec/changes/core-module-v1/FREESWITCH-FACTS.md  (PATCH — add FF-035)

tests/unit/security/eavesdrop_policy_test.cc          (new)
tests/unit/security/eavesdrop_app_test.cc             (new — FS-mock)
tests/unit/security/eavesdrop_detector_test.cc        (new — FS-mock)
tests/integration/eavesdrop_layer1_deny.cc            (new)
tests/integration/eavesdrop_layer1_audit.cc           (new)
tests/integration/eavesdrop_layer2_detection.cc       (new)
tests/integration/eavesdrop_non_bot_unaffected.cc     (new)
tests/integration/eavesdrop_tenant_override.cc        (new)
```

All test files use the existing `osw_test_helpers` + FS-mock harness
established in W6 Track A.

## Spec deltas

### `module_config.proto` (add 2 fields)

```protobuf
message ModuleConfig {
  // ... existing fields ...

  // Module-default eavesdrop policy on bot-participating calls.
  // Tenants may override via per-tenant ACL block.
  // Allowed: "deny" (default) | "audit" | "allow"
  string eavesdrop_policy = 42;

  // ... existing ...
}

message TenantAcl {
  // ... existing api_key, allowed_contexts ...

  // Per-tenant override of eavesdrop_policy. If unset, the
  // module-default applies. If set to "allow" while the module
  // default is "deny", the tenant gets allow.
  string eavesdrop_policy_override = 7;

  // Short-circuit "allow eavesdrop entirely" toggle. If false,
  // every eavesdrop attempt on this tenant's bot calls is denied
  // regardless of policy field.
  bool allow_eavesdrop = 8;
}
```

### Audit subclasses (existing helper, new namespaces)

`osw::audit::Emit("osw.eavesdrop.{denied,audit,allowed,detected_post_attach}", ...)`
already routes via the Tier classifier; the four subclasses are added
to the **Tier-1 allowlist** in `src/events/tier.cc` so they don't
accidentally degrade to Tier 2 / Tier 3 under default classification.

### `FREESWITCH-FACTS.md` — new entry FF-035

The sub-agent writes a new FF entry in the existing style, covering:

```text
## FF-035 — Dialplan-app registration: SWITCH_ADD_APP + switch_application_interface_t

`mod_open_switch` registers `osw_eavesdrop` as a custom dialplan app
by populating a switch_application_interface_t pointer inside the
SWITCH_MODULE_LOAD entry point.  The macro SWITCH_ADD_APP wraps the
allocation + chaining; the resulting interface lives for the module's
lifetime in the module pool.

Allocation site: src/switch_loadable_module.c:* (cite line range from
the bundled FS source under tools/freeswitch-1.10.12-trixie).

Lifecycle:
  - Allocated from the module pool by SWITCH_ADD_APP.
  - Owned by the module; freed by FS when the module is unloaded.
  - The app's `application_function` is a C function pointer that FS
    invokes on the dialplan thread when the app is reached via
    <action application="..."/>.

Calling convention of application_function: SWITCH_STANDARD_APP
macro expands to (switch_core_session_t* session, const char* data),
where `data` is the raw text after the app name in the dialplan
action. For osw_eavesdrop, `data` is the target channel UUID.

Thread context: The application_function executes on the calling
channel's dialplan thread. switch_ivr_eavesdrop_session blocks that
thread for the duration of the eavesdrop session; the module's app
must NOT hold any cross-channel lock across the call.

Concurrency: Multiple dialplan threads may be in osw_eavesdrop
simultaneously (one per eavesdropping call). The implementation MUST
be re-entrant; mutexes for module-internal state are scoped to short
critical sections only.

Source citation: tools/freeswitch-1.10.12-trixie/src/include/switch_module_interfaces.h
(struct switch_application_interface) + src/switch_loadable_module.c
(SWITCH_ADD_APP expansion).

Used by: src/security/eavesdrop_app.cc registers osw_eavesdrop via
SWITCH_ADD_APP from SWITCH_MODULE_LOAD_FUNCTION.
```

The exact line citations are added by the sub-agent after grepping
the bundled FS source.

## Implementation steps

### 1. `EavesdropPolicy` enum + parser

`include/osw/security/eavesdrop_policy.h`:

```cpp
namespace osw::security {

enum class EavesdropPolicy : std::uint8_t {
    kDeny = 0,   // default
    kAudit,
    kAllow,
};

[[nodiscard]] EavesdropPolicy ParseEavesdropPolicy(std::string_view s) noexcept;
[[nodiscard]] std::string_view EavesdropPolicyName(EavesdropPolicy p) noexcept;

/// Resolves the effective policy for a given tenant.  Per-tenant
/// override wins over module-default; allow_eavesdrop=false forces
/// kDeny regardless of policy string.
[[nodiscard]] EavesdropPolicy ResolveEffectivePolicy(
    const ModuleConfig& cfg,
    std::string_view tenant_id) noexcept;

}  // namespace osw::security
```

`ParseEavesdropPolicy`:

- Case-insensitive match.
- Empty / unknown / NULL → `kDeny` (safe default).

`ResolveEffectivePolicy`:

- Look up tenant in `cfg.tenant_acls`.
- If found and `allow_eavesdrop == false` → `kDeny`.
- If found and `eavesdrop_policy_override` is non-empty → parse it.
- Else → parse `cfg.eavesdrop_policy` (module default).
- Always emit one log line at DEBUG with resolved policy + tenant +
  source ("tenant_override" / "module_default" / "deny_forced").

Test (`eavesdrop_policy_test.cc`):

- `ParseEavesdropPolicy("DENY") == kDeny`
- `ParseEavesdropPolicy("audit") == kAudit`
- `ParseEavesdropPolicy("Allow") == kAllow`
- `ParseEavesdropPolicy("") == kDeny`
- `ParseEavesdropPolicy("garbage") == kDeny`
- `ResolveEffectivePolicy(cfg{module="audit"}, "unknown_tenant") == kAudit`
- `ResolveEffectivePolicy(cfg{module="audit", tenant{allow_eavesdrop=false}}, "t") == kDeny`
- `ResolveEffectivePolicy(cfg{module="deny", tenant{override="allow", allow=true}}, "t") == kAllow`

### 2. Channel-var marker in `StartTts` + `StartVoicebot`

In both handlers, after `MediaBugManager::Attach` returns success:

```cpp
// Mark the channel as bot-participating for eavesdrop policy + Layer-2 detection.
const auto policy =
    osw::security::ResolveEffectivePolicy(*module_config_, req.header().tenant_id());

osw::raii::SessionLock target(req.channel_uuid());
if (target) {
    auto* chan = target.channel();
    switch_channel_set_variable(chan, "osw_bot_session", "true");
    switch_channel_set_variable(chan, "osw_bot_purpose",
                                /*StartTts:*/ "tts" /*StartVoicebot:*/ "voicebot");
    switch_channel_set_variable(chan, "osw_eavesdrop_policy",
                                std::string(osw::security::EavesdropPolicyName(policy)).c_str());
    switch_channel_set_variable(chan, "osw_tenant",
                                req.header().tenant_id().c_str());
}
```

Use `osw::raii::SessionLock` (already exists in `include/osw/raii/`)
so the session ref-lock is released RAII-style. The variable writes
are FS-thread-safe.

If the SessionLock fails (channel disappeared between Attach and now),
log a WARN and continue — the Attach already succeeded so the bug is
attached but the marker is missing; Layer 2 falls back to module
default in its audit emit.

Both handler patches share the same code; lift to a small helper in
`include/osw/security/eavesdrop_policy.h`:

```cpp
/// Sets osw_bot_session=true, osw_bot_purpose, osw_eavesdrop_policy,
/// osw_tenant on the target channel.  Idempotent.  No-op if the
/// session lookup fails.
void MarkBotSession(switch_core_session_t* session,
                    std::string_view purpose,
                    EavesdropPolicy policy,
                    std::string_view tenant_id);
```

`StartTts` calls `MarkBotSession(sess, "tts", policy, tenant)`;
`StartVoicebot` calls with `"voicebot"`.

Test (`eavesdrop_app_test.cc::MarkBotSessionSetsVariables`):

- FS-mock channel; call `MarkBotSession` → assert the four channel
  variables are set on the mock.

### 3. Layer 1 — `osw_eavesdrop` dialplan app

`src/security/eavesdrop_app.cc`:

```cpp
#include <switch.h>
#include "osw/security/eavesdrop_app.h"
#include "osw/security/eavesdrop_policy.h"
#include "osw/raii/session_lock.h"
#include "osw/events/audit.h"
#include "osw/log/log.h"

namespace osw::security {

namespace {

SWITCH_STANDARD_APP(OswEavesdropAppFunction) {
    try {
        if (!session) return;
        if (zstr(data)) {
            osw::log::Warn("osw_eavesdrop: missing target UUID");
            return;
        }

        const std::string target_uuid(data);
        osw::raii::SessionLock target(target_uuid);
        if (!target) {
            osw::log::Warn("osw_eavesdrop: target {} not found", target_uuid);
            return;
        }

        const char* policy_raw =
            switch_channel_get_variable(target.channel(), "osw_eavesdrop_policy");
        const char* tenant =
            switch_channel_get_variable(target.channel(), "osw_tenant");
        const char* purpose =
            switch_channel_get_variable(target.channel(), "osw_bot_purpose");
        const EavesdropPolicy effective =
            policy_raw ? ParseEavesdropPolicy(policy_raw) : EavesdropPolicy::kDeny;

        EmitEavesdropAudit(session, target.channel(), effective,
                           /*detected_at=*/"1_pre_attach",
                           /*decision=*/effective == EavesdropPolicy::kDeny
                                            ? "hangup"
                                            : "permitted");

        if (effective == EavesdropPolicy::kDeny) {
            switch_channel_hangup(switch_core_session_get_channel(session),
                                  SWITCH_CAUSE_POLICY_REJECTED);
            return;
        }

        // policy = audit | allow: delegate to FS's public eavesdrop IVR.
        switch_ivr_eavesdrop_session(session,
                                     target_uuid.c_str(),
                                     /*require_group=*/nullptr,
                                     ED_DEFAULT);
    } catch (const std::exception& e) {
        osw::log::Error("osw_eavesdrop: exception: {}", e.what());
    } catch (...) {
        osw::log::Error("osw_eavesdrop: unknown exception");
    }
}

}  // namespace

void RegisterOswEavesdropApp(switch_loadable_module_interface_t* module_interface) {
    switch_application_interface_t* app;
    SWITCH_ADD_APP(app, /*name=*/"osw_eavesdrop",
                   /*short_desc=*/"Bot-call eavesdrop with policy enforcement",
                   /*long_desc=*/"Wraps FS eavesdrop with policy check on bot-marked sessions.",
                   /*function=*/OswEavesdropAppFunction,
                   /*syntax=*/"<target-uuid>",
                   /*flags=*/SAF_NONE);
}

}  // namespace osw::security
```

`EmitEavesdropAudit` (helper in `src/security/eavesdrop_app.cc`)
builds and emits the audit event with the schema from
`security-and-eavesdrop.md` §"What the audit event contains":

```cpp
void EmitEavesdropAudit(switch_core_session_t* eavesdropper_session,
                       switch_channel_t* target_chan,
                       EavesdropPolicy policy,
                       std::string_view detected_at,
                       std::string_view decision) {
    const char* subclass = policy == EavesdropPolicy::kDeny ? "osw.eavesdrop.denied"
                           : policy == EavesdropPolicy::kAudit ? "osw.eavesdrop.audit"
                           : "osw.eavesdrop.allowed";
    // detected_at = "2_post_attach_detection" → subclass = "osw.eavesdrop.detected_post_attach"
    if (detected_at == "2_post_attach_detection") {
        subclass = "osw.eavesdrop.detected_post_attach";
    }

    osw::audit::Builder b(subclass, osw::events::Tier::k1Durable);
    b.Header("target_uuid", switch_channel_get_uuid(target_chan));
    b.Header("target_tenant",
             switch_channel_get_variable(target_chan, "osw_tenant"));
    b.Header("target_bot_purpose",
             switch_channel_get_variable(target_chan, "osw_bot_purpose"));
    b.Header("policy_applied",
             std::string(EavesdropPolicyName(policy)));
    b.Header("decision", std::string(decision));
    b.Header("layer", std::string(detected_at));
    if (eavesdropper_session) {
        const auto* eavesdropper_chan =
            switch_core_session_get_channel(eavesdropper_session);
        b.Header("supervisor_identity",
                 switch_channel_get_variable(eavesdropper_chan, "sip_from_uri"));
        b.Header("supervisor_ip",
                 switch_channel_get_variable(eavesdropper_chan, "sip_network_ip"));
        b.Header("Unique-ID", switch_channel_get_uuid(eavesdropper_chan));
    } else {
        b.Header("Unique-ID", switch_channel_get_uuid(target_chan));
    }
    osw::audit::Emit(std::move(b));
}
```

`b.Header(..., nullptr)` must be safe (the audit helper already
skips NULL values per W2). If not, the sub-agent adds a NULL guard
around each `Header` call.

Test (`eavesdrop_app_test.cc`):

- `Deny_HangsUpEavesdropper`: FS-mock target with `osw_bot_session=true`,
  `osw_eavesdrop_policy=deny`. Eavesdropper session passed to
  `OswEavesdropAppFunction("<target-uuid>")` → assert
  `switch_channel_hangup` was called with `POLICY_REJECTED` + assert
  audit emit with subclass `osw.eavesdrop.denied`.
- `Audit_DelegatesToFs`: same setup, policy=`audit` →
  `switch_ivr_eavesdrop_session` mock is called + audit emitted with
  subclass `osw.eavesdrop.audit`.
- `Allow_DelegatesToFs`: policy=`allow` → delegates + audit subclass
  `osw.eavesdrop.allowed`.
- `MissingData_NoOp`: pass empty string for `data` → no audit, no hangup.
- `TargetNotFound_NoOp`: pass UUID that the SessionLock can't resolve
  → no audit, no hangup, WARN logged.
- `NullSession_NoOp`: pass `nullptr` for session → no crash.

### 4. Layer 2 — `MEDIA_BUG_START` detector

`src/security/eavesdrop_detector.cc`:

```cpp
namespace osw::security {

namespace {

void OnMediaBugStart(switch_event_t* event, void* /*user_data*/) {
    try {
        if (!event) return;
        const char* bug_fn = switch_event_get_header(event, "Media-Bug-Function");
        if (!bug_fn || std::strcmp(bug_fn, "eavesdrop") != 0) return;

        const char* uuid = switch_event_get_header(event, "Unique-ID");
        if (!uuid) return;
        osw::raii::SessionLock target(uuid);
        if (!target) return;

        const char* marked =
            switch_channel_get_variable(target.channel(), "osw_bot_session");
        if (!marked || std::strcmp(marked, "true") != 0) return;

        const char* policy_raw =
            switch_channel_get_variable(target.channel(), "osw_eavesdrop_policy");
        const EavesdropPolicy effective =
            policy_raw ? ParseEavesdropPolicy(policy_raw) : EavesdropPolicy::kDeny;

        EmitEavesdropAudit(/*eavesdropper_session=*/nullptr,
                           target.channel(),
                           effective,
                           /*detected_at=*/"2_post_attach_detection",
                           /*decision=*/"detected_only");
    } catch (const std::exception& e) {
        osw::log::Error("eavesdrop_detector: exception: {}", e.what());
    } catch (...) {
        osw::log::Error("eavesdrop_detector: unknown exception");
    }
}

switch_event_node_t* g_node = nullptr;

}  // namespace

bool BindEavesdropDetector() {
    if (switch_event_bind_removable(/*id=*/"osw_eavesdrop_detector",
                                    SWITCH_EVENT_MEDIA_BUG_START,
                                    /*subclass_name=*/SWITCH_EVENT_SUBCLASS_ANY,
                                    OnMediaBugStart,
                                    /*user_data=*/nullptr,
                                    &g_node) != SWITCH_STATUS_SUCCESS) {
        osw::log::Error("eavesdrop_detector: bind failed");
        return false;
    }
    return true;
}

void UnbindEavesdropDetector() {
    if (g_node) {
        switch_event_unbind(&g_node);
        g_node = nullptr;
    }
}

}  // namespace osw::security
```

**Important.** Per `security-and-eavesdrop.md`, Layer 2 is
**DETECTION-ONLY**. The handler MUST NOT call
`switch_core_media_bug_remove_callback` on the eavesdrop bug — even
if the symbol were reachable, FF-002's thread-id gate would make it a
silent no-op (and the wrong code path could crash). The sub-agent
MUST NOT add a "removal path" here regardless of how tempting it is.

Audit emit MUST be cheap: classifier + ring enqueue. The Tier-1 ring
already exists (W2). Rate-limiting is delegated to a downstream sink;
the module does not rate-limit Layer-2 audits at emit time (every
attach to a bot session is a genuine event worth recording).

Test (`eavesdrop_detector_test.cc`):

- `IgnoresNonEavesdropBugs`: fire `MEDIA_BUG_START` with
  `Media-Bug-Function=stt_transcribe` → no audit emit.
- `IgnoresUnmarkedSessions`: fire with `Media-Bug-Function=eavesdrop`
  on a session without `osw_bot_session` variable → no audit emit.
- `EmitsAuditForBotMarked_DenyPolicy`: bot-marked + policy=deny → audit
  subclass `osw.eavesdrop.detected_post_attach` with
  `policy_applied=deny`.
- `EmitsAuditForBotMarked_AuditPolicy`: same flow + policy=audit →
  subclass `detected_post_attach` with `policy_applied=audit`.
- `EmitsAuditWhenPolicyVarMissing`: bot-marked but policy var absent
  → audit emitted with `policy_applied=deny` (safe default).
- `DoesNotRemoveBug`: assert (via mock) that
  `switch_core_media_bug_remove_callback` is NOT called.

### 5. Module wiring

`src/mod_open_switch.cc`:

```cpp
SWITCH_MODULE_LOAD_FUNCTION(mod_open_switch_load) {
    // ... existing steps 1-5 ...

    // === W7 Track A: register osw_eavesdrop dialplan app + MEDIA_BUG_START detector ===

    osw::security::RegisterOswEavesdropApp(*module_interface);  // *module_interface is out param

    if (!osw::security::BindEavesdropDetector()) {
        osw::log::Warn("Failed to bind MEDIA_BUG_START detector — Layer 2 disabled");
        // Non-fatal: Layer 1 still works.
    }

    // ... rest of Load ...
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_open_switch_shutdown) {
    // ... existing steps 1-4 ...

    // === W7 Track A: unbind MEDIA_BUG_START detector before MediaBugManager teardown ===
    osw::security::UnbindEavesdropDetector();

    // ... rest of Shutdown ...
}
```

The exact step numbers depend on the current Load body; the sub-agent
inserts the W7 block in a logical position (after the audit /
event-bus init, before the gRPC server start).

### 6. Config schema

`include/osw/config/module_config.h`:

```cpp
struct ModuleConfig {
    // ... existing ...
    std::string eavesdrop_policy = "deny";  // module default
    // ... existing ...
};

struct TenantAcl {
    // ... existing ...
    std::string eavesdrop_policy_override;  // empty = use module default
    bool allow_eavesdrop = false;           // default forbids eavesdrop
    // ... existing ...
};
```

`Validate()`:

- `eavesdrop_policy ∈ {"deny", "audit", "allow"}` (case-insensitive;
  unknown → coerce to `"deny"` + WARN).
- `tenant.eavesdrop_policy_override`: same allowed set; empty is
  legal (means "use module default").

`open_switch.conf.xml` schema docs (in `docs/CONFIG.md` or wherever the
operator-facing docs live in the repo — sub-agent adapts):

```xml
<settings>
  <param name="eavesdrop_policy" value="deny"/>
</settings>

<acl>
  <tenant id="qc-team">
    <param name="api_key" value="qc-..."/>
    <param name="allow_eavesdrop" value="true"/>
    <param name="eavesdrop_policy_override" value="audit"/>
  </tenant>
</acl>
```

The XML parser (whatever shim is in `src/config/`) reads these into
the new fields. Sub-agent extends the existing parser in the
existing file; do NOT introduce a new parser library.

### 7. Tier classifier — add the 4 subclasses to Tier-1 allowlist

`src/events/tier.cc`:

```cpp
constexpr const char* kTier1Subclasses[] = {
    // ... existing audit subclasses ...
    "osw.eavesdrop.denied",
    "osw.eavesdrop.audit",
    "osw.eavesdrop.allowed",
    "osw.eavesdrop.detected_post_attach",
    // ...
};
```

Or whatever the existing data structure for the allowlist is — the
sub-agent extends in style.

Test (`tier_test.cc`):

- `Classify("CUSTOM", "osw.eavesdrop.denied") == Tier::k1Durable`
- Same for the other three subclasses.

## Test plan summary

| Test | What it proves |
|---|---|
| `ParseEavesdropPolicy` table (eavesdrop_policy_test.cc) | Enum ↔ string round-trip; unknown → kDeny |
| `ResolveEffectivePolicy` table | Tenant override wins; allow_eavesdrop=false forces deny |
| `MarkBotSessionSetsVariables` (eavesdrop_app_test.cc) | StartTts/StartVoicebot post-attach hook sets 4 channel vars |
| `Layer1_Deny_HangsUpEavesdropper` | osw_eavesdrop app on deny channel: hangup + audit Tier 1 |
| `Layer1_Audit_DelegatesToFs` | osw_eavesdrop app on audit: delegates to switch_ivr_eavesdrop_session + audit Tier 1 |
| `Layer1_Allow_DelegatesToFs` | osw_eavesdrop app on allow: delegates + audit Tier 1 |
| `Layer1_MissingData_NoOp` | Empty data arg → WARN, no crash, no audit |
| `Layer1_TargetNotFound_NoOp` | Unknown target UUID → WARN, no audit, no hangup |
| `Layer2_IgnoresNonEavesdropBugs` (eavesdrop_detector_test.cc) | Other bug functions don't emit eavesdrop audits |
| `Layer2_IgnoresUnmarkedSessions` | Non-bot sessions don't emit eavesdrop audits |
| `Layer2_EmitsAuditForBotMarked_DenyPolicy` | bot-marked + deny → Tier 1 audit subclass `detected_post_attach`, `policy_applied=deny`, bug NOT removed |
| `Layer2_EmitsAuditForBotMarked_AuditPolicy` | Same flow with policy=audit |
| `Layer2_EmitsAuditWhenPolicyVarMissing` | Missing policy var → default deny |
| `Layer2_DoesNotRemoveBug` | switch_core_media_bug_remove_callback is NOT called |
| `TierClassifier_AllFourSubclassesTier1` | Classifier routes all 4 subclasses to Tier 1 |
| `Integration_NonBotEavesdrop_NoEnforcement` | Eavesdrop on a non-bot call: no policy, no audit |
| `Integration_TenantOverrideAllow` | Tenant with allow_eavesdrop=true + policy=audit overrides module default deny |
| `Integration_PolicyVarRemovedAfterAttach` | Variable removed mid-call: Layer-2 falls back to module-default in audit emit |
| `Integration_VadProtectedCallNotAffected` | VAD-only call (no TTS/voicebot) is NOT marked, eavesdrop allowed without policy enforcement |

The sub-agent translates each into a `TEST(...)` or
`TEST_F(IntegrationFixture, ...)` block. Total ~18 acceptance tests.

## Verification gate (per commit)

The Sonnet sub-agent runs these BEFORE every push:

```bash
cd /tmp/open-switch-w7a
make protos.lint
make protos
cmake -B build -DENABLE_ASAN=ON -DENABLE_TSAN=OFF -DBUILD_TESTS=ON   # ASAN local lesson from W6A
cmake --build build -j
ctest --test-dir build --output-on-failure
make edge.lint   # clang-format + clang-tidy
# Spec-doc lint:
docker run --rm -v "$PWD":/work davidanson/markdownlint-cli2:v0.18.0 'openspec/changes/core-module-v1/**/*.md'
```

All MUST be green before push. If any leak / race / lint fires, fix on
the worktree and re-run; do NOT push a "fix later" commit.

After first push, CI runs the full matrix (proto / amd64 ASAN / arm64
ASAN / TSAN race / clang-tidy / markdownlint). The sub-agent watches
the PR; on failure, fixes on the branch and pushes a NEW commit
(NEVER force-push without explicit user authorization).

## Commit message

```text
feat(security): W7 Track A — osw_eavesdrop app + MEDIA_BUG_START detector

Add the V1 eavesdrop policy on bot-participating calls per
designs/security-and-eavesdrop.md:

  - Layer 1: osw_eavesdrop dialplan app reads osw_eavesdrop_policy
    channel variable and hangs up on "deny" / delegates to
    switch_ivr_eavesdrop_session on "audit"/"allow". Emits Tier-1
    audit before any bug attaches.

  - Layer 2: MEDIA_BUG_START event handler emits Tier-1 audit when
    a bug with function=eavesdrop attaches to a session marked
    osw_bot_session=true. Detection only; does NOT remove the bug
    (FF-002 thread-id gate + FF-003 static symbol make removal
    unimplementable against vanilla FS v1.10.12).

  - Channel-var marker: StartTts and StartVoicebot now set
    osw_bot_session / osw_bot_purpose / osw_eavesdrop_policy /
    osw_tenant on the target channel post-attach.

  - Module config: eavesdrop_policy (module default) + per-tenant
    eavesdrop_policy_override + allow_eavesdrop ACL gate.

  - Audit: 4 new Tier-1 subclasses (osw.eavesdrop.denied / .audit /
    .allowed / .detected_post_attach).

  - FACT: new FF-035 documents SWITCH_ADD_APP + switch_application_
    interface_t lifecycle.

18 acceptance tests pass under ASAN+TSAN locally.
```

No `Co-Authored-By:` line (project memory: HARDENED). Author / committer
is `@luongdev`.

## Interaction with W7 Track D (`StartBot` multi-target)

When the W7 Track D `StartBot` RPC attaches its read+write bugs on a
target channel, the bugs carry `function_name = "mod_open_switch"`
(via `MediaBugManager::Attach` — see W6 Track A brief §"Bug callback
trampoline"). The function name is what FS publishes as the
`Media-Bug-Function` header in the `MEDIA_BUG_START` event (FF-011).

The Layer 2 detector in this Track filters strictly on
`Media-Bug-Function == "eavesdrop"`. Bot bugs from Track D do NOT
match this filter and are NEVER flagged as eavesdrop events. This is
the intended separation: the policy is about supervisor eavesdrop,
not about module-owned bot infrastructure.

Track D also marks the target channel with `osw_bot_session=true` and
`osw_eavesdrop_policy=<resolved>` immediately after attach (same
helper `MarkBotSession()` introduced in this Track). A supervisor
who later issues `eavesdrop` on the same channel via raw FS dialplan
WILL trigger Layer 2 — the marker is set, the supervisor's bug
carries `function_name="eavesdrop"`, and the detector emits the
Tier-1 `osw.eavesdrop.detected_post_attach` audit as designed.

This means the bot infrastructure and the eavesdrop policy stack
COEXIST: Track D handles bot lifecycle, Track A handles supervisor
listen-in. Neither interferes with the other.

## Out of scope (for Track A explicitly)

- Recording (Track B owns RECORDING_RELAY purpose + StartRecordingRelay).
- Patching FreeSWITCH to expose `eavesdrop_callback` symbol — not on V1.
- OAuth2 / OIDC for control plane — V1.5.
- Operator-side ACL examples (`<context name="bot-tenant-acme">` block) —
  documented in `security-and-eavesdrop.md` as Layer 3; nothing to
  ship in the module.

## Sub-agent prompt template (orchestrator-internal)

The orchestrator hands the brief above as the authoritative spec.
Spawn-time prompt (filled by orchestrator):

```text
You are the W7 Track A sub-agent. Your authoritative scope is
/tmp/open-switch-w7a/openspec/changes/core-module-v1/implementation/W7-track-A-eavesdrop-policy.md.

Implementation rules (HARDENED in project memory):

  - NO Co-Authored-By trailers in commits. Author/committer is @luongdev.
  - Run ASAN=ON locally before every push (lesson from W6 Track A).
  - Do NOT force-push without explicit user authorization. After first
    push to your branch, every history-rewriting push requires fresh
    user approval.
  - Do NOT commit build artifacts.
  - Run codex/gemini CLI is the orchestrator's job, not yours. Do NOT
    spawn review sub-agents.

When done, commit on branch implementation/wave7-track-a-eavesdrop with
the exact commit message in §"Commit message" of the brief. Push with
`git push -u origin implementation/wave7-track-a-eavesdrop`. Open PR
via `gh pr create` or notify the orchestrator. Do NOT merge.
```
