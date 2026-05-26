# Architecture — mod_open_switch V1

## Goal

A single FreeSWITCH module that bridges FS to external services on three
clean planes, sharing a process with FreeSWITCH so the integration cost
collapses to one socket and one configuration file.

## High-level diagram

```text
┌────────────────────────────────────────────────────────────────────────┐
│                    FreeSWITCH process (PID 1 in container)             │
│                                                                        │
│   ┌─── FS core ────────────────────────────────────────────────────┐   │
│   │  mod_sofia (SIP)   mod_dptools   media engine   event facility │   │
│   │       │                 │              │              │         │   │
│   └───────┼─────────────────┼──────────────┼──────────────┼────────┘   │
│           │                 │              │              │            │
│           │  switch_*  C API (linkage at load)             │            │
│           ▼                 ▼              ▼              ▼            │
│   ┌──────────────────────────────────────────────────────────────┐    │
│   │              mod_open_switch.so (this repo)                   │    │
│   │                                                                │    │
│   │  ┌──────────────┐  ┌──────────────┐  ┌────────────────────┐  │    │
│   │  │ Control      │  │ Event        │  │ Media              │  │    │
│   │  │  plane       │  │  plane       │  │  plane             │  │    │
│   │  │              │  │              │  │                    │  │    │
│   │  │ gRPC server  │  │ event_bind() │  │ media_bug_add()    │  │    │
│   │  │  + handlers  │  │  + router    │  │  manager           │  │    │
│   │  │              │  │  + sinks     │  │  per-call bidi     │  │    │
│   │  │ Idempotency  │  │              │  │  gRPC streams      │  │    │
│   │  │  + ACL       │  │ In-memory    │  │  (TTS/STT/         │  │    │
│   │  │  + mTLS      │  │  ring (gRPC  │  │   voicebot/AMD)    │  │    │
│   │  │              │  │  stream tail)│  │  Resampler         │  │    │
│   │  └──────┬───────┘  └──────┬───────┘  └─────────┬──────────┘  │    │
│   │         │                  │                    │              │    │
│   │  ┌──────┴──────────────────┴────────────────────┴───────────┐ │    │
│   │  │           Cross-cutting infrastructure                   │ │    │
│   │  │   RAII helpers · structured log · health counters ·      │ │    │
│   │  │   config loader · graceful drain · idempotency cache     │ │    │
│   │  └────────────────────────────────────────────────────────┬─┘ │    │
│   │                                                            │   │    │
│   │  ┌─────────────────────────────────────────────────────────┴┐ │    │
│   │  │           In-memory per-tier event rings                  │ │    │
│   │  │   ┌──────────────┐  ┌──────────────┐  ┌─────────────────┐ │ │    │
│   │  │   │ Tier 1 ring  │  │ Tier 2 ring  │  │ Tier 3 ring     │ │ │    │
│   │  │   │  16384       │  │  8192        │  │  4096           │ │ │    │
│   │  │   │  (MPSC)      │  │  (MPSC)      │  │  (MPSC)         │ │ │    │
│   │  │   └──────┬───────┘  └──────┬───────┘  └────────┬────────┘ │ │    │
│   │  │          │                  │                   │           │ │    │
│   │  │          └──────────────────┴───────────────────┘           │ │    │
│   │  │                            │                                │ │    │
│   │  │                            ▼                                │ │    │
│   │  │           Subscriber broadcaster (shared-serialise once,    │ │    │
│   │  │           push shared_ptr into per-subscriber send queues)  │ │    │
│   │  └───────────────────────────────────────────────────────────┘ │    │
│   └──────────────────────────────────────────────────────────────────┘ │
│                                                                        │
└───────┬──────────────────────────────────────────────┬─────────────────┘
        │                                              │
   gRPC :50061                              External services
   (Control + Media + SubscribeEvents)      (TTS, STT, bot, event subscribers)
```

There is no in-module Redis (or other broker) sink. Per the
transport ADR ([`transport-adr.md`](transport-adr.md)), gRPC
`SubscribeEvents` is the sole event transport; operators run HA
subscriber pairs to get Tier-1 no-loss.

## Plane breakdown

### Control plane — gRPC server

The module starts a gRPC server on a configurable port (default 50061).
It exposes `open_switch.control.v1.ControlService`. Every RPC takes a
`RequestHeader` carrying `request_id` (UUIDv7), `tenant_id`, and an
optional W3C `traceparent`.

