# Event tiers — durability taxonomy and routing

## Why tiers exist

FreeSWITCH emits a wide variety of events. Treating all of them with
the same buffering and ordering guarantees is either wasteful (high
volume) or unsafe (consumer drops a billing event because a heartbeat
filled its queue).

We classify events into three tiers based on the question:
**"If we lost this event, what breaks?"**

| Tier | If lost, what breaks? | Acceptable loss rate |
|---|---|---|
| 1 (Critical) | Billing wrong, CDR incomplete, compliance gap | **Zero — operator must run HA subscriber pair** |
| 2 (State) | UI / dashboards stale; recoverable from later state | < 0.1% (per-hour window) |
| 3 (Ephemeral) | Nothing meaningful breaks | Best-effort |

Each tier has its own in-memory ring buffer (the replay window) and
its own backpressure-on-overflow policy. The module classifies events
and tags them with the tier; subscribers decide what to do with each
tier based on their durability needs. See
[`transport-adr.md`](transport-adr.md) for why gRPC streaming is the
sole transport (the module no longer ships to Redis directly).

## Tier 1 — Critical (billing / CDR / compliance)

**Loss tolerance from the module side**: tier is best-effort within
the module's in-memory ring; the module cannot promise no-loss without
durable storage. Operators MUST run ≥ 2 subscribers in HA, each
persisting to durable store (Kafka topic, Redis Stream, etc.), if no-loss
is a business requirement.

**Routing default** (configurable; events routed to Tier 1 ring):

| FS event | Why Tier 1 |
|---|---|
| `CHANNEL_ANSWER` | Billing start trigger |
| `CHANNEL_BRIDGE` | Talk-time start, agent stats |
| `CHANNEL_HANGUP_COMPLETE` | CDR finalisation, billing stop |
| `CHANNEL_DESTROY` | Resource cleanup confirmation; if lost, leak detection breaks |
| `RECORD_START` | Compliance proof of recording start |
| `RECORD_STOP` | Compliance proof of recording end |
| `DETECTED_TONE` (if AMD-billed) | AMD decision = bill or not |
| `CUSTOM` with subclass matching `*::critical` | Domain-specific business events |
| `CUSTOM` with subclass matching `osw::audit::*` | Module's own audit events |

**Backpressure policy**:

- **Ring overflow** (16384 events default, configurable): evict oldest.
  Increment `tier1_ring_overflow_total` counter. Emit a notification
  custom event tagged with Tier 2 subclass `osw::tier1_ring_overflow`
  (Tier 2 to avoid recursion into the Tier 1 ring).
- **Per-subscriber send-queue overflow** (4096 envelopes default per
  subscriber): close that subscriber's stream with
  `RESOURCE_EXHAUSTED`. Subscriber reconnects (with `since_seq` if
  tracked) and resumes from there.
- **No subscribers connected** when Tier 1 events arrive: events
  accumulate in the ring up to its capacity, then evict oldest. Module
  logs WARN every 30s with the drop count.

**Ring size sizing guidance**: 16384 entries × ~10 ev/s = ~25 minutes
of replay coverage. Operators with high Tier 1 volume (e.g., > 100
calls/min) may want to increase.

**Recommended HA subscriber pattern**:

```text
                          ┌──▶ subscriber A (primary)  ──▶ durable store
mod_open_switch ──events──┤
                          └──▶ subscriber B (standby)  ──▶ durable store
```

Each subscriber persists events with `event_id` for downstream dedup.
If A is down, B's persistence covers the gap. If both are down, the
module's ring covers ~25 minutes; longer gaps lose events.

## Tier 2 — State (call lifecycle, IVR state)

**Loss tolerance**: < 0.1% per hour. Most state events are recoverable
in principle (the eventual CHANNEL_HANGUP_COMPLETE Tier-1 event carries
enough info to reconstruct most state), but losing many of them breaks
real-time dashboards and IVR flows.

**Routing default**:

