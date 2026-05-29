# Control API spec — gRPC service contracts

The control plane service is `open_switch.control.v1.ControlService`,
defined in `proto/open_switch/control/v1/control.proto`. This document
specifies the externally-visible contract: error model, idempotency,
deadlines, retry semantics, ACL gating, and method-by-method behavior.

## Service shape

```protobuf
service ControlService {
  rpc Originate(OriginateRequest) returns (OriginateResponse);
  rpc Hangup(HangupRequest) returns (HangupResponse);
  rpc HangupMany(HangupManyRequest) returns (HangupManyResponse);
  rpc Bridge(BridgeRequest) returns (BridgeResponse);
  rpc Execute(ExecuteRequest) returns (ExecuteResponse);
  rpc SetVariables(SetVariablesRequest) returns (SetVariablesResponse);
  rpc Hold(HoldRequest) returns (HoldResponse);
  rpc Unhold(UnholdRequest) returns (UnholdResponse);
  rpc BlindTransfer(BlindTransferRequest) returns (BlindTransferResponse);

  rpc StartStt(StartSttRequest) returns (StartSttResponse);       // media-bridge
  rpc StopStt(StopSttRequest) returns (StopSttResponse);
  rpc StartTts(StartTtsRequest) returns (StartTtsResponse);       // media-bridge
  rpc StopTts(StopTtsRequest) returns (StopTtsResponse);
  rpc StartVoicebot(StartVoicebotRequest) returns (StartVoicebotResponse);
  rpc StopVoicebot(StopVoicebotRequest) returns (StopVoicebotResponse);
  rpc StartRecordingRelay(StartRecordingRelayRequest)
      returns (StartRecordingRelayResponse);
  rpc StopRecordingRelay(StopRecordingRelayRequest)
      returns (StopRecordingRelayResponse);

  rpc SubscribeEvents(SubscribeEventsRequest)
      returns (stream open_switch.events.v1.EventEnvelope);

  rpc Health(HealthRequest) returns (HealthResponse);
}
```

The `StartStt`/`StartTts`/etc. methods are media-control RPCs detailed
under the media-bridge spec but listed here for completeness. The
remainder of this document focuses on the call-control half.

## RequestHeader contract

Every request carries a `RequestHeader`:

```protobuf
message RequestHeader {
  string request_id = 1;
  string tenant_id  = 2;
  string traceparent = 3;
  google.protobuf.Timestamp deadline = 4;
}
```

- `request_id` MUST be unique per logical request. UUIDv7 recommended
  for time-ordering. Used for idempotency dedup (see below).
- `tenant_id` MUST be present (non-empty) and match an ACL entry. The
  module rejects empty `tenant_id` with `INVALID_ARGUMENT` and unknown
  tenants with `PERMISSION_DENIED`. **Empty tenant_id is NEVER treated
  as a valid identity** (Phase 1 Codex finding I-6) — this prevents
  idempotency cache collisions and ACL bypass.
- `traceparent` is W3C trace context. Module propagates into emitted
  events for cross-system correlation.
- `deadline` is informational; the gRPC deadline (set on the call)
  is authoritative.

If `request_id` is empty: module generates one and includes it in
response logs, but the request is NOT idempotent (every retry creates
a new call). Callers SHOULD always set it.

## Idempotency

The module maintains a per-process LRU cache keyed by
`(tenant_id, request_id)` with TTL `idempotency_ttl_seconds`
(default 300). Each entry holds one of three values:

- **In-flight marker** + `std::condition_variable` for waiters
- **Cached response** (the protobuf, serialized)
- (After TTL expiry: entry evicted)

Behaviour table (Phase 1 Codex finding C-5 — in-flight semantics
explicitly specified; round-3 finding N3 — error-path cleanup
made explicit):

