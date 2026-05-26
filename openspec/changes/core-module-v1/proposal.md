# Core module V1 — proposal

**Status**: Phase 1 specs (drafting).
**Owner**: Opus 4.7 (orchestrator) + sub-agent implementation TBD.
**Target**: First deployable version of `mod_open_switch.so`.

## Why

FreeSWITCH operators bridging FS to modern stacks today must compose
several legacy mechanisms:

- `mod_event_socket` (ESL) for event consumption — text MIME-formatted,
  single TCP consumer per connection, no durability, no replay.
- A second TCP/HTTP layer for command and control (originate, hangup, ...).
- A third layer for audio bridging (mod_audio_stream, mod_audio_fork,
  or custom modules) — each with its own protocol and operational shape.
- Eventually a custom gRPC module written one-off per project to glue it
  together.

The result is multiple sockets, multiple protocols, multiple security
postures, and a thicket of code that is hard to test in isolation.

`mod_open_switch` collapses these into one module exposing three planes
on a single gRPC port (with optional Redis sinks for events):

1. **Control plane** — gRPC API for `Originate`, `Hangup`, `Bridge`,
   `Hold`, `Transfer`, `Execute`, `SetVariables`, `SubscribeEvents`,
   `Health`. Idempotent via `request_id`. mTLS-capable.
2. **Event plane** — FS events bound by the module, routed through
   tiered transports (Redis Streams for critical, Redis Pub/Sub for
   ephemeral) plus an in-memory gRPC server-stream fallback for tail use.
3. **Media plane** — per-call bidirectional gRPC stream from the module
   (client) to an external service (server) for TTS playback, STT
   transcription, voicebot duplex, AMD detection, or recording relay.
   Implemented via FreeSWITCH media bug, supporting multiple bugs
   per call with explicit priority ordering.

The module coexists with `mod_event_socket` and does not deprecate it.
Operators choose which to load; both may be loaded simultaneously.

## What we are building (V1 scope)

| Subsystem | Includes | Excludes (deferred) |
|---|---|---|
| Control gRPC | Originate, Hangup, HangupMany, Bridge, Execute, SetVariables, Hold/Unhold, BlindTransfer, SubscribeEvents, Health | Attended transfer state machine, conference control, presence |
| Event routing | 3-tier model, Redis Streams sink, Redis Pub/Sub sink, in-memory gRPC stream sink, configurable header allowlist | Kafka/NATS sinks (plug-in interface present), CDR enrichment pipeline |
| Media bridge | Multi-bug-per-call manager, bidi gRPC streams, resampling (8/16/24/48 kHz), G.711/PCM/Opus, stereo split for recording relay | Inbound DTMF generation, mid-stream codec renegotiation, video |
| Recording | Bug-priority architecture so FS native recording captures bot audio; spec for stereo vs mono | Built-in recording sink (use FS native `record_session`) |
| Eavesdrop policy | Channel-variable flag, deny/audit/allow policy, audit-event emission | Per-supervisor RBAC matrix |
| Security | mTLS, API key per tenant, dial-context ACL, per-tenant rate limiting | OAuth2, SPIFFE identities, per-method authorization |
| Observability | Structured JSON logs via FS log facility, basic in-memory counters via `Health` RPC | Prometheus / OTel — V2 |
| Lifecycle | Graceful drain, hot config reload, idempotent shutdown | Process-level supervisor (use FS native) |
| Memory safety | RAII helpers (SessionLock, EventGuard, MediaBugLease), ASAN/LSAN in CI, nightly Valgrind, clang-tidy gates | Formal verification — N/A |

## Non-goals

- Replacing FreeSWITCH. We integrate; FS still does SIP/RTP/codec/NAT.
- Replacing `mod_event_socket`. We coexist.
- Hosting orchestration logic (call flow). Module is plumbing.
- Persisting CDR / call records ourselves. Tier-1 events go to Redis;
  downstream consumers persist as appropriate.
- Multi-region replication of events. Single-cluster Redis V1.

## Exit criteria (V1 ship gate)

1. Module loads cleanly into FreeSWITCH 1.10.12 on Debian trixie
   amd64 + arm64.
2. CI green (build + ASAN + LSAN + unit + integration).
3. Nightly Valgrind run reports zero definite leaks across the full
   integration suite for 7 consecutive nights.
4. 50 CCU soak (10-minute, 50 concurrent calls, mix of Originate +
   bot duplex + STT + recording) passes without:
   - any goroutine/socket/dialog leak,
   - any audio gap > 60 ms,
   - any cancel propagation > 100 ms p99,
   - any Tier-1 event loss measurable at the Redis consumer.
5. Eavesdrop policy enforcement test passes:
   - `deny`: eavesdrop attempt hangs up the eavesdropping channel,
     audit event fires.
   - `audit`: eavesdrop succeeds, audit event fires at Tier 1.
   - `allow`: no enforcement.
6. Documentation complete: install, upgrade, troubleshooting, security
   threat model.

## Phase 1 deliverables (this drop)

This proposal is accompanied by the following spec documents in
`designs/` and `specs/`:

- `designs/architecture.md` — the 3-plane component map.
- `designs/memory-management.md` — RAII patterns, tooling, checklist.
- `designs/event-tiers.md` — durability taxonomy + routing rules.
- `designs/transport-adr.md` — why Redis (Streams + Pub/Sub), plug-in
  interface for Kafka/NATS later.
- `designs/media-bridge.md` — media bug manager, multi-bug coordination.
- `designs/call-transcribe.md` — STT path through media bridge.
- `designs/recording-with-bot.md` — bug priority for native recording.
- `designs/security-and-eavesdrop.md` — threat model + policy enforcement.
- `specs/control-api/spec.md` — gRPC service contracts, error model,
  idempotency.

Implementation Phase 2 (subtask breakdown in `tasks.md`, to follow after
spec sign-off) covers the actual `src/` code drop.

## Risks (top 5)

1. **Memory safety in C++ FS module**. Mitigated by: ASAN/LSAN/Valgrind in
   CI, RAII helpers as code requirement, dedicated `memory-management.md`
   spec, code review checklist.
2. **gRPC version drift vs FreeSWITCH build**. Mitigated by: linking
   against a known gRPC version (`v1.74.x`) installed to `/opt/grpc` via
   the builder image, `-Wl,--exclude-libs,ALL` to isolate from other FS
   modules linking different gRPC versions.
3. **Event loss under consumer slowness**. Mitigated by: Tier 1 uses
   Redis Streams with bounded MAXLEN (configurable), backpressure
   visible via metrics, slow-consumer drop policy is explicit per tier.
4. **Eavesdrop policy bypass**. Mitigated by: hook at the dialplan +
   state-handler layers, not just the gRPC API; threat model documented;
   audit events are Tier-1 themselves.
5. **License confusion (AGPL ↔ commercial)**. Mitigated by: README +
   LICENSE clearly state AGPL-3.0-or-later; commercial dual-licensing
   contact path documented.

## References

- [README.md](../../../README.md)
- [LICENSE](../../../LICENSE) — AGPL-3.0-or-later
- [Architecture](designs/architecture.md)
- [Memory management](designs/memory-management.md)
- [Event tiers](designs/event-tiers.md)
- [Transport ADR](designs/transport-adr.md)
- [Media bridge](designs/media-bridge.md)
- [Call transcribe](designs/call-transcribe.md)
- [Recording with bot](designs/recording-with-bot.md)
- [Security + eavesdrop](designs/security-and-eavesdrop.md)
- [Control API](specs/control-api/spec.md)
