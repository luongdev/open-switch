# ADR: Event transport for mod_open_switch V1

**Status**: Accepted
**Date**: 2026-05-26
**Deciders**: project owner + Opus 4.7 orchestrator

## Context

The module's event plane (see [`event-tiers.md`](event-tiers.md))
emits three tiers of events from FreeSWITCH:

- Tier 1 (critical, billing-grade): ~10 ev/s typical, must not lose.
- Tier 2 (state, recoverable): ~30-50 ev/s typical, tolerate < 0.1% loss.
- Tier 3 (ephemeral, high-volume): ~100-500 ev/s, best-effort.

We need to pick transport(s) that satisfy these constraints, are
operationally cheap, are well-supported from C++ inside the FS process,
and are AGPL-compatible.

The project owner has explicitly:

- ruled out Kafka ("không xài"),
- ruled out Consul/etcd, Prometheus/Grafana for V1,
- prefers Redis ("redis cài dễ"),
- requires the module to "tuân thủ FS" — no heavy telemetry/control
  plane bolted on at this phase.

## Decision

**V1 transport: Redis Streams (Tier 1, Tier 2) + Redis Pub/Sub (Tier 3).**

A pluggable `EventSink` interface is added from day one so that Kafka,
NATS, or other backends can be added later without rewriting the
event-routing layer. **Only the Redis implementations ship in V1.**

## Alternatives considered

### A. Redis Streams + Redis Pub/Sub (chosen)

**Pros**:

- One transport system to operate. Operators commonly already have
  Redis.
- Redis Streams provide durability (AOF + RDB), consumer groups,
  replay via offsets, at-least-once + dedup-capable, ordered per stream.
- Redis Pub/Sub is ideal for fire-and-forget Tier 3 (no broker storage
  overhead).
- Sub-millisecond latency typical on LAN.
- Mature C client: `hiredis` (BSD) → wrapped by `redis-plus-plus` (Apache 2.0).
- `redis-plus-plus` supports async ops, connection pooling, cluster mode,
  Sentinel.
- Memory-bounded streams via `XADD MAXLEN ~` (approximate trim).

**Cons**:

- Single-master per stream key. Scale-out requires sharding by stream
  name, which doesn't matter at our scale (< 100k ev/s).
- Persistence is RAM-bounded; AOF replays on restart but can be slow.
- Not as battle-tested as Kafka at 1M+ ev/s — but we're at 100/s, so
  irrelevant.
- Multi-region replication is harder than Kafka. V1 is single-cluster
  per region.

**Verdict**: Best fit for our scale + ops cost target.

### B. Kafka + NATS (rejected by owner)

**Pros**:

- Industry standard. Kafka for durable, NATS for fire-and-forget.
- Replay, multi-consumer, high throughput, multi-region capable.

**Cons**:

- Two systems to operate. Owner doesn't run Kafka today.
- Kafka minimum 3-broker HA + ZK/KRaft + monitoring. Heavy for a
  contact-center sized deployment.
- librdkafka and nats.c both mature, but adds two C library deps to
  the FS module.

**Verdict**: Rejected by owner. Listed for completeness; the plug-in
interface allows operators to add later.

### C. NATS JetStream + NATS core

**Pros**:

- Single-binary deployment. Lightweight.
- JetStream provides durability + consumer groups + replay.
- Modern wire protocol; small footprint.

**Cons**:

- `nats.c` client is less mature than `hiredis` (smaller community).
- Adding another infra component for the operator. They already need
  Redis (or some equivalent) for sentence-cache use cases adjacent
  to this stack.
- C++ API ergonomics weaker than `redis-plus-plus`.

**Verdict**: Solid alternative; not picked because operator already
has/wants Redis.

### D. gRPC server-streaming only (no broker)

**Pros**:

- Reuses gRPC stack already built for control plane.
- No additional infrastructure.
- TLS / auth via gRPC standard.

**Cons**:

- No durability. Consumer crash = events lost during downtime.
- No replay.
- Memory pressure if subscribers fall behind.
- Reinvents pub/sub poorly.

**Verdict**: Used as a FALLBACK only — the `SubscribeEvents` RPC
provides this for operators who don't want any Redis. Not suitable
as the primary Tier 1 / Tier 2 transport.

### E. PostgreSQL LISTEN/NOTIFY

**Pros**:

- If operator already has Postgres for other things.

**Cons**:

- 8 KB payload limit per NOTIFY; our envelopes can exceed.
- Notifications are not durable beyond the LISTEN session.
- Backing each notification with a table row adds significant write
  load to Postgres.

**Verdict**: Niche; not picked.

### F. RabbitMQ

**Pros**:

- Mature, well-understood.

