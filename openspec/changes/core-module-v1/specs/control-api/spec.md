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
- `tenant_id` MUST be present and match an ACL entry. The module
  rejects unknown tenants with `PERMISSION_DENIED`.
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
(default 300). Cached entries store the response protobuf.

Behavior:

| Scenario | Module action |
|---|---|
| First call with `(tenant, request_id)` | Execute, cache response, return |
| Repeat call within TTL with same `(tenant, request_id)` AND same method | Return cached response WITHOUT re-execution |
| Repeat call within TTL with same `(tenant, request_id)` but DIFFERENT method | Reject with `ALREADY_EXISTS` |
| Repeat call AFTER TTL expired | Treated as new request |

Idempotency applies to ALL write methods (Originate, Hangup, Bridge,
Execute, SetVariables, Hold/Unhold, BlindTransfer, Start*/Stop*).
Read-only methods (Health, SubscribeEvents) are not cached.

`request_id` is per-tenant: same UUID under different tenants are
independent.

The module restarts wipe the cache. Operators are advised to use a
slightly shorter retry window than `idempotency_ttl_seconds` on the
client side to avoid the false-negative window after a module restart.

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
within a single gRPC deadline. Module processes in batches and may
return `DEADLINE_EXCEEDED` with a partial `hungup_uuids` list. The
caller can resume with the remainder.

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
tenant's allowed-app list (or `*` for unrestricted). Default tenant
allowed apps: `answer`, `set`, `playback`, `record_session`,
`bridge`, `transfer`, `hangup`, `sleep`.

**FS API**: `switch_api_execute` for synchronous; `switch_ivr_broadcast`
or `switch_core_session_execute_application_async` for async.

**Why we restrict apps**: arbitrary `Execute` is full RCE on FS via
the `system` or `bgapi` apps. Default allowlist excludes those.

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

**Reserved variable prefixes** that the module rejects to prevent
ACL bypass:

- `osw_*` — module-internal; immutable from gRPC. The module sets
  these.
- `eavesdrop_*` — managed by FS eavesdrop machinery; immutable.
- `record_*` — managed by FS recording machinery; the module
  rejects sets to these to prevent disabling compliance recording.

Attempting to set a reserved var returns `INVALID_ARGUMENT`.

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
  (glob patterns) is applied before send.

**Not for production durable consumption**. Use Redis Streams for that.

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
}
```

Liveness + readiness check. Implements the standard gRPC health
check semantics so Kubernetes / Consul / load balancers can probe.

- `SERVING`: module is up and ready.
- `NOT_SERVING`: module loaded but rejecting RPCs (e.g., Redis down
  beyond grace period).
- `DRAINING`: SIGTERM received; rejecting new Originate /
  SubscribeEvents; existing calls allowed to finish.

## Rate limiting

Per-tenant token bucket. Default refill rate = 100 RPS;
burst size = refill × 2. On overflow: `RESOURCE_EXHAUSTED`.

Configured per-tenant in the ACL block. Read-only methods (Health,
SubscribeEvents start) are NOT rate-limited (they don't trigger FS
operations).

## Concurrency limits

Per-tenant active-call cap. Default = unbounded; operator may set
e.g., `max_active_channels=500` per tenant. Originate that would
exceed the cap returns `RESOURCE_EXHAUSTED`.

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

## Open questions (resolve during implementation)

1. Should `Originate` block the gRPC thread for the full timeout, OR
   should we add an async variant returning `job_id` and emitting a
   completion event? V1 sync is simpler; async variant deferred.
2. Should `SubscribeEvents` support `since=<offset>` resumption?
   For V1 no — fresh start each subscribe. Operators wanting resume
   use Redis Streams directly.
3. Should we add a `WhoAmI` RPC returning the authenticated identity
   for client-side debugging? Useful but not blocker.
4. Should we support gRPC reflection? Helps debugging. Default off
   (production); operator opts in via config.