Methods cover the same scope as ESL inbound commands plus high-level
helpers (Bridge, Hold, Transfer). The detailed contract is in
[`specs/control-api/spec.md`](../specs/control-api/spec.md).

Key behaviors:
- All write RPCs are idempotent: a duplicate `request_id` within the
  configured TTL returns the cached response.
- Per-tenant API key authentication (in addition to optional mTLS).
- Per-tenant rate limiting via token bucket.
- Per-tenant allowed dialplan contexts — Originate can only target
  contexts the tenant is allowed in.
- gRPC server-streaming `SubscribeEvents` is the sole event
  transport. Backed by per-tier in-memory rings (MPSC producer side,
  single broadcaster consumer); slow subscribers are dropped
  explicitly with `RESOURCE_EXHAUSTED`. Durability beyond the
  replay window is the subscriber's responsibility — see
  [`event-tiers.md`](event-tiers.md) "No-loss reference
  architecture".

Threading model:
- gRPC uses a thread pool managed by the gRPC library. Synchronous
  service handlers; long-running operations (`Originate` with a 60-s
  timeout) run on the gRPC thread.
- `switch_ivr_originate` itself may block; we accept this and size the
  gRPC thread pool to be at least 2 × the max concurrent Originate rate.
  Asynchronous Originate (returning a job_id, completion as event) is a
  future enhancement.

### Event plane — bind, route, ship

The module binds to FreeSWITCH events via `switch_event_bind`, using a
single callback for ALL events. The callback enqueues to an
internal **lock-free MPSC** (multi-producer, single-consumer) ring per
tier; each tier has a dedicated consumer thread that converts FS
events to `EventEnvelope` protobufs and ships to active gRPC
subscribers via the SubscribeEvents stream.

The producer side is MPSC, not SPSC, because FreeSWITCH services its
event dispatch queue with a pool of up to 64 threads
(FREESWITCH-FACTS FF-004 — `src/switch_event.c:82-95` and lines
367-389). The pool starts at one thread and grows on demand under
load. Our `switch_event_bind` callback may therefore be invoked
concurrently from multiple dispatch threads. The round-2 spec
called this an SPSC ring, which was unsound; the round-3 design is
MPSC.

```text
FS event_facility
       │
       ▼
[ event_bind callback ]──┐
       │                 │  Fast path: tier classify, serialize once
       │                 │  into a shared_ptr<const string>, push onto
       │                 │  per-tier ring + each subscriber's send queue.
       │                 │  Target return-to-FS latency ≤ 50 µs.
       ▼                 ▼
  Tier classifier (event_name + subclass match against routing rules)
       │
       │  seq allocated here (per-tier std::atomic<uint64_t>)
       │  envelope serialized to bytes once
       │
       ├──▶ Tier 1 ring (16384, FIFO evict on overflow)
       ├──▶ Tier 2 ring (8192)
       └──▶ Tier 3 ring (4096)
            │
            │  for each active gRPC SubscribeEvents stream:
            │  - apply subscriber's tier + event_name filters
            │  - push shared_ptr (16 B overhead) onto its bounded queue
            ▼
[ Per-subscriber send queues (4096 cap default) ]
            │
            │  gRPC sender thread per subscriber drains, writes to wire
            ▼
External gRPC subscriber (operator-owned)
            │
            │  Subscriber's choice for downstream durability:
            │  Kafka / Redis Streams / S3 / file / dashboard / drop.
            │  This is OPERATOR-SIDE persistence — the module itself
            │  does not write to any broker.
            ▼
Operator-defined durability policy
```

Why per-tier rings: each tier has different volume + replay-window
needs. Mixing tier 3 heartbeats with tier 1 billing into a single ring
means a heartbeat burst evicts a CHANNEL_ANSWER. Per-tier isolation
prevents that.

Ring sizing (defaults; configurable per `event_ring_capacity_tierN`):

- Tier 1: 16384 envelopes. ≈25 min replay @ 10 ev/s.
- Tier 2: 8192. ≈3 min @ 40 ev/s.
- Tier 3: 4096. ≈20s @ 200 ev/s.

Overflow policy is FIFO evict in all three tiers — the module does NOT
block FS event delivery. Durability is the subscriber's responsibility
(see [`event-tiers.md`](event-tiers.md) "No-loss reference architecture"
and [`transport-adr.md`](transport-adr.md) for the ADR rationale).