| FS event | Notes |
|---|---|
| `CHANNEL_CREATE` | New call started |
| `CHANNEL_PROGRESS` | Early-media indication |
| `CHANNEL_PROGRESS_MEDIA` | Early media with SDP |
| `CHANNEL_HANGUP` | Hangup initiated (not yet complete; see Tier 1 for HANGUP_COMPLETE) |
| `CHANNEL_OUTGOING` | Outbound leg started |
| `CHANNEL_ORIGINATE` | Originate request |
| `CHANNEL_PARK` / `CHANNEL_UNPARK` | IVR park state |
| `CHANNEL_STATE` / `CHANNEL_CALLSTATE` | State-machine transitions |
| `DTMF` | User input — usually Tier 2; ops may move to Tier 1 if IVR-business-critical |
| `PRESENCE_IN` / `PRESENCE_OUT` | Status board updates |
| `CUSTOM` with subclass NOT matching `*::critical` or `*::debug` or `osw::audit::*` | Default for custom events |

**Backpressure policy**: same as Tier 1 (evict oldest on ring overflow,
kick subscriber on per-subscriber overflow), but with smaller ring
(8192 default).

## Tier 3 — Ephemeral (heartbeats, debug, high-volume)

**Loss tolerance**: best-effort. Drops on overflow are expected.

**Routing default**:

| FS event | Volume notes |
|---|---|
| `HEARTBEAT` | 1 per 30s system-wide |
| `SESSION_HEARTBEAT` | 1 per 5s per call (50 CCU = 10/s) |
| `RE_SCHEDULE` | Internal FS scheduler; chatty |
| `MODULE_LOAD` / `MODULE_UNLOAD` | Rare, admin-only |
| `STARTUP` / `SHUTDOWN` | Once-per-process |
| `LOG` | High-volume diagnostic; usually filtered |
| `TRAP` | SNMP-style alerts |
| `CODEC` | Codec negotiation debug |
| `RTCP_MESSAGE` | RTCP stats; can be very high volume |
| `API` | API command audit (info only) |
| `BACKGROUND_JOB` | Async job results |
| `CUSTOM` with subclass matching `*::debug` | Developer / debug events |

**Backpressure policy**: same eviction semantics; smaller ring (4096
default). For very high-volume events (RTCP_MESSAGE, etc.) operators
may downsample at routing time via custom rule attributes.

## Routing rules — config schema

The XML config (`open_switch.conf.xml`) maps FS event names (and
optionally subclasses) to a tier. Rules are evaluated top-down; first
match wins. A wildcard `*` catches anything unmatched.

```xml
<event-routing>
  <rule match="CHANNEL_ANSWER"              tier="1"/>
  <rule match="CHANNEL_BRIDGE"              tier="1"/>
  <rule match="CHANNEL_HANGUP_COMPLETE"     tier="1"/>
  <rule match="RECORD_START"                tier="1"/>
  <rule match="RECORD_STOP"                 tier="1"/>
  <!-- ... -->
  <rule match="CUSTOM" subclass="*::critical"   tier="1"/>
  <rule match="CUSTOM" subclass="osw::audit::*" tier="1"/>
  <rule match="CUSTOM" subclass="*::debug"      tier="3"/>
  <rule match="CUSTOM" subclass="*"             tier="2"/>
  <rule match="*"                               tier="2"/>
</event-routing>
```

Compared to the previous draft, the `sink` attribute is gone — there is
only one transport (gRPC streaming). The tier tag remains as
metadata on the envelope for subscriber routing.

Rule semantics:

- `match` matches the event name (case-sensitive).
- `subclass` matches the `Event-Subclass` header for `CUSTOM` events.
- If `match="CUSTOM"` and no `subclass` is given, the rule matches
  every CUSTOM regardless of subclass.

## Header allowlist

FS events have many headers (some calls > 100). Shipping all of them
on every event is wasteful. The module config picks an explicit
allowlist:

```xml
<event-headers>
  <include name="Unique-ID"/>
  <include name="Channel-Call-UUID"/>
  <include name="Caller-Caller-ID-Name"/>
  <!-- ... -->
</event-headers>
```

Default list (see `open_switch.conf.xml.sample`) covers the headers
needed for billing + IVR state. Operators add/remove based on their
downstream consumers' needs.

`variable_*` headers are NOT included by default. They are added per
the `<channel-variable-includes>` allowlist:

```xml
<channel-variable-includes>
  <include name="tenant_id"/>
  <include name="campaign_id"/>
  <include name="agent_id"/>
  <include name="traceparent"/>
</channel-variable-includes>
```