| Scenario | Module action |
|---|---|
| First call with `(tenant, request_id)` | Insert in-flight marker. Execute. On completion of the handler — whether success OR non-success — replace the marker with the cached response (see "Error-path cleanup" below). Return the response. |
| Repeat call within TTL with same `(tenant, request_id)` AND same method, **original still in-flight** | Block on the in-flight marker's condvar with a "shadow deadline" = `min(gRPC deadline, idempotency_in_flight_max_wait)`. When original completes: return the same response. If shadow deadline expires first: return `ALREADY_EXISTS` with message "Originate in flight; await original or use a fresh request_id". |
| Repeat call within TTL with same `(tenant, request_id)` AND same method, **original completed (success or error)** | Return cached response WITHOUT re-execution. |
| Repeat call within TTL with same `(tenant, request_id)` but DIFFERENT method | Reject with `ALREADY_EXISTS`. |
| Repeat call AFTER TTL expired | Treated as new request. |

Why the in-flight block (vs the round-1 draft which left this
unspecified — Codex C-5 scenario): a client whose gRPC deadline
fires while Originate is mid-dial would retry and HIT the in-flight
marker, not miss the cache. Without the block: re-execution →
double-dial (customer dials twice). The block + shadow timeout
prevents double-dial while keeping clients responsive.

### Error-path cleanup (Codex round-3 finding N3)

On any non-success return from the original handler — `INTERNAL`,
`FAILED_PRECONDITION`, `RESOURCE_EXHAUSTED`, `UNAVAILABLE`, etc.,
including handler exceptions caught by the gRPC interceptor — the
module MUST:

1. Replace the in-flight marker with the cached **error** response
   (the protobuf-serialized response containing the
   `ErrorDetail.type` and message that will be sent to the
   original caller). Errors are cacheable for idempotency
   purposes.
2. `notify_all()` the marker's condition variable so any waiters
   on the same `(tenant, request_id)` unblock immediately.
3. Subsequent retries within TTL receive the same error response
   from cache, byte-for-byte identical to what the original caller
   saw.

Rationale: the cache contract is "same request, same response".
That contract holds for non-success responses too. A client whose
original Originate returned `FAILED_PRECONDITION(USER_BUSY)` and
who retries 200 ms later with the same `request_id` should see
the same `USER_BUSY` rather than a fresh dial. Operators wanting
"retry the call on error" use a new `request_id`.

**Configurable opt-out**: operators with a strong "retries should
work after errors" stance can set
`idempotency_cache_errors=false` in `<settings>`. With this off,
non-success handler returns evict the marker entirely (no cached
response stored) and notify waiters with the in-flight cancel
sentinel — waiters return `ALREADY_EXISTS` as if shadow-timeout
had fired. New retries with the same `request_id` proceed as
first calls. Default is `true` (cache errors); switching to
`false` reintroduces the double-dial-on-fast-retry risk against
errors, which is why it is not the default.

**Crash path (unhandled exception, SIGSEGV, OOM-killer)**: if the
process aborts mid-execution, the in-memory cache is gone. The
"refuse module reload while any in-flight Originate exists" guard
(`Module-restart false-negative gap` below) catches graceful
reloads. For SIGKILL / OOM-killer there is no module-side
recovery; the residual is documented in the gap section.

Idempotency applies to ALL write methods (Originate, Hangup, Bridge,
Execute, SetVariables, Hold/Unhold, BlindTransfer, Start*/Stop*).
Read-only methods (Health, SubscribeEvents) are not cached.

`request_id` is per-tenant: same UUID under different tenants are
independent.

### Module-restart false-negative gap (residual risk)

A module reload wipes the cache. If FS channels survive the reload
(depends on FS reload semantics — `fs_cli reload mod_open_switch`
unloads + reloads the .so without affecting active channels), a
client retrying an Originate that was in-flight at reload time will
miss the cache and **may** double-dial.

Mitigations:

1. **Refuse module reload while any in-flight Originate exists** —
   the reload command returns `RESOURCE_EXHAUSTED` until the
   in-flight set drains. Operators can SIGTERM the module if
   forcing reload is required, accepting the gap.
2. **Optional cache persistence** (deferred V1.5): persist the cache
   to a small local SQLite file on graceful drain; reload on module
   load. Not in V1 scope.
3. **Client retry policy**: client SHOULD use a retry window
   slightly shorter than `idempotency_ttl_seconds` AND retry only
   after the module reports `SERVING` again (via Health), not blindly.

The gap exists but is bounded by client retry policy and the
"refuse reload while in-flight" guard.

### Configuration