Subscriber broadcast uses ref-counted shared serialization: each event
is serialized to bytes once, and each subscriber's queue holds a
`std::shared_ptr<const std::string>`. Memory cost is `1 × payload +
N × pointer-size` regardless of subscriber count.

The FS `event_bind` callback runs in FreeSWITCH's event-emitting thread
context. It MUST return quickly (target ≤ 50 µs per event) or it will
back-pressure the entire FS process — including SIP signalling state
machinery handled by mod_sofia. The new design (per Phase 1 Codex
finding C-4) eliminates the network I/O that the previous Redis design
introduced on this thread.

### Media plane — multi-bug per call

When an external service wants to bridge audio with a FreeSWITCH channel,
the module exposes a gRPC client that opens a stream to
`open_switch.media.v1.MediaBridge.Stream` on the external service. The
module attaches one or more FreeSWITCH media bugs to the channel,
each tied to one direction or purpose:

- TTS playback (`SMBF_WRITE_REPLACE`): service sends audio frames,
  module writes them onto the channel's write side. The caller hears the
  bot.
- STT transcribe (`SMBF_READ_STREAM`): module reads the channel's
  read-side audio, ships frames to service. Service returns transcript
  messages.
- Voicebot duplex: combines both. Module opens ONE stream and uses two
  bugs internally.
- AMD detect (`SMBF_READ_STREAM`): module reads, service returns a
  human/machine verdict.
- Recording relay (`SMBF_READ_STREAM` + `SMBF_WRITE_STREAM`): module
  taps both sides, ships either mixed-mono or stereo split to service.

Multiple media bugs may coexist on a single channel. Their priority
order determines the audio chain. See
[`media-bridge.md`](media-bridge.md) and
[`recording-with-bot.md`](recording-with-bot.md) for details.

Lifecycle:
- Media bug add happens on `Originate.after_answer = AppSequence{StartBot}`
  or via a dedicated control RPC `StartMediaStream`.
- Media bug remove happens on channel hangup (via state handler) or on
  explicit stop RPC. The remove is invoked from the channel destroy
  state handler — the bug must be removed before the channel teardown
  completes.
- gRPC stream close: half-close from module side on graceful stop;
  full cancel on hard hangup. Service-side close triggers module to
  remove the bug and optionally emit a `bot_stream_lost` event.

### Cross-cutting infrastructure

#### RAII helpers (mandatory; see [`memory-management.md`](memory-management.md))

- `osw::SessionLock`: pairs `switch_core_session_locate` /
  `switch_core_session_rwunlock`.
- `osw::EventGuard`: pairs `switch_event_create` /
  `switch_event_destroy` if not fired.
- `osw::MediaBugLease`: pairs `switch_core_media_bug_add` /
  `switch_core_media_bug_remove`.
- `osw::XmlNode`: pairs `switch_xml_open_*` / `switch_xml_free`.

#### Config loader

XML config (FS-native `switch_xml_config_parse_module_settings`) loaded
at module load and on hot-reload SIGHUP. The config is validated; if
validation fails, the previous config remains active and a Tier-1 event
fires with the validation error.

#### Structured logging

Wraps `switch_log_printf` with a JSON-formatted message including:
`timestamp`, `level`, `subsystem`, `tenant_id` (when known),
`channel_uuid` (when applicable), `traceparent` (when applicable),
`message`, structured fields.

Log lines remain FreeSWITCH-routable (they go through FS log facility),
so operators continue to use FS log rotation. The JSON shape lets log
aggregators parse them cleanly.

#### Health counters

In-memory counters reported via the `Health` RPC:
- `active_channels`
- `active_media_bugs`
- `events_emitted_total` (per tier)
- `grpc_rpcs_total` (per method + status)
- `transport_send_failures_total` (per sink)
- `idempotency_cache_size`

These are NOT Prometheus metrics in V1 (Prometheus stack is out of scope
for V1 per project decisions). Operators wanting metrics export should
run a sidecar that polls `Health` and translates.

#### Graceful drain

On SIGTERM, the module:
1. Sets internal status to `DRAINING`. `Health` RPC returns `DRAINING`.
2. Refuses new Originate / SubscribeEvents calls (returns
   `UNAVAILABLE`). Existing SubscribeEvents streams continue.
3. Continues to service existing channels and media streams.
4. Waits up to `drain_timeout_seconds` (default 30) for active
   channels to drop to zero.
