# Event tiers — durability taxonomy and routing

## Why tiers exist

FreeSWITCH emits a wide variety of events. Treating all of them with
the same durability and ordering guarantees is either wasteful (high
volume) or unsafe (lose a billing event).

We classify events into three tiers based on the question:
**"If we lost this event, what breaks?"**

| Tier | If lost, what breaks? | Acceptable loss rate |
|---|---|---|
| 1 (Critical) | Billing wrong, CDR incomplete, compliance gap | **Zero** |
| 2 (State) | UI / dashboards stale; recoverable from later state | < 0.1% |
| 3 (Ephemeral) | Nothing meaningful breaks | Best-effort |

Each tier maps to a transport (`event-sinks` in config) and a
backpressure policy. See [`transport-adr.md`](transport-adr.md) for
why Redis Streams + Pub/Sub were chosen.

## Tier 1 — Critical (billing / CDR / compliance)

**Loss tolerance**: zero. Module rather backs up internal queues
indefinitely than drops a Tier 1 event.

**Transport requirements**:
- Durable persistence (disk + ideally replication).
- Ordered per `node_id` (per FS instance).
- At-least-once delivery (consumer must dedup via `event_id`).
- Replay-capable (consumer crash → resume from offset).

**Routing default** (configurable):

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

**Backpressure policy**: NEVER drop. If Redis is unreachable, the
module's Tier 1 in-memory ring (4096 envelopes) buffers. When the ring
fills:

1. Log `osw::tier1_backpressure` at `error` level.
2. Stop accepting new events into the ring; `switch_event_bind`
   callback BLOCKS (this back-pressures the FS event thread).
3. Emit a custom Tier 1 event `osw::transport_degraded` (which will
   itself wait — we accept this).
4. Continue retrying Redis indefinitely with exponential backoff
   (1s → 30s cap).

This is intentional. Blocking FS event delivery is bad, but losing a
billing event is worse. In practice, Tier 1 volume is ~10 ev/s; the
4096 ring holds ~7 minutes of buffering at peak.

**Recovery**: when Redis returns, drain the ring; events fire in
order. Consumer dedup via `event_id` is required because retries may
produce duplicates.

**Configured sink in V1**: Redis Streams (`osw.events.tier1`) with
`MAXLEN ~ 100000` (approximate trim). At ~10 ev/s × 86400 s/day =
864000 events/day; 100k entries = ~3 hours of replay window. Consumers
that lag more than 3 hours lose ground; operator can increase MAXLEN
at the cost of Redis memory.

## Tier 2 — State (call lifecycle, IVR state)

**Loss tolerance**: < 0.1%. Events are recoverable in principle (the
final CHANNEL_HANGUP_COMPLETE Tier-1 event carries enough info to
reconstruct), but losing many of them breaks dashboards and IVR
flows.

**Transport requirements**:
- Persistence preferred but not strictly required.
- Best-effort ordering per channel UUID.
- At-least-once delivery preferred.
- Some replay capability (last hour or so).

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
| `CUSTOM` with subclass NOT matching `*::critical` or `*::debug` | Default for custom events |

**Backpressure policy**: bounded retry then drop with metric.

1. Tier 2 ring fills (16384 entries).
2. Shipper thread retries Redis up to 30s with exponential backoff.
3. If still failing: drop the oldest event in the ring (NEW events are
   more useful than stale ones).
4. Emit a Tier 1 event `osw::tier2_dropped` summarising loss counts
   per minute.

**Configured sink in V1**: Redis Streams (`osw.events.tier2`) with
`MAXLEN ~ 50000` (approximate trim). ~30-50 ev/s × 86400 = 2.5-4.3M
events/day; 50k entries = ~15-30 minutes of replay window.

## Tier 3 — Ephemeral (heartbeats, debug, high-volume)

**Loss tolerance**: best-effort. If no consumer is attached, events
disappear. Consumer slowness → drop.

**Transport requirements**:
- No persistence.
- No ordering guarantees.
- At-most-once delivery.
- Multiple subscribers ok.

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

**Backpressure policy**: drop immediately when ring full. No retry.
Emit a Tier 1 event `osw::tier3_dropped` once per minute summarising
the drop count.

**Configured sink in V1**: Redis Pub/Sub (`osw.events.tier3`). No
persistence; subscribers that aren't connected miss messages.

For operators who want a tail without setting up Redis at all, the
gRPC `SubscribeEvents` stream also receives Tier 3 events (in-memory
ring, slow-consumer = drop). This is meant for ops console / debug
use, NOT for production monitoring.

## Routing rules — config schema

The XML config (`open_switch.conf.xml`) maps FS event names (and
optionally subclasses) to a tier and a sink. Rules are evaluated
top-down; first match wins. A wildcard `*` catches anything
unmatched.

```xml
<event-routing>
  <rule match="CHANNEL_ANSWER"              tier="1" sink="tier1-critical"/>
  <rule match="CHANNEL_BRIDGE"              tier="1" sink="tier1-critical"/>
  <rule match="CHANNEL_HANGUP_COMPLETE"     tier="1" sink="tier1-critical"/>
  <!-- ... -->
  <rule match="CUSTOM" subclass="*::critical" tier="1" sink="tier1-critical"/>
  <rule match="CUSTOM" subclass="*::debug"    tier="3" sink="tier3-ephemeral"/>
  <rule match="CUSTOM" subclass="*"           tier="2" sink="tier2-state"/>
  <rule match="*"                             tier="2" sink="tier2-state"/>
</event-routing>
```