The intent is to keep envelopes small (target < 4 KB for Tier 1) to
preserve gRPC stream throughput.

## Envelope schema

The wire format is `open_switch.events.v1.EventEnvelope` (protobuf,
defined in `proto/open_switch/events/v1/events.proto`).

Stable fields (never change tags):

- `event_id` (UUIDv7) — for consumer dedup.
- `tier` — classification (1/2/3).
- `event_name` — FS event name.
- `subclass_name` — for CUSTOM events.
- `node_id` — FS instance identifier.
- `emitted_at` — FS-supplied timestamp.
- `seq` — monotonic per (node, tier). Subscribers use this for
  `since_seq` replay.
- `tenant_id` — scope.
- `traceparent` — W3C trace context.
- `channel_uuid` — for call-related events.
- `variables` — selected channel variables.
- `headers` — selected FS event headers.
- `body` — raw FS event body, for events that have one.
- `schema_version` — increment only on breaking changes.

Schema evolution policy:

- New fields take new tag numbers. Existing tags are NEVER reused
  even after field removal.
- The `schema_version` field is the consumer's contract; consumers
  MUST check it and reject envelopes with unknown major version.
- Breaking changes (e.g., field semantics change without renaming)
  bump the major schema version. The module emits both old and new
  for one release cycle to allow rolling consumer upgrades.

## Sequencing guarantees

- Within a `(node_id, tier)` stream, the `seq` field is strictly
  increasing. Subscribers can detect gaps.

- **Where `seq` is allocated**: on the **producer side** (the FS
  event-bind callback thread), with a per-tier
  `std::atomic<uint64_t>` counter. The seq is assigned BEFORE the
  envelope is pushed onto the ring. This guarantees ordering matches
  enqueue order. Phase 1 Codex review (I-10) flagged this as
  unspecified; it is now explicit.

- Across tiers, ordering is NOT guaranteed. A Tier 1 CHANNEL_ANSWER
  may arrive at the subscriber after a Tier 2 CHANNEL_PROGRESS for
  the same call, because each tier has its own ring + send pipeline.

- Per-channel ordering within a tier IS preserved: all events for a
  given channel pass through the same FS event thread → same tier
  ring → same per-subscriber queue.

Subscribers needing total ordering across tiers for a single channel
should:

- Subscribe to all relevant tiers.
- Sort by `emitted_at` in a windowed buffer (e.g., 1-second window).
- Use `seq` per tier to detect gaps within a stream.

## `since_seq` replay (subscriber-side resumption)

Subscribers track the highest `seq` they have successfully processed
per (node, tier). On reconnect after a drop:

```protobuf
SubscribeEventsRequest {
  since_seq:    1003       // resume from seq 1004
  node_id:      "fs-1"     // filter by node
  tiers:        ["TIER_1_CRITICAL", "TIER_2_STATE"]
}
```

Module behavior:

| Case | Response |
|---|---|
| `seq=1004` still in ring | Replay ring from 1004 to ring tail, then continue live |
| `seq=1004` evicted from ring | Return `RESOURCE_EXHAUSTED`; subscriber retries without `since_seq` and accepts the gap |
| `since_seq=0` or omitted | Live tail only (no replay) |

Replay window depends on ring size:

| Tier | Default ring | Approx replay @ default rate |
|---|---|---|
| 1 | 16384 | ~25 min @ 10 ev/s |
| 2 | 8192 | ~3 min @ 40 ev/s |
| 3 | 4096 | ~20 s @ 200 ev/s |

Operators tune via config (`event_ring_capacity_tierN`).

## Configuration knobs

```xml
<settings>
  <!-- Per-tier replay ring capacities -->
  <param name="event_ring_capacity_tier1" value="16384"/>
  <param name="event_ring_capacity_tier2" value="8192"/>
  <param name="event_ring_capacity_tier3" value="4096"/>

  <!-- Subscriber-side controls -->
  <param name="max_subscribers"                value="16"/>
  <param name="subscriber_send_queue_capacity" value="4096"/>
</settings>
```

Validation rules applied at module load:

- Each `event_ring_capacity_tierN` ≥ 256 (smaller is pointless).
- Sum of all ring capacities ≤ 1048576 (1M total entries; hard cap to
  prevent a config typo from allocating gigabytes).
- `max_subscribers` in [1, 64].
- `subscriber_send_queue_capacity` in [256, 65536].

A bad value rejects module load with a clear error message.

## Test plan

| Test | What it proves |
|---|---|
| `tier_classification_default` | Fire one CHANNEL_ANSWER, one CHANNEL_STATE, one HEARTBEAT. Assert each lands in the expected tier ring. |
| `tier_classification_custom_subclass` | Fire CUSTOM with subclass `osw::test::critical`, `osw::test::debug`, `osw::test::unknown`. Assert tier 1, 3, 2 respectively. |
| `seq_monotonic_per_tier_node` | Generate 10000 events; assert seq is strictly increasing per (node, tier). |
| `ring_overflow_evicts_oldest` | Saturate Tier 3 ring (4096 cap) with 6000 events. Assert oldest 2000 are gone; tail is current. Drop counter = 2000. |
| `tier1_overflow_emits_warning_event` | Saturate Tier 1 ring with no subscribers. Assert `osw::tier1_ring_overflow` events appear in Tier 2 ring (not Tier 1, to avoid recursion). |
| `subscriber_kick_on_queue_full` | One slow subscriber (drain 10 ev/s), produce 1000 ev/s. Assert stream closed with RESOURCE_EXHAUSTED after queue full; counter increments. |
| `since_seq_replay_inside_window` | Subscribe with `since_seq=N`, where N is in ring. Assert replay from N+1 in order, then live tail. |
| `since_seq_replay_outside_window` | Subscribe with `since_seq=N`, where N is too old. Assert RESOURCE_EXHAUSTED. |
| `multi_subscriber_independent_pacing` | Subscriber A fast, B slow. Assert A is unaffected by B's slowness. B eventually kicked. |
| `shared_serialization_no_amplification` | With 5 subscribers consuming, assert protobuf serialization happens once per event (measure via instrumented serializer). |
| `header_allowlist` | Fire event with 50 headers; assert envelope contains only allowlisted headers. |
| `variable_allowlist` | Fire event with 20 variable_* headers; assert envelope contains only allowlisted variables. |
| `envelope_schema_stability` | Generate envelopes; deserialise with a v0.1 proto. Assert forward compatibility (unknown fields don't break parsing). |
| `dedup_by_event_id` | (Integration test) Module produces same event_id only once across all subscribers. Subscribers can dedup safely. |

## Operator notes

### No-loss reference architecture

For deployments requiring no-loss on Tier 1 events:

1. Run module with `max_subscribers ≥ 2`.
2. Deploy two subscribers per node (different hosts, different
   availability zones if possible).
3. Each subscriber:
   - Subscribes with no `since_seq` initially.
   - On every received envelope, persists to durable store
     (Kafka / Redis Stream / write-ahead log) keyed by `event_id`.
   - ACKs to its own consumer pipeline only after persistence.
   - On reconnect: uses the highest persisted `seq` as `since_seq`.
4. Downstream pipeline reads from the durable store and dedups by
   `event_id`.

This pattern + the module's HA-friendly broadcast yields effective
no-loss as long as ≤ 1 subscriber is down at a time AND the replay
ring covers the recovery window.

### Single-subscriber best-effort

For best-effort deployments (dashboards, internal tools, dev):

1. Run module with one subscriber.
2. Accept gaps during subscriber restarts.
3. Use `since_seq` to minimise the gap window.

## Non-features (V1)

- **No exactly-once semantics**. We provide at-least-once + dedup via
  `event_id`. Exactly-once needs distributed consensus we don't want
  to take on.
- **No per-tenant subscriber filtering at module side**. Subscribers
  receive all events for the node and filter by `tenant_id` themselves.
- **No CDR generation in the module**. FS emits CHANNEL_HANGUP_COMPLETE
  with billing-relevant variables; downstream consumers build CDR.
- **No module-side replay beyond ring window**. Replay across module
  restart or > ring-window is the subscriber's problem (persist to
  durable store as above).
- **No multi-region replication**. V1 ships per-node module; each
  node's subscribers handle their own node's events.