5. Forcibly closes remaining streams; calls
   `switch_core_session_hupall` for any remaining bot-attached
   channels.
6. Drains per-tier event rings: continues delivering buffered events
   to active SubscribeEvents subscribers for up to
   `event_drain_timeout_seconds` (default 5). Subscribers may close
   their end early to expedite. After the timeout, any remaining
   ring contents are dropped and a final
   `osw::audit::shutdown_with_pending_events` Tier 1 event is emitted
   (best-effort — may itself be dropped if no subscriber remains).
7. Shuts down gRPC server (`server_->Shutdown(grpc_drain_deadline)`).
   Default `grpc_drain_deadline` = 2s.
8. Closes any open media-upstream gRPC channels.
9. Returns from `mod_open_switch_shutdown`.

Phase 1 Codex finding I-13 (the prior draft punted on "XADD with WAIT?")
is resolved by deletion: post-F0 there is no Redis sink. The drain
ordering above is the complete graceful shutdown.

#### Idempotency cache

A bounded LRU cache keyed by `(tenant_id, request_id)` with TTL
`idempotency_ttl_seconds` (default 300). Stores the response
proto-serialized for replay. Capacity sized to 5 × expected RPS × TTL
(default 1500 entries).

## Process layout in a typical deployment

```text
┌────────── Host (Linux, Debian trixie) ──────────┐
│                                                  │
│  ┌─── Docker container ─────────────────────┐    │
│  │ FreeSWITCH 1.10.12                        │    │
│  │   + mod_open_switch.so loaded             │    │
│  │ Listens:                                  │    │
│  │   :5060/udp (sofia internal)              │    │
│  │   :5080/udp (sofia external)              │    │
│  │   :16384-32768/udp (RTP range)            │    │
│  │   :50061/tcp (mod_open_switch gRPC)       │    │
│  └─────────┬──────────────────────────────────┘   │
│            │                                       │
└────────────┼───────────────────────────────────────┘
             │ gRPC :50061
             │ (Control + Media + SubscribeEvents)
             │
   ──────────┴────────────────────────────────────────────────
                                                  ▲
                                                  │ SubscribeEvents
                                                  │ stream
   ┌──────────────┐                ┌──────────────┴──────────────┐
   │ Orchestrators │               │ Event subscribers (operator) │
   │ Bot logic     │               │  · HA subscriber pair for    │
   │ Voicebot      │               │    Tier-1 no-loss            │
   │ STT service   │               │  · Persist to Kafka/Redis    │
   │ TTS service   │               │    Streams/S3/file/dashboard │
   └──────────────┘                │    on the subscriber side    │
                                   └──────────────────────────────┘
```

The deployment contains no in-module broker. The container ships
FreeSWITCH + `mod_open_switch.so` + the FS-managed runtime
dependencies — no Redis, no Kafka, no NATS service is required for
the module to operate. Operators wanting durable event persistence
run their own subscribers (per the
[`transport-adr.md`](transport-adr.md) HA pattern) and choose their
own storage backend on that side.

For multi-FS deployments, every FS node runs its own `mod_open_switch`
instance. Event subscribers receive events from each node via
separate `SubscribeEvents` streams; subscribers can filter by the
`node_id` field in the envelope, or run one subscriber per node and
fan-in downstream. There is NO direct inter-module coordination;
FreeSWITCH's own clustering features (`mod_sla`, distributed
registrations, etc.) are used as before.

## Sequence: Inbound voicebot call

```text
SIP UA → INVITE → FS sofia → dialplan
                                │
                                │ <action application="set" data="tenant_id=acme"/>
                                │ <action application="set" data="bot_session_id=${uuid()}"/>
                                │ <action application="answer"/>
                                │ <action application="lua" data="start_open_switch_bot.lua"/>
                                ▼
                       lua: invoke gRPC StartMediaStream
                                │
                                ▼
                       mod_open_switch
                          │ register session in correlator
                          │ media_bug_add (write/replace)
                          │ media_bug_add (read/stream)
                          │ open gRPC stream to bot endpoint
                          ▼
                       Bot service
                          │ negotiates StreamReady
                          ▼
                       Bidirectional audio flows
                          │ (caller mic → STT → bot logic → TTS → caller ear)
                          │
                          ▼ (caller hangs up)
                       FS emits CHANNEL_HANGUP
                          │
                          ▼
                       mod_open_switch
                          │ state handler removes media bugs
                          │ half-closes gRPC stream
                          │ emits Tier-1 CHANNEL_HANGUP_COMPLETE event
                          │ emits Tier-1 BOT_CALL_COMPLETED custom event
                          ▼
                       Bot service receives stream end
```