`subclass` is a `fnmatch`-style glob.

Rule semantics:
- `match` matches the event name (case-sensitive).
- `subclass` matches the `Event-Subclass` header for `CUSTOM` events.
- If `match="CUSTOM"` and no `subclass` is given, the rule matches
  every CUSTOM regardless of subclass.
- Tier is a hint to the shipper for backpressure policy; the sink
  determines actual transport.

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

The intent is to keep events small: the entire envelope SHOULD fit
in < 4 KB on the wire for Tier 1 events to keep Redis throughput
sane.

## Envelope schema

The wire format is `open_switch.events.v1.EventEnvelope` (protobuf,
defined in `proto/open_switch/events/v1/events.proto`).

Stable fields (never change tags):
- `event_id` (UUIDv7) — for consumer dedup.
- `tier` — classification.
- `event_name` — FS event name.
- `subclass_name` — for CUSTOM events.
- `node_id` — FS instance identifier.
- `emitted_at` — FS-supplied timestamp.
- `seq` — monotonic per (node, tier).
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
  increasing. Consumers can detect gaps. Gaps within Tier 1 are bugs
  and trigger alerts.
- Across tiers, ordering is NOT guaranteed. A Tier 1 CHANNEL_ANSWER
  may arrive at the consumer after a Tier 2 CHANNEL_PROGRESS for the
  same call, because Tier 1 and Tier 2 are shipped on independent
  threads to independent streams.
- Per-channel ordering within a tier IS preserved, because all events
  for a given channel pass through the same FS event thread → same
  tier ring → same shipper thread → same Redis stream.

If a consumer needs total ordering across tiers for a single channel,
the recommended pattern is:
- Subscribe to all relevant streams.
- Sort by `emitted_at` over a windowed buffer (e.g., 1-second window).
- Use `seq` to detect gaps within a stream.

This is a consumer-side concern. The module does not attempt total
ordering across tiers because it would require a single shared
serialization point, which would become a bottleneck.

## Test plan

| Test | What it proves |
|---|---|
| `tier1_no_loss_under_redis_outage` | Stop Redis for 2 minutes, generate 1000 Tier 1 events at 10/s. Bring Redis back. Consumer receives all 1000 with no gaps. |
| `tier2_bounded_loss_under_redis_outage` | Stop Redis for 5 minutes, generate 30000 Tier 2 events at 100/s. Bring Redis back. Consumer receives the most recent ~16k (ring capacity) with reported drop count metric. |
| `tier3_drops_on_full_ring` | Saturate Tier 3 with 100000 events/s. Verify ring drops oldest, drop counter increments, FS event thread does not block. |
| `tier_routing_default` | Fire one CHANNEL_ANSWER, one CHANNEL_STATE, one HEARTBEAT. Assert each lands on its expected sink and tier. |
| `tier_routing_custom` | Fire CUSTOM with subclass `osw::test::critical`, `osw::test::debug`, `osw::test::unknown`. Assert tier 1, 3, 2 respectively. |
| `envelope_schema_stability` | Generate envelopes; deserialize with a v0.1 proto. Assert forward compatibility (unknown fields don't break parsing). |
| `seq_monotonic` | Generate 10000 events; assert seq is strictly increasing per (node, tier). |
| `dedup_on_retry` | Force Redis transient error; ring retries succeed. Consumer (using event_id) dedupes. Assert no duplicate processing. |

## Operator-tunable knobs

| Knob | Default | Purpose |
|---|---|---|
| Tier 1 ring capacity | 4096 | Buffering during Redis outage; higher = more memory, more buffer time |
| Tier 2 ring capacity | 16384 | Higher = more buffer; tradeoff vs drop tolerance |
| Tier 3 ring capacity | 8192 | Drops are expected; lower = more drops, less memory |
| Tier 1 Redis MAXLEN | 100000 | Replay window length; higher = more Redis memory |
| Tier 2 Redis MAXLEN | 50000 | Same tradeoff |
| Header allowlist | curated default | Add headers needed by downstream consumers |
| Variable allowlist | curated default | Add channel vars needed by downstream consumers |
| Tier-1 retry backoff cap | 30s | Aggressive retry; cap prevents thundering herd on Redis recovery |

## Non-features (V1)

- **No exactly-once semantics**. We provide at-least-once + dedup via
  `event_id`. Exactly-once needs distributed consensus we don't want
  to take on.
- **No event filtering by tenant at sink boundary**. All events for a
  node go to one Redis stream per tier. Consumers filter by
  `tenant_id` after consumption. Per-tenant streams are a deferred
  feature (would require thousands of streams for some operators).
- **No CDR generation in the module**. FS emits CHANNEL_HANGUP_COMPLETE
  with billing-relevant variables; downstream consumers build CDR. The
  module is a bus, not a billing system.
- **No event replay from the module side**. Replay is a consumer
  feature using Redis Streams' XREAD offsets. The module never
  re-emits past events.
- **No multi-region replication**. V1 ships single-cluster Redis. For
  multi-region, run a separate Redis cluster per region and stitch at
  consumer side, OR add Redis cluster replication (out of module
  scope).
