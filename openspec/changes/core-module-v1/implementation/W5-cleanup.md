# W5 — Production gate cleanup

**Owner**: Opus 4.7 (orchestrator). Implementation: Sonnet sub-agents per track.
**Branch convention**: `implementation/wave5-{track}` off `main` after PR #10
(W4 strip) lands. Worktrees under `.claude/worktrees/wave5-*`.
**Status**: Plan.

## Why

W4 shipped TLS (opt-in) + Prometheus metrics **collector**. Two production-gate
items the previous wave landed but did not finish wiring, and which now block
running mod_open_switch as a real internal service:

1. **MetricsServer HTTP endpoint is unwired**. `src/observability/metrics_server.cc`
   compiles and is linked into the module, but the module load path
   (`src/core/module.cc`) never constructs nor starts it. A Prometheus scraper
   pointed at `${metrics_bind_address}:${metrics_port}` will get connection
   refused even though `metrics_enabled=true` in the config.

2. **Idempotency cache is config-only.** `Config::idempotency_ttl_seconds` /
   `idempotency_cache_capacity` / `idempotency_in_flight_max_wait_seconds` are
   parsed from XML and exposed on the struct, but no cache object exists, and
   the W3 handlers (Originate in particular) do not check or populate one. A
   client that retries Originate during a network glitch will dial the
   destination **twice**.

## Out of scope (deferred)

- **Tenant ACL enforcement** (`Config::tenant_allowed_contexts`). The original
  design tied tenant identity to mTLS CN / API key; after W4 strip there is
  no authenticated identity to key the ACL against. A client-claimed
  `tenant_id` request header could enforce a "soft" ACL, but that only
  protects against bugs in trusted orchestrators, not malicious peers, and
  the deployment model (internal trusted clients) already assumes no
  malicious peers. Defer until either (a) auth comes back, or (b) operator
  asks for safety-net tenancy.

- **Eavesdrop policy** (`MOD.SEC.001`). The policy gates supervisor
  eavesdropping on bot-participating calls — which presupposes a media
  plane to attach bots to. With media plane not yet implemented, the policy
  has no calls to gate. Defer until the W6+ media plane wave; include
  eavesdrop as Track D of whichever wave first delivers `media_bug` plumbing.

## Tracks

### Track A — Wire MetricsServer into module load

**Files touched**:
- `src/core/module.cc`
- `include/osw/core/module.h` (if a Module class member is added)
- `src/core/CMakeLists.txt` (link osw_observability if not already)
- `tests/integration/metrics_http_test.cc` (new, optional)

**Implementation**:
1. In `Module::Load` (or equivalent module-load entry), gate on
   `config_.metrics_enabled`. When true, construct
   `osw::observability::MetricsServer` with the prometheus::Registry obtained
   from the existing Health-metrics adapter, bind on
   `config_.metrics_bind_address:config_.metrics_port`, and `Start()`.
2. Hold the server as a `std::unique_ptr<MetricsServer>` member; in
   `Module::Shutdown`, call `Stop()` before tearing down the prometheus
   registry.
3. On bind failure (port already in use), log `Error` and continue module
   load — metrics are observability, not safety; load should not fail.
4. When `metrics_enabled=false`, do not construct the server. Log `Info` once
   noting metrics are disabled.

**Acceptance**:
- `curl http://${metrics_bind_address}:${metrics_port}/metrics` returns
  Prometheus exposition format with at least the Health counters and (if
  W4C RpcMetrics is registered) `osw_rpc_calls_total` / `osw_rpc_latency_seconds`.
- Module load with `metrics_enabled=false` does not bind any port; no
  HTTP server is started.
- Module shutdown cleanly stops the HTTP server (no port-in-use error on
  immediate reload).

**Tests**:
- Unit test exists for `MetricsServer` itself — keep as-is.
- New: integration test that loads the module against a temp config with
  `metrics_bind_address=127.0.0.1`, `metrics_port=0` (kernel-assigned), and
  asserts a successful `GET /metrics` after load.

### Track B — Idempotency cache for Originate (and Bridge/Execute)

**Files added**:
- `include/osw/control/idempotency_cache.h`
- `src/control/idempotency_cache.cc`
- `tests/unit/control/idempotency_cache_test.cc`