**Cons**:

- Heavier ops than NATS or Redis.
- AMQP semantics overkill for our needs.
- Throughput lower than Kafka at high CCU.

**Verdict**: Operationally heavier than Redis without clear benefit
for our scale.

### G. Apache Pulsar

**Pros**:

- Modern, multi-tenancy, geo-replication.

**Cons**:

- Three subsystems (broker + bookkeeper + ZK). Heaviest of the
  options. Overkill.

**Verdict**: Rejected.

## Decision matrix

| Criterion | Weight | A: Redis | B: Kafka+NATS | C: NATS | D: gRPC only | E: PG LISTEN | F: RabbitMQ | G: Pulsar |
|---|---|---|---|---|---|---|---|---|
| Operator ops cost | 0.30 | **9** | 3 | 7 | 10 | 8 | 5 | 1 |
| Durability for Tier 1 | 0.25 | 8 | **10** | 8 | 1 | 6 | 8 | 10 |
| Latency p99 | 0.15 | **9** | 7 | 9 | 10 | 7 | 8 | 7 |
| C++ client maturity | 0.10 | **10** | 9 | 7 | 10 | 9 | 8 | 7 |
| Multi-region readiness | 0.05 | 5 | **9** | 7 | 1 | 4 | 6 | 10 |
| Owner alignment | 0.15 | **10** | 0 | 5 | 6 | 4 | 4 | 0 |
| Weighted score | | **8.65** | 5.85 | 7.10 | 6.40 | 6.40 | 6.10 | 4.65 |

Redis wins on weighted score, dominated by owner alignment + ops cost.

## Architecture: pluggable EventSink interface

```cpp
namespace osw::transports {

class EventSink {
 public:
  virtual ~EventSink() = default;

  // Backend identifier ("redis-streams", "redis-pubsub", "grpc-stream", ...).
  virtual std::string_view backend() const = 0;

  // Configuration name (from XML <sink name="...">).
  virtual std::string_view name() const = 0;

  // Synchronous send. Returns OK on durable accept, FAILED otherwise.
  // The caller is expected to invoke this from the shipper thread,
  // not the FS event thread.
  virtual SendResult Send(const events::v1::EventEnvelope& envelope) = 0;

  // Health probe used by the Health RPC.
  virtual HealthStatus Health() const = 0;

  // Graceful drain.
  virtual void Drain(std::chrono::milliseconds timeout) = 0;
};

struct SendResult {
  enum class Code {
    OK,
    RETRY_LATER,   // transient; shipper should back off and retry
    DROP,          // permanent; shipper should drop and record metric
  };
  Code code;
  std::string error;
};

}  // namespace osw::transports
```

V1 implementations:

- `RedisStreamsSink` (Tier 1, Tier 2)
- `RedisPubSubSink` (Tier 3)
- `GrpcStreamSink` (in-process fallback for `SubscribeEvents`)
- `NullSink` (for tests)

V2+ implementations (NOT in V1; plug-in slot present):

- `KafkaSink`
- `NatsJetStreamSink` (durable) + `NatsCoreSink` (fire-forget)
- `FileRotatedSink` (for offline forensics)

## RedisStreamsSink — concrete design

### Wire format

`XADD <stream> [MAXLEN ~ <n>] * envelope <serialized-protobuf>`

The single field name is `envelope`. The value is the protobuf-binary
serialised `EventEnvelope`. We do NOT split into multiple Redis fields
because (a) the envelope is self-describing, (b) field-per-attribute
inflates Redis memory, (c) protobuf forward-compat is easier with one
opaque field.

Stream IDs are Redis-assigned (`*`). The module retains them in logs
for cross-referencing with consumer offsets.

### MAXLEN approximate

`MAXLEN ~ N` (with the `~` modifier) lets Redis trim in bigger chunks
for efficiency. For our use case the variance is acceptable (we treat
MAXLEN as a soft cap, not a precise budget).

### Consumer groups

The module does NOT manage consumer groups. Consumers create groups
on their side via `XGROUP CREATE`. The module is producer-only.

### Connection management

- One `redis-plus-plus` connection per sink instance.
- `redis-plus-plus` `RedisConnection` is thread-unsafe; we use one
  connection per shipper thread (one per tier). Connection pool with
  per-thread-pinned connections — NOT a shared pool, because the
  shipper threads each have a single connection's worth of throughput.
- Reconnect on error (exponential backoff 100ms → 30s cap).
- TLS supported (Redis 6+).
- AUTH via `requirepass` or ACL.

### Failure handling

Per the Tier 1 backpressure policy:

```cpp
auto result = sink.Send(envelope);
switch (result.code) {
  case SendResult::Code::OK:
    // metric: sent
    break;
  case SendResult::Code::RETRY_LATER:
    // for Tier 1: re-queue at head of internal ring, sleep backoff
    // for Tier 2: re-queue at head, bounded retry; after N seconds → drop
    // for Tier 3: drop immediately
    break;
  case SendResult::Code::DROP:
    // unrecoverable; emit Tier 1 alarm event and stop ringing
    break;
}
```

### MULTI/EXEC?

Not used in V1. We send one envelope per XADD. The performance overhead
is acceptable (Redis pipelining via `redis-plus-plus` can batch many
XADDs on the wire). Pipelining is enabled (default in redis-plus-plus
async API).

## RedisPubSubSink — concrete design

### Wire format

`PUBLISH <channel> <serialized-protobuf-binary>`

Single channel per sink configuration.

### No persistence

Pub/Sub is fire-and-forget. Subscribers must be connected to receive.
If the operator wants durability on Tier 3, they should configure that
tier with `RedisStreamsSink` instead (with a small MAXLEN).

### Connection management

Same as Streams (one connection per shipper thread, exponential
reconnect).

### Failure handling

`PUBLISH` itself doesn't fail except on connection error. On connection
error: backoff + retry. Events accumulating in the Tier 3 ring during
the outage will spill out via the ring's drop-oldest policy.

## GrpcStreamSink — concrete design

In-process. The sink retains an in-memory ring buffer (configurable
capacity, default 4096) and a set of subscribers (active gRPC
SubscribeEvents streams). On `Send`:

1. Push envelope to ring.
2. Notify all subscribers via per-subscriber condition variable.
3. Each subscriber's reactor pops from ring and writes to gRPC stream.

If a subscriber is slow, its per-subscriber offset falls behind the
ring tail. When the offset overruns the ring (we've discarded events
since the subscriber last consumed), the subscriber's stream is
closed with `RESOURCE_EXHAUSTED` and the operator must reconnect with
a fresh offset (i.e., they lose events between the last consumed and
the current ring head — by design).

This is NOT a Tier 1 / Tier 2 transport. Operators relying on this
for billing-grade events will lose data and we say so loudly in the
config sample comments.

## Operational guidance (for the operator docs)

### Redis sizing

Per-tier estimate at 50 CCU + 10 calls/min:

| Tier | Events/sec | Bytes/event (avg) | MB/hour | Recommended MAXLEN | Memory at MAXLEN |
|---|---|---|---|---|---|
| 1 | 10 | 1500 | 54 | 100k | ~150 MB |
| 2 | 40 | 2000 | 288 | 50k | ~100 MB |
| 3 | 200 | 800 (Pub/Sub no persist) | — | N/A | ~0 |

Total Redis memory floor: ~250 MB for streams + Redis's own overhead.
Single-node Redis with 1 GB RAM is plenty for V1.

### Redis HA

For HA: Sentinel + replica. The module can be configured with the
Sentinel URL via `redis://<sentinel>?service=mymaster`.

Redis Cluster is NOT required for our throughput. Adding it adds
operational complexity. If operator wants it: redis-plus-plus
supports cluster mode; minor config change.

### TLS

Recommended for cross-host Redis. Set `url=rediss://...` (note `rediss`
scheme). Provide CA cert via `ca_file` param.

### ACL

Use Redis 6+ ACLs to restrict the module's user to:
- `+xadd` on `osw.events.tier1`, `osw.events.tier2`
- `+publish` on `osw.events.tier3`
- `+ping`, `+info`, `+client` for health checks
- Nothing else

Consumer credentials are separate (they need `+xread`, `+xreadgroup`).

## Migration / future work

When the operator wants to add Kafka/NATS for additional consumers:

1. Add `KafkaSink` (or `NatsJetStreamSink`) implementing `EventSink`.
2. Update `open_switch.conf.xml` to add a sink + route some tier to it.
3. The module can fan out to multiple sinks per tier (the routing
   layer supports multiple `<rule sink="..."/>` matches by adding
   more rules with same `match` to different sinks).

The plug-in interface explicitly supports adding sinks without changes
to the routing layer.

## Consequences

- **Operator runs Redis** (already common). One additional Docker
  service.
- **Consumers must dedup via `event_id`** because at-least-once is the
  contract. Consumers must persist their last-seen offset to resume
  after crash.
- **Tier 1 events block the FS event thread under sustained Redis
  outage**. This is acceptable because losing billing events is worse
  than back-pressuring FS for a few minutes. Operators should monitor
  Redis health.
- **Kafka adopters need to wait or contribute a `KafkaSink`** to the
  plug-in. The plug-in interface is stable from V1 so a Kafka sink
  can land later without breaking the module ABI.