## Sequence: Outbound campaign call with idempotent retry

```text
Orchestrator → gRPC Originate(request_id=abc, endpoints=[..], 
                              after_answer={apps=[StartBotMediaStream]})
                    │
                    ▼
             mod_open_switch
                    │ Idempotency cache hit on (tenant, abc)?
                    │   Yes → return cached response (no FS call)
                    │   No  → proceed
                    │
                    │ ACL check: tenant allowed dialplan context?
                    │ Rate limit: tenant under quota?
                    │
                    ▼
             switch_ivr_originate (blocks gRPC thread for up to 60s)
                    │
                    │ (FS dials, peer answers)
                    ▼
             Apply after_answer apps:
               execute "start_bot_media_stream" inline
                    │
                    ▼
             Module returns OriginateResponse{channel_uuid=X}
             (cache in idempotency LRU for 300s)
                    │
             ── Tier 2 CHANNEL_ANSWER event already emitted by FS ──
             ── Tier 1 CHANNEL_BRIDGE event emitted when bridged ──
             ── Tier 1 CHANNEL_HANGUP_COMPLETE on hangup ──
```

## Failure modes (architectural-level)

| Failure | Detection | Recovery |
|---|---|---|
| All subscribers offline | `subscriber_count==0`; ring fill grows | Events accumulate in per-tier ring up to capacity, then evict oldest. Module logs WARN every 30s with drop count. NO durability promise during this window — operator must run HA subscriber pair if no-loss is required (see [`event-tiers.md`](event-tiers.md) "No-loss reference architecture"). |
| Slow subscriber | Per-subscriber queue fills | Close that subscriber's gRPC stream with `RESOURCE_EXHAUSTED`. Increment `subscriber_kicked_total{cause="queue_full"}`. Subscriber reconnects (with `since_seq` if tracked). Fast subscribers unaffected. |
| Subscriber misses replay window | `since_seq` outside ring | Return `RESOURCE_EXHAUSTED` on subscribe. Subscriber retries without `since_seq` and accepts the gap. |
| gRPC port bind fails | `server_->BuildAndStart` returns null | Module load fails. FreeSWITCH startup fails (deliberate — operator must fix or unload). |
| FreeSWITCH crash | Process dies | Container orchestrator restarts. Module state is in-process; nothing to restore. Active calls are torn down by SIP-side BYE retransmits + subscriber disconnect; orchestrators must idempotently retry. |
| Module crash but FS survives | `std::set_terminate` handler (opt-in; chains the previous handler — see Codex round-3 N5 below); signal-safe abort on SIGSEGV/SIGABRT only if explicitly enabled | Cannot easily recover from a poisoned module state. If the operator enables `osw_panic_on_unhandled=true`, the module's handler logs and then calls `_exit()` after chaining. Otherwise the handler logs, chains the previous handler, and lets FS / earlier handlers decide. See "Terminate-handler chaining" below for the full contract. |
| Memory leak in module | LSAN in CI, Valgrind nightly | Block release on detection. Production: container restarts on OOM. |
| Slow external bot service | gRPC media-stream stalls | Per-stream timeout (configurable per purpose). On timeout: module sends silence frames to channel, emits Tier-2 `bot_stream_stalled`. Operator can configure abandon-call vs continue-silent. |
| Idempotency cache poisoned (wrong response cached) | None directly | Cache TTL is short (300s default); cache is per-process so a module reload clears it. |
| Eavesdrop bypass attempt | Audit event fires when attempt detected | If `policy=deny`, FS hangs up the eavesdropper. If `policy=audit`, event still fires for SIEM follow-up. See [`security-and-eavesdrop.md`](security-and-eavesdrop.md). |

### Terminate-handler chaining (Codex round-3 finding N5)

`std::set_terminate` replaces the **process-wide** C++ terminate
handler — a single per-process slot in libstdc++. The slot is
shared with every other module loaded into the FreeSWITCH process
(`mod_grpc`, `mod_signalwire`, and any other C++-using module). FS
modules are loaded with `RTLD_LOCAL` by default (FREESWITCH-FACTS
FF-010), but the terminate handler is not bound to a dlopen
namespace — it is a libstdc++ global. Module load order is operator-
controlled via `modules.conf.xml`; whoever calls `set_terminate`
last wins.

