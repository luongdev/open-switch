# ADR: Event transport for mod_open_switch V1

**Status**: Accepted (revised 2026-05-26 after Phase 1 Codex review).
**Date**: 2026-05-26 (rev. 2)
**Deciders**: project owner + Opus 4.7 orchestrator
**Supersedes**: the v1 Redis-Streams-and-PubSub draft of this ADR.

## Context

The module's event plane (see [`event-tiers.md`](event-tiers.md)) emits
three tiers of FreeSWITCH events. The transport question is: how do
those events reach external consumers?

### What changed since the first draft

The first draft of this ADR (committed 2cf00e7, since rewritten) picked
Redis Streams + Redis Pub/Sub as the V1 transport with a pluggable
`EventSink` interface so Kafka / NATS could be added later. Phase 1
Codex review (`reviews/codex-phase1.md`, finding C-4) flagged that the
Tier-1 "never drop" backpressure policy would block the FreeSWITCH
event-dispatch thread under a sustained Redis outage. Because the FS
event facility is the same bus mod_sofia uses to drive SIP state
transitions, blocking that thread cascades into SIP call setup failure.
"Lose billing > stall signaling" is the wrong trade for a SaaS contact
center: a stalled SIP gateway looks dead.

Project owner then proposed a simpler model:

> "Có khi cái events khỏi bắn ra Redis đi, kệ mẹ cho cái gRPC server
> bên ngoài nó xử? Chỉ cần bắn sang thôi là được. Nó đỡ phức tạp hơn
> hẳn, nhưng vẫn nên có mapping."

Translation: drop the in-module Redis transport entirely. Module's job
is to classify, buffer briefly, and stream to external gRPC subscribers.
The external subscribers (already running because they handle TTS / STT
/ voicebot logic) pick their own durability: Kafka, Redis, S3, file,
drop — their choice.

The tier classification stays (it's still useful metadata so subscribers
can route by criticality), but the module no longer commits to durable
delivery itself.

## Decision

**V1 transport: gRPC server-streaming `SubscribeEvents`. This is the
ONLY transport.** No Redis, no Kafka, no NATS in the module.

The pluggable `EventSink` interface from the previous draft is also
**dropped from V1**. Future transports (if anyone asks for direct Redis
producer, direct Kafka producer, etc.) get a new OpenSpec change and a
new sink subsystem at that time, with the freedom to redesign the
abstraction with implementation experience.

## What this means concretely

```text
FS event facility
       │ switch_event_bind callback
       ▼
[ Tier classifier — matches event name + subclass against routing rules ]
       │ tier metadata attached to envelope
       ▼
[ Per-tier in-memory ring (1 per tier) — REPLAY BUFFER, not shipper queue ]
       │ enqueue at tail; oldest evicted at head on overflow
       ▼
[ Subscriber broadcaster — for each active SubscribeEvents stream: ]
       │ - filter envelope by subscriber's tier / event_name filters
       │ - push ref-counted-shared envelope into per-subscriber send queue
       ▼
[ Per-subscriber bounded send queue (1 per stream) ]
       │ gRPC sender thread drains and writes to wire
       ▼
External gRPC server (operator-owned)
       │ operator routes to Kafka / Redis / S3 / file / dashboard / drop
       ▼
Whatever durability policy the operator chose
```

Module's contract (revised):

> "We classify events into tiers. We buffer events briefly in an
> in-memory replay ring. We deliver to any subscriber that is connected
> and can keep up. Durability beyond the replay window is YOUR job."

## Multi-subscriber broadcast

Multiple subscribers may connect simultaneously (default cap: 16).
Common operator pattern is 2 subscribers for HA:

```text
                          ┌──▶ subscriber A (primary)  ──▶ Kafka
mod_open_switch ──events──┤
                          └──▶ subscriber B (standby)  ──▶ Kafka
```

If A fails, B has the events; downstream Kafka dedups by `event_id`.

If both A and B fail (network partition, both crash): events are lost
during the partition. Operator's choice — that's the trade for module
simplicity.

### Shared serialization (zero broadcast amplification)

Each event is serialized to protobuf bytes **once**. The byte buffer is
held by `std::shared_ptr<const std::string>` and the pointer is pushed
into each subscriber's send queue. Memory per event = 1 × payload +
N × 16 bytes overhead (independent of subscriber count).

When all subscribers have drained an envelope past their cursor, the
shared_ptr refcount drops to zero and the buffer is freed. The
per-tier ring buffer holds its own shared_ptr until the entry is
evicted from the ring (FIFO).

### Per-subscriber pacing

Each subscriber has its own bounded send queue (default 4096 envelopes).
A slow subscriber fills its own queue without affecting fast
subscribers. When a subscriber's queue is full, the module:

1. Closes the gRPC stream with `RESOURCE_EXHAUSTED`.
2. Increments `subscriber_kicked_count{cause="queue_full"}` metric.
3. Logs at WARN level.
4. The client reconnects (with `since_seq` if it tracked its last-seen
   sequence) and resumes.

## `since_seq` replay

Each emitted envelope carries a monotonic `seq` per (node, tier). When
a subscriber reconnects after a drop, it may send:

```protobuf
SubscribeEventsRequest {
  since_seq: 1003
}
```

Meaning: "I last consumed seq 1003 inclusive; send me everything from
1004 onward."

Module behavior:

| Case | Response |
|---|---|
| `since_seq + 1` is still in the ring | Replay ring contents from `since_seq + 1` to ring tail, then continue live tail |
| `since_seq + 1` has been evicted from the ring | Return `RESOURCE_EXHAUSTED` with message "since_seq outside replay window"; client retries without `since_seq` and accepts the gap |
| `since_seq` omitted or 0 | Live tail only; no replay |