```xml
<settings>
  <param name="idempotency_ttl_seconds"           value="300"/>
  <param name="idempotency_in_flight_max_wait_ms" value="60000"/>
  <!-- N3: cache non-success handler returns (INTERNAL, FAILED_PRECONDITION,
       etc.) the same way successful responses are cached. Retries within
       TTL receive the same error response. Set to "false" to evict the
       in-flight marker on error and let retries re-execute — bringing
       back the double-dial-on-fast-retry risk for failing handlers. -->
  <param name="idempotency_cache_errors"          value="true"/>
</settings>
```

## Error model

Errors use gRPC status codes plus a `ErrorDetail` protobuf in the
response (tag 99 by convention). The `ErrorDetail.type` mirrors gRPC
codes; `ErrorDetail.fs_cause` carries the FreeSWITCH cause string for
telephony errors.

| gRPC code | When used |
|---|---|
| `OK` | Success. Always with `ErrorDetail.type=TYPE_UNSPECIFIED` (= unset). |
| `INVALID_ARGUMENT` | Missing required field, malformed value, unknown enum, unsupported sample rate, etc. |
| `NOT_FOUND` | Target channel UUID does not exist, dialplan context not found, etc. |
| `ALREADY_EXISTS` | request_id reused for different method (see Idempotency). |
| `PERMISSION_DENIED` | Tenant ACL rejected the operation. |
| `UNAUTHENTICATED` | API key invalid or mTLS verification failed. |
| `RESOURCE_EXHAUSTED` | Rate limit hit; concurrent call cap; port pool exhausted. |
| `FAILED_PRECONDITION` | Channel not in the right state (e.g., Hangup of already-hangup'd channel). |
| `DEADLINE_EXCEEDED` | gRPC deadline before completion. |
| `UNAVAILABLE` | Module is DRAINING; transient; client should retry on another instance. |
| `INTERNAL` | Bug or unexpected FS error. |

Clients SHOULD retry on `UNAVAILABLE` with backoff and on
`DEADLINE_EXCEEDED` only if they reset `request_id`. Clients MUST
NOT retry on `INVALID_ARGUMENT` / `PERMISSION_DENIED` /
`UNAUTHENTICATED`.

## Per-method spec

### Originate

Originate a call. May immediately answer-and-do-something via the
`after_answer` oneof. Blocks the gRPC thread for up to the configured
timeout (`OriginateRequest.timeout`, default 60s).

```protobuf
message OriginateRequest {
  RequestHeader header = 1;
  repeated string endpoints = 2;
  Strategy strategy = 3;
  oneof after_answer {
    DialplanTarget dialplan = 10;
    AppSequence apps = 11;
    string bridge_to_uuid = 12;
    bool park = 13;
  }
  string caller_id_name = 20;
  string caller_id_number = 21;
  map<string, string> variables = 22;
  google.protobuf.Duration timeout = 23;
  string check_id = 24;
}
```

**ACL check**: tenant's `allowed_contexts` must include the target
context (from `dialplan.context` or implicit context if using `apps`).

**FS API**: `switch_ivr_originate` with a constructed dialstring.

**Behavior**:

1. If `after_answer.park=true`: dial, on answer park, return channel
   uuid.
2. If `after_answer.dialplan`: dial, on answer transfer to extension.
3. If `after_answer.apps`: dial, on answer execute apps in sequence.
4. If `after_answer.bridge_to_uuid`: dial, on answer bridge to the
   given uuid (typically used for queue-callback flows).

**`check_id`**: if set, the module locates this channel BEFORE the
B-leg answers; if the channel is gone, the originate is cancelled.
Useful for "abandon if customer hangs up while we're dialing the
agent" patterns.

**Response**:

```protobuf
message OriginateResponse {
  string channel_uuid = 1;
  ErrorDetail error = 99;
}
```

`channel_uuid` is FS's Channel-Unique-ID for the new leg. The caller
can use it for subsequent control operations.

**Failure cases**:

- No allowed context → `PERMISSION_DENIED`.
- Endpoint dial string malformed → `INVALID_ARGUMENT`.
- Originate returned non-success → `FAILED_PRECONDITION` with
  `ErrorDetail.fs_cause` = FS cause string (e.g., `"USER_BUSY"`,
  `"NO_USER_RESPONSE"`).
- check_id channel gone → `FAILED_PRECONDITION` with
  `fs_cause="ORIGINATOR_CANCEL"`.

**Events emitted** (by FS, routed by module):

- Tier 2: `CHANNEL_OUTGOING`, `CHANNEL_PROGRESS` (if early media).
- Tier 1: `CHANNEL_ANSWER` (on answer), `CHANNEL_BRIDGE` (if bridging).
- Tier 2: `CHANNEL_HANGUP`.
- Tier 1: `CHANNEL_HANGUP_COMPLETE`, `CHANNEL_DESTROY`.

### Hangup

Hang up a specific channel.

```protobuf
message HangupRequest {
  RequestHeader header = 1;
  string uuid = 2;
  string cause = 3;
  map<string, string> variables = 4;
}
```

**ACL**: tenant must own the channel (channel variable `osw_tenant`
matches). Cross-tenant hangup is `PERMISSION_DENIED`.

**FS API**: `switch_channel_hangup` with cause from
`switch_channel_str2cause`.

**Variables**: applied via `switch_channel_set_variable` before
hangup so they show up in the post-hangup CDR / `on_reporting`
handler.

**Behavior**: idempotent against already-hungup channels: returns OK
without error (FS internally tolerates). The audit event still fires.

**Response**: `HangupResponse { ErrorDetail error }`.

**Audit event**: Tier 1 `osw::audit::hangup_admin` with the cause and
tenant.

### HangupMany

Hangup many channels at once. Useful for tenant drains.

```protobuf
message HangupManyRequest {
  RequestHeader header = 1;
  repeated string uuids = 2;
  string cause = 3;
}
```

**ACL**: each UUID checked individually. Mixed-tenant input returns
the channels actually hung up plus an error.

**Response**:

```protobuf
message HangupManyResponse {
  repeated string hungup_uuids = 1;
  ErrorDetail error = 99;
}
```

**Rate consideration**: HangupMany of 1000 UUIDs may not be feasible
within a single gRPC deadline. Module processes UUIDs in **input
order** (Phase 1 Codex finding I-8) and may return `DEADLINE_EXCEEDED`
with a partial `hungup_uuids` list. The caller can resume with the
remainder by computing `set(input.uuids) - set(response.hungup_uuids)`
and retrying. Input-order processing makes the resume slice
deterministic.

### Bridge

Bridge two existing legs.

```protobuf
message BridgeRequest {
  RequestHeader header = 1;
  string leg_a_uuid = 2;
  string leg_b_uuid = 3;
  map<string, string> variables = 4;
}
```

**ACL**: both legs must be in the same tenant. Cross-tenant bridge =
`PERMISSION_DENIED`.

**FS API**: `switch_ivr_uuid_bridge`.

**Pre-bridge**: if either leg is in `CF_BROADCAST` (playing a file),
stop the broadcast first.

**Response**: `bridged_uuid` is `leg_b_uuid` on success.

### Execute

Run a FreeSWITCH dialplan application on a channel.

```protobuf
message ExecuteRequest {
  RequestHeader header = 1;
  string uuid = 2;
  string app = 3;
  string args = 4;
  bool async = 5;
}
```

**ACL**: tenant must own channel; the app name must be in the
tenant's allowed-app list (or `*` for unrestricted).

**Default tenant allowed-app list** (post-Phase-1-fix-sprint per
Codex finding C-7):

```
answer, set, playback, record_session, hangup, sleep, osw_eavesdrop
```

**Apps NOT on the default allowlist** (operator must opt in per tenant,
with documented care):

| App | Risk |
|---|---|
| `system` / `bgapi` / `lua` / `python` | Full RCE on the FS host |
| `transfer` / `osw_transfer` | Args include destination context — bypasses Originate ACL unless the module's per-app validator (below) checks the third arg. Default: NOT allowed; if operator opts in, the module enforces context allowlist on the args. |
| `bridge` | Args include endpoint URI which can target any destination, plus `bridge_pre_execute_*` variables. Use Bridge RPC instead, which has explicit ACL. |
| `eavesdrop` (raw) | Bypasses bot-call eavesdrop policy. Use `osw_eavesdrop` instead. |
| `originate` | Bypasses Originate RPC's tenant ACL on contexts. Use Originate RPC instead. |
| `play_and_get_digits` | Reads variable_* state; exfiltration vector if tenant variables hold secrets. |

The module enforces this at handler entry by checking the requested
app name against `tenant.allowed_apps` (per-tenant config). The
defaults err on the side of safety; operators expand consciously.

**Per-app validation** for context-sensitive apps:

If `transfer` is opt-in for a tenant, the module's Execute handler
parses `args` as `<destination> [<dialplan>] [<context>]` and
verifies the third arg (if present) against `tenant.allowed_contexts`.
Mismatch returns `PERMISSION_DENIED`. The same validator covers
`osw_transfer` and any future context-routing app.

For `bridge` (also opt-in), the destination URI's profile + context
hints are validated against `tenant.allowed_bridges` (a separate
allowlist).

**FreeSWITCH `${variable}` expansion (Codex round-3 finding N4):**

FreeSWITCH dialplan applications interpret `${variable}` references
at app-run time, AFTER our gRPC handler has parsed the literal
`args` string. An attacker (or a careless caller) submitting

```
Execute(uuid=X, app="transfer", args="666 XML ${attacker_var}")
```

would have our parser see the literal `${attacker_var}` as the
third arg. We cannot evaluate the variable on our side (it lives on
the FS channel and may differ from the value we'd resolve here),
and a literal string-compare against the allowlist would either
under-reject (`${attacker_var}` doesn't match the allowlist literal,
but FS will expand it to a privileged context at run time) or
over-reject (a legitimate `${valid_var}` is also rejected).

The module REJECTS any Execute args for context-sensitive apps
(`transfer`, `osw_transfer`, `bridge` — i.e., the apps in the
per-app validator) that contain a `${...}` substring, with:

- `ErrorDetail.type = INVALID_ARGUMENT`
- Message: `"FS variable expansion (\${...}) is not permitted in
  Execute args for app '<app>'; expand at the orchestrator before
  submitting"`

Detection is a substring search for the literal `${`. This is
deliberately conservative — it catches all legitimate FS variable
syntax, including nested references like `${${meta}_extension}`,
and rejects them as a class. Operators wanting variable values in
the transfer destination must resolve them at the orchestrator
before issuing the RPC.

**The check applies even when the variable would expand to a value
that IS in the allowlist.** The module cannot prove that the
runtime expansion matches; the safe stance is to refuse the entire
class of inputs.

**FS API**: `switch_api_execute` for synchronous; `switch_ivr_broadcast`
or `switch_core_session_execute_application_async` for async.

**Why we restrict apps**: arbitrary `Execute` is full RCE on FS via
the `system` or `bgapi` apps. The expanded allowlist + per-app
validation closes the C-7 transfer-context-bypass attack noted in
Phase 1 Codex review.

**Response**: `ExecuteResponse { string result, ErrorDetail error }`.
For async, `result` is empty and OK.

### SetVariables

Set channel variables.

```protobuf
message SetVariablesRequest {
  RequestHeader header = 1;
  string uuid = 2;
  map<string, string> variables = 3;
}
```

**ACL**: tenant owns channel.

**Reserved variable model**: SetVariables uses a **denylist of
security-relevant prefixes** (Phase 1 Codex finding I-7 — the prior
draft's allowlist was incomplete). Operators wanting stricter behaviour
can configure a per-tenant explicit allowlist that overrides the
default denylist semantics.

Default denylist — module rejects sets to ANY variable whose name
matches one of:

```
osw_*                  # module-internal; immutable
eavesdrop_*            # FS eavesdrop app behaviour
record_*               # FS recording config (rate, format)
recording_*            # mod_recording / variants
playback_*             # playback termination/silence params
bridge_*               # bridge app behaviour (pre/post execute hooks)
bridge_pre_execute_*   # arbitrary app execute before bridge
bridge_post_execute_*  # arbitrary app execute after bridge
originate_*            # originate timeout, caller-id overrides, etc.
endpoint_*             # endpoint module config
hold_music             # MOH file path (file disclosure)
sip_h_*                # outbound SIP header injection
sip_invite_*           # SIP invite headers
sip_to_*               # SIP To header
sip_from_*             # SIP From header
api_*                  # api command hooks
exec_*                 # execute-on-* hooks
session_*              # session-level config
domain_name            # domain spoofing
context                # dialplan context override (bypasses ACL)
dialplan               # dialplan type override
```

Attempting to set any denylisted variable returns `INVALID_ARGUMENT`
with message identifying which variable was rejected.

The denylist is deliberately broad. Operators wanting to allow a
specific denylisted variable for a specific tenant configure an
allowlist override:

```xml
<tenant id="acme">
  <param name="setvar_allow_override" value="sip_h_X-Custom-Header,playback_silence_ms"/>
</tenant>
```

Variables in the override pass through. The reverse (denying a
non-denylisted variable) is not supported; if a variable name is
ever discovered to be security-relevant, it's added to the global
denylist via module update.

### Configuration

```xml
<settings>
  <param name="setvar_denylist_extra" value=""/>  <!-- comma-separated; extends default -->
</settings>
```

### Hold / Unhold

Hold (or unhold) one or more channels.

```protobuf
message HoldRequest {
  RequestHeader header = 1;
  repeated string uuids = 2;
}
```

**FS API**: `switch_core_media_toggle_hold(session, 1)` for hold;
`switch_core_media_toggle_hold(session, 0)` for unhold.

**Response**: `held_uuids` lists those actually transitioned (already-
held channels are silently skipped).

### BlindTransfer

Blind transfer a channel.

```protobuf
message BlindTransferRequest {
  RequestHeader header = 1;
  string uuid = 2;
  string destination = 3;
  string dialplan = 4;
  string context = 5;
  map<string, string> variables = 6;
}
```

**ACL**: tenant owns channel; tenant allowed in target context.

**FS API**: `switch_ivr_session_transfer`.

### Media-control RPCs

`StartStt`, `StopStt`, `StartTts`, `StopTts`, `StartVoicebot`,
`StopVoicebot`, `StartRecordingRelay`, `StopRecordingRelay`.

These are implemented per the
[`media-bridge.md`](../../designs/media-bridge.md) spec. Common shape:

```protobuf
message Start<Purpose>Request {
  RequestHeader header = 1;
  string channel_uuid = 2;
  string upstream_endpoint = 3;
  // purpose-specific fields ...
}

message Start<Purpose>Response {
  string stream_id = 1;     // module-internal handle; needed for Stop
  ErrorDetail error = 99;
}
```

`stream_id` is opaque to the caller and used for the Stop RPC. The
module also tracks streams by `(channel_uuid, purpose)` so callers
can Stop without the stream_id if they have those.

#### Recording relay

`StartRecordingRelay` attaches a module-owned read tap plus write tap
and sends recording audio to `relay_endpoint` as `RECORDING_RELAY`.
The call is refused with `FAILED_PRECONDITION` unless a module-owned
INJECT bug (`StartTts` or `StartVoicebot` write side) is already
active on the channel. This keeps module-managed recordings behind bot
injection in the FS media-bug chain.

```protobuf
message StartRecordingRelayRequest {
  RequestHeader header = 1;
  string channel_uuid = 2;
  string relay_endpoint = 3;
  bool stereo = 4;          // false = mono mixed, true = L/R interleaved
  uint32 sample_rate_hz = 5; // 0 = recording_default_rate_hz
}

message StopRecordingRelayRequest {
  RequestHeader header = 1;
  string channel_uuid = 2;
  string stream_id = 3; // empty = stop all recording relays on channel
}
```

Config knobs:

- `recording_send_ring_ms` default `500`.
- `stereo_desync_warn_ms` default `5`.
- `stereo_desync_timeout_ms` default `25`.
- `recording_default_rate_hz` default `8000`.
- `warn_record_before_inject` default `true`; when `StartTts` or
  `StartVoicebot` finds an FS-native `record_session` bug already
  attached, the module emits `osw.recording.warn_record_before_inject`.

### SubscribeEvents

Server-streaming RPC. Module sends `EventEnvelope` messages to the
client until the client cancels.

```protobuf
message SubscribeEventsRequest {
  RequestHeader header = 1;
  repeated string tiers = 2;
  repeated string event_names = 3;
}
```

**Behavior**:

- Module maintains an in-memory ring buffer (capacity
  `event_ring_capacity`, default 4096).
- Per-subscriber offset advances on each successful write.
- If the subscriber's offset falls behind the ring tail (drop
  detected), the stream is closed with `RESOURCE_EXHAUSTED`.
- Filtering by `tiers` (e.g., `["TIER_1_CRITICAL"]`) and `event_names`
  is applied before send. The `event_names` matcher is **prefix-
  wildcard only** in V1 (Codex W2.5 C-2): a trailing `*` matches any
  suffix (e.g. `CHANNEL_*` matches `CHANNEL_HANGUP_COMPLETE`); any
  other input is an exact-string match. Generic glob syntax
  (`*` anywhere, `?`, `[abc]` character classes) is NOT supported in
  V1 — operators that need richer matching should subscribe to all
  events for the tier and filter client-side. The same filter is
  applied to both the replay window and the live tail (Codex W2.5
  C-1).

**Durability beyond the replay window is the subscriber's
responsibility.** For Tier-1 (billing-grade) no-loss requirements,
operators MUST run ≥ 2 subscribers per node (HA pair) and persist
events to a durable store on the subscriber side (Kafka topic,
Redis Streams, write-ahead log, S3 — operator's choice). See
[`event-tiers.md`](../../designs/event-tiers.md) "No-loss reference
architecture". The module's per-tier in-memory ring + `since_seq`
replay (see the `SubscribeEventsRequest.since_seq` proto field) is
the only module-side replay mechanism; it covers roughly the
ring-size-by-emission-rate window (~25 min for Tier-1 at default
sizing).

### Health

```protobuf
message HealthResponse {
  enum Status {
    STATUS_UNSPECIFIED = 0;
    SERVING = 1;
    NOT_SERVING = 2;
    DRAINING = 3;
  }
  Status status = 1;
  string module_version = 2;
  string freeswitch_version = 3;
  uint64 active_channels = 4;
  uint64 active_media_bugs = 5;
  uint64 events_emitted_total = 6;
  // Active SubscribeEvents stream count.
  uint32 subscriber_count = 7;
  // Per-tier ring fill percentage (0-100).
  uint32 tier1_ring_fill_pct = 8;
  uint32 tier2_ring_fill_pct = 9;
  uint32 tier3_ring_fill_pct = 10;
  // Total events evicted (dropped) per tier since module load.
  uint64 tier1_dropped_total = 11;
  uint64 tier2_dropped_total = 12;
  uint64 tier3_dropped_total = 13;
}
```

The wire shape mirrors `proto/open_switch/control/v1/control.proto`'s
`HealthResponse` exactly (Codex round-3 finding N12 — earlier drafts
of this spec showed only the first six fields; subsequent proto
revisions added the subscriber + per-tier metrics and the spec was
out of sync).

Liveness + readiness check. Implements the standard gRPC health
check semantics so Kubernetes / Consul / load balancers can probe.

- `SERVING`: module is up and ready.
- `NOT_SERVING`: module loaded but rejecting RPCs (e.g., catastrophic
  config validation failure on hot-reload, no subscribers AND a
  Tier-1 ring overflow rate over an operator-configured threshold,
  or admin-forced via `osw_force_not_serving` channel variable).
  No "Redis down" path exists — there is no in-module Redis.
- `DRAINING`: SIGTERM received; rejecting new Originate /
  SubscribeEvents; existing calls allowed to finish.

Operators wanting metric-style telemetry (Prometheus, etc.) run a
sidecar that polls `Health` and translates. V1 does not emit
Prometheus metrics natively (project decision — see
[`architecture.md`](../../designs/architecture.md#health-counters)).

## Rate limiting

Per-tenant token bucket. Default refill rate = 100 RPS;
burst size = refill × 2. On overflow: `RESOURCE_EXHAUSTED`.

Configured per-tenant in the ACL block. Read-only methods (Health,
SubscribeEvents start) are NOT rate-limited (they don't trigger FS
operations).

## Concurrency limits

Per-tenant active-call cap.

**Default** (Phase 1 Codex finding N-6): the module derives a safe
default from `FS_MAX_SESSIONS` divided by the number of configured
tenants, rounded up. This prevents the foot-gun where a first-time
operator forgets to set the cap and a single tenant can consume the
host's entire session pool.

For example: `FS_MAX_SESSIONS=2000` and three tenants → default cap
~670 per tenant. Operator may override per tenant via
`max_active_channels` in the ACL block.

Originate that would exceed the cap returns `RESOURCE_EXHAUSTED`.

## Streaming RPCs and cancellation

`SubscribeEvents` is the only server-streaming RPC in V1. Cancellation
semantics:

- Client cancels: module cleans up subscriber state, no error event.
- Server closes (drain, slow consumer): stream terminates with
  `RESOURCE_EXHAUSTED` (slow consumer) or `UNAVAILABLE` (drain).
- Network drop: gRPC keepalive detects within 10s, cleanup fires.

The media RPCs (`Start*Stream`, etc.) are unary; the streaming
happens out-of-band on a separate gRPC channel **module → upstream
service**, not exposed via the control plane.

## Tracing

Module emits OpenTelemetry traces (via gRPC's built-in interceptor
when configured). Trace context comes from `RequestHeader.traceparent`
and propagates into:

- The gRPC outbound to upstream media services (via metadata).
- Channel variable `traceparent` on the FS channel, available to
  downstream apps and emitted in events.

Trace spans named `osw.ctrl.<method>` for control RPCs;
`osw.media.<purpose>.<start|frame|stop>` for media-bridge operations.

V1 emits spans but doesn't include OTel collector — operator opts in
via gRPC interceptor config in `open_switch.conf.xml`. Default V1:
spans disabled (project owner: "không cắm telemetry V1").

## Versioning

- Service path is `/open_switch.control.v1.ControlService/<Method>`.
- The `v1` segment is the **major** version. Breaking changes go to
  `v2`. Both versions can be implemented simultaneously by the
  module during a deprecation window (one release cycle).
- Within `v1`: only backward-compatible changes. New fields take new
  tags. Renamed fields are NOT allowed.
- Tags are reserved on removal:

```protobuf
message OriginateRequest {
  reserved 99;            // removed `legacy_dial_prefix` (V1.2)
  reserved "legacy_dial_prefix";
}
```

## Compatibility test fixtures

`tests/integration/control_api_compat_test.cc` exercises every RPC
against a known-good FS dialplan + a mock upstream gRPC service.
Failures here block release.

`tests/integration/control_api_idempotency_test.cc` exercises the
idempotency cache: same request_id twice returns same response;
across-tenant request_id is independent; TTL-expired entry
re-executes.

`tests/integration/control_api_acl_test.cc` exercises the ACL gates:
disallowed context rejected, cross-tenant hangup rejected, reserved
variable set rejected.

`tests/integration/control_api_execute_var_expansion_test.cc`
exercises the N4 reject path: `Execute(app=transfer, args="666 XML ${attacker_var}")`
returns `INVALID_ARGUMENT` with the documented message. Variations
with single-quoted args and nested `${${meta}}` references also
reject. A control case with literal context name in the args is
checked against the allowlist normally.

`tests/integration/control_api_idempotency_errors_test.cc` exercises
the N3 error-cache path: a handler that returns `FAILED_PRECONDITION`
on the first call returns the same response (byte-for-byte) on a
retry within TTL. With `idempotency_cache_errors=false`, the retry
re-executes.

## Open questions (resolve during implementation)

1. Should `Originate` block the gRPC thread for the full timeout, OR
   should we add an async variant returning `job_id` and emitting a
   completion event? V1 sync is simpler; async variant deferred.
2. Should we add a `WhoAmI` RPC returning the authenticated identity
   for client-side debugging? Useful but not blocker.
3. Should we support gRPC reflection? Helps debugging. Default off
   (production); operator opts in via config.

Note: `SubscribeEvents` resumption via `since_seq` is already
specified (and present in the proto at
`proto/open_switch/control/v1/control.proto`
`SubscribeEventsRequest.since_seq`). An earlier draft of this
document had a "should we add since=<offset>" open question that
contradicted the post-F0 proto; the question is resolved by
implementation — see SubscribeEvents above and
[`event-tiers.md`](../../designs/event-tiers.md) "`since_seq`
replay" for the contract.