**Files modified**:
- `src/control/handlers/originate_handler.cc` — check + populate cache
- `src/control/handlers/bridge_handler.cc` — same pattern
- `src/control/handlers/execute_handler.cc` — same pattern
- `src/control/CMakeLists.txt` — link new TU
- `src/core/module.cc` — construct cache, inject into handlers via
  `ControlServiceSkeleton::SetIdempotencyCache` (new method)

**API sketch**:
```cpp
namespace osw::control {

class IdempotencyCache {
  public:
    struct Entry {
        grpc::Status status;
        std::string serialized_response;  // proto-serialized
        std::chrono::steady_clock::time_point expires_at;
    };

    IdempotencyCache(std::size_t capacity,
                     std::chrono::seconds ttl,
                     std::chrono::seconds in_flight_max_wait);

    /// Look up by request_id. Returns:
    ///   - kHit + cached response (replay safely)
    ///   - kInFlight + future-like wait token (block up to in_flight_max_wait)
    ///   - kMiss (caller should execute and call Store)
    enum class State { kMiss, kHit, kInFlight };
    struct LookupResult {
        State state;
        Entry entry;  // populated when state == kHit
    };
    LookupResult LookupOrReserve(const std::string& request_id);

    /// Caller stores the result. Wakes any in-flight waiters.
    void Store(const std::string& request_id, Entry entry);

    /// Caller failed to compute — release the in-flight reservation so other
    /// waiters can retry. Used in catch (...) blocks.
    void Cancel(const std::string& request_id);
};

}  // namespace osw::control
```

**Implementation notes**:
- LRU eviction at capacity. `std::unordered_map<std::string, ListIterator>` +
  `std::list<KeyEntryPair>` is the canonical pattern.
- TTL check on every `LookupOrReserve` (lazy expiry). Background sweeper
  thread is overkill at capacity=1500.
- In-flight reservation uses a condvar per key (or a single condvar +
  predicate map). Don't hold the cache mutex while waiting.
- Empty `request_id` → bypass cache (caller's choice not to dedupe).
- Reserved entries that exceed `in_flight_max_wait` return `kMiss` to the
  waiter; the original executor still owns the slot. This prevents
  permanent stalls when an executor wedges.

**Acceptance**:
- `OriginateRequest` with the same `request_id` issued twice within TTL
  returns the cached response on the second call without dialing again.
  Verified by counting `switch_ivr_originate` calls in the FS mock.
- Concurrent identical Originate requests serialise: the second one waits,
  then returns the first's response (kHit on retry path).
- Cache full → oldest entry evicted, no leak.
- Empty `request_id` → cache untouched.

**Tests**:
- Unit: LookupOrReserve / Store / Cancel; LRU eviction; TTL expiry;
  in-flight wait timeout; concurrent stress (TSAN gate).
- Handler integration: Originate twice with same request_id → mock
  `switch_ivr_originate` called once.

## Wave-level gates

After both tracks merge:

- `make protos.lint && make build && make test` clean (ASAN amd64 + arm64
  + TSAN).
- Manual smoke: docker compose up `osw-fs`, run Go client, verify
  `curl :9090/metrics` returns counters, run Originate twice with same
  `request_id` and confirm only one dial.
- Codex CLI wave review (no Claude sub-agents per HARDENED memory).
- W5 closeout doc in `openspec/changes/core-module-v1/implementation/`
  with findings + closed-loop SHAs.

## Sequencing

1. PR #10 (W4 strip) + PR #11 (W3 fix) merge to main (waiting on user).
2. Branch `implementation/wave5-track-a-metrics-http` off main.
   Sonnet sub-agent implements Track A.
3. Branch `implementation/wave5-track-b-idempotency` off main (parallel
   with A; no overlap in touched files).
   Sonnet sub-agent implements Track B.
4. Orchestrator merges both tracks into `implementation/wave5-cleanup`,
   resolves any module.cc conflicts (both tracks touch it).
5. PR `implementation/wave5-cleanup` → main.
6. Codex CLI wave review.
7. Fix sprint if findings (W5.5).
8. Merge; mark V1 production-gate complete.