The replay window depends on ring size + event emission rate. With
defaults (Tier-1 ring=16384, ~10 ev/s) replay covers ~25 minutes.
Operators can tune by setting `event_ring_capacity_tierN` in config.

## Tier-specific behavior

Tier classification still affects backpressure handling, but the
mechanics are now all in-process:

| Tier | Ring overflow policy | Per-subscriber-queue overflow policy |
|---|---|---|
| 1 | Evict oldest. Increment `tier1_ring_overflow_total`. Emit `osw::tier1_ring_overflow` event AT TIER 2 (cannot recurse into Tier 1 ring). | Kick subscriber. |
| 2 | Evict oldest. Counter. | Kick subscriber. |
| 3 | Evict oldest. Counter. | Kick subscriber. |

Note: there is no longer a "Tier 1 never drops" guarantee. The module
**cannot** guarantee no loss without durable storage, and durable
storage is now the subscriber's responsibility. Operators wanting
no-loss SHOULD:

1. Run ≥ 2 subscribers per node in HA.
2. Each subscriber persists to durable store (Kafka topic, Redis Stream,
   etc.) before ACK'ing in their own consumer pipeline.
3. Downstream dedup by `event_id`.

This is documented in the operator guide as the "no-loss reference
architecture".

## Why this simplification is the right call

- **Eliminates Phase 1 Codex finding C-4** by removing the cause: there
  is no Redis to back-pressure on. The FS event-dispatch thread never
  blocks on network I/O.
- **Eliminates redis-plus-plus + hiredis dependencies** from the build.
  Smaller .so, simpler Dockerfile.builder, smaller attack surface.
- **Eliminates Redis ops burden for operators** who don't already run
  Redis: no extra service, no ACL config, no Redis HA design.
- **Pushes the durability decision to the right layer**: the operator
  knows whether they want Kafka, Redis, S3, both, or none. Forcing a
  Redis-only choice in the module constrains them.
- **Module remains a relay, not a queue**: clearer mental model. The
  FS-side concerns (binding, classification, buffering) stay; the
  durability-side concerns (replication, retention, replay across
  restarts) leave.

## What we give up

- **No durability across module restart**: events in flight when the
  module reloads are lost. Mitigation: operators perform module
  reload during low-traffic windows; durable Tier-1 events (e.g.,
  CHANNEL_HANGUP_COMPLETE) usually have FS-side hooks the operator
  can backstop with FS native CDR mechanisms.
- **No durability across consumer-side outages > replay window**:
  if all subscribers are down for longer than the replay ring covers,
  the events outside that window are lost. Mitigation: HA subscriber
  pair as above.
- **No producer-side dedup**: at-most-once per (subscriber, event_id).
  If the module re-emits during a retry path, subscribers see the
  same event twice and must dedup by `event_id`.
- **No replay from cold storage**: there is no module-side cold
  archive. Subscribers wanting "look at last week's events" must
  persist to their own store.

For Tier 1 events specifically (billing-grade), this means the operator
MUST run an HA subscriber pair persisting to durable storage. The
module documents this requirement loudly in the operator guide.

## Alternatives considered (revised)

This ADR reconsidered the same alternatives as the first draft:

### A. Redis Streams + Pub/Sub in-module (previous V1 pick)

**Reason rejected**: C-4 trade-off (FS signaling stall under outage) +
adds ops burden + adds two build deps. Better to push durability to
the subscriber layer.

### B. Kafka + NATS in-module

**Reason rejected**: two services to operate, much heavier than Redis,
already ruled out by project owner.

### C. NATS JetStream in-module

**Reason rejected**: still adds an in-module dep + ops burden for
durability that the subscriber layer can provide more flexibly.

### D. gRPC streaming as sole transport (CHOSEN)

**Pros**:
- Operator already runs gRPC service for media bridge.
- No extra ops surface.
- Module remains pure relay.
- No new build deps.
- Multi-subscriber broadcast for HA is straightforward.
- Eliminates entire class of "what if our durable store is down"
  failure modes.

**Cons**:
- Module cannot promise no-loss alone. Operator must run HA
  subscribers for that promise. Documented loudly.
- Replay window bounded by in-memory ring (default ~25 min for Tier 1).

**Verdict**: chosen. Owner-aligned, simpler, addresses C-4.

### E. PG LISTEN/NOTIFY, RabbitMQ, Pulsar

**Reason rejected**: same as previous draft (PG payload limit,
RabbitMQ ops heaviness, Pulsar 3-subsystems).

## Future work

If a deployment ever needs in-module direct-to-Redis or direct-to-Kafka
production (e.g., for very high-volume events that don't fit a
subscriber-pull model), that capability gets its own OpenSpec change
**after** V1 ships and we have implementation experience. The change
would:

1. Introduce a new sink subsystem with first-class design (informed by
   V1 lessons).
2. Add new config schema for the new sink.
3. Keep gRPC streaming as the default; new sinks are additive.

V1 stays focused. The plug-in interface is NOT pre-built — premature
abstractions hurt; we add the abstraction when there are 2 concrete
implementations to abstract over.

## Consequences

- The `redis-plus-plus` + `hiredis` dependencies are removed from
  `CMakeLists.txt` and `Dockerfile.builder`.
- The Redis runtime container is removed from `compose.yaml.example`.
- The `<event-sinks>` block is removed from
  `open_switch.conf.xml.sample`; `<event-routing>` simplifies to
  tier classification (no `sink` attribute).
- The `SubscribeEventsRequest` proto gains `since_seq` and `node_id`
  fields.
- The Health RPC adds `subscriber_count` to its response.
- Operator documentation must include the HA subscriber pattern as
  the recommended posture for Tier-1 durability.
- The Phase 1 Codex finding C-4 is resolved by deletion of the
  blocking-backpressure design.