The implications:

- If our module installs a handler at load time and another C++
  module loads after us and installs its own, theirs wins; ours is
  replaced. The other module gets to decide the process's
  abort-on-unhandled behaviour.
- If we install last, theirs is replaced. We do not know what
  cleanup they wanted to run.
- On `unload mod_open_switch`, our handler function pointer
  becomes a dangling reference in libstdc++. Any subsequent
  unhandled C++ exception will jump into freed memory.

V1 contract:

1. The terminate-handler install is **opt-in**, controlled by
   `osw_panic_on_unhandled` in `<settings>` (default `false`).
   With the default, the module does NOT call `std::set_terminate`
   at all and FreeSWITCH's own crash-handling stays in charge. This
   is the safest interop posture with `mod_grpc` and similar.
2. When `osw_panic_on_unhandled=true`, on module load:
   - Capture the previous handler with the return value of
     `std::set_terminate(osw_terminate)`.
   - Store the previous handler in a module-static
     `std::atomic<std::terminate_handler> prev_terminate_`.
3. On termination, `osw_terminate`:
   - Logs via signal-safe `write(STDERR_FILENO, ...)` — NOT
     `switch_log_printf` (allocates) or `std::cerr` (allocates and
     locks).
   - Calls the captured previous handler if non-null.
   - If the previous handler returns (it shouldn't — terminate
     handlers are supposed to not-return — but defensively): calls
     `_exit(STATUS_OSW_TERMINATE)` to trigger a clean container
     restart.
4. On module unload (Phase 2 — V1 stub does not call
   `std::set_terminate` at all):
   - Restore the previous handler:
     `std::set_terminate(prev_terminate_.load())`.
   - Race with other modules that also `set_terminate` between our
     install and unload is unavoidable; the documentation calls
     this out so operators know to unload modules in reverse-load
     order if possible.

For hard signals (SIGSEGV, SIGABRT, SIGBUS), the same logic
applies: signal handler installation is opt-in via a separate
`osw_install_signal_handlers=false` (default false). FS installs
its own handlers; chaining a second handler in a SIGSEGV context
is unsafe (heap may be corrupted; calling printf may itself
crash). Default off so FS's handlers run alone.

Operator guidance: enable `osw_panic_on_unhandled` only if you
know no other module installs a terminate handler that needs to
run for cleanup. The safer default is to let unhandled exceptions
land in FS's standard abort path and trigger the container
orchestrator's restart policy.

## What this design explicitly does NOT do

1. **Take SIP/RTP responsibility from FreeSWITCH**. We use FS for all
   media-protocol work. No SIP UA / RTP stack inside the module.
2. **Persist state**. The module is stateless across restarts.
   Idempotency cache, event rings, dialog state — all in-memory.
   Durable state lives wherever the subscriber persists it (Kafka,
   Redis Streams, S3, file, on the subscriber's side — operator's
   choice) and in FreeSWITCH's own stores (CDR backends, recording
   files, etc.). This module is stateless across restarts;
   reload-mid-call leaves the FS channel in place but loses the
   in-memory caches (see Idempotency §"Module-restart false-negative
   gap" in [`specs/control-api/spec.md`](../specs/control-api/spec.md)).
3. **Coordinate across nodes**. Multi-FS coordination uses FS's own
   features (registrations, mod_sla, etc.). The module is per-node.
4. **Replace the FS dialplan**. Dialplan stays the entry point for calls.
   The module is invoked from dialplan actions and from external gRPC.
5. **Deprecate `mod_event_socket`**. Both can be loaded; operator
   chooses which to use for each integration.

## Component map → source layout

| Plane | Subsystem | Source dir |
|---|---|---|
| All | Module lifecycle, config, RAII helpers | `src/core/` |
| Control | gRPC server, handlers, idempotency, ACL | `src/control/` |
| Event | event_bind, tier router, proto convert, MPSC rings | `src/events/` |
| Event | gRPC SubscribeEvents broadcaster + per-subscriber queues | `src/events/subscribe/` |
| Media | Bug manager, bidi stream client, resampler | `src/media/` |
| Security | Eavesdrop guard, TLS setup, ACL evaluator | `src/security/` |
| Observability | Logger, health counters | `src/observability/` |

Each subsystem has a corresponding `tests/unit/<subsystem>_test.cc` and
where applicable a `tests/integration/<subsystem>_it.cc`.
