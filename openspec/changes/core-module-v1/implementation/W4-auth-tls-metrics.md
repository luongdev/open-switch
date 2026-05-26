# W4 — Auth + TLS + Metrics (production-deploy gate)

**Status.** Planned 2026-05-26. To start once W3 (Control plane) merges
clean. Selected over the alternative "W4 = Media plane" because the W3
control-plane RPCs (Originate / Hangup / Bridge / Execute / etc.) ship
fully **unauthenticated** in W3 — anyone with network access to the
gRPC port can dial calls, hangup live channels, transfer to arbitrary
destinations. That's not deployable to a non-air-gapped environment.
Media plane (recording/playback) is genuinely useful but does not
create a security blocker; it defers cleanly to V1.5 / V2.

W4 closes three gaps that together make the module production-deployable:

| Gap                          | Track | Why now                       |
|------------------------------|-------|-------------------------------|
| gRPC server uses plaintext   | A     | TLS / mTLS required for prod  |
| RPCs accept any caller       | B     | per-RPC authz, identity, RBAC |
| Operators have no metrics    | C     | Prometheus endpoint + Health  |

Three parallel tracks. Track A (TLS) is independent — can land first
or last. Tracks B + C consume A's identity context (auth-decision
metrics, mTLS-CN extraction).

---

### In scope

- **Track A (TLS / mTLS)**: replace the W1 stub in `src/control/tls.cc`
  with real cert loading from a configured key/cert pair. Server-side
  TLS is always-on; mTLS (client-cert required) is configurable but
  defaults ON in production builds (`OSW_TLS_REQUIRE_CLIENT_CERT=ON`).
  Cert hot-reload on SIGHUP (no module restart needed).
- **Track B (Auth + RBAC)**: gRPC server-interceptor that runs on
  every RPC. Extracts identity from either (a) mTLS client cert CN or
  (b) a Bearer token (JWT, signed with the cluster's signing key).
  Identity → RBAC role lookup (configured in
  `open_switch.conf.xml`). Per-RPC required permission set
  enforced. Deny path returns `PERMISSION_DENIED` with the missing
  permission name in the gRPC status detail. Audit emit on every
  decision: `osw.control.authz.<allow|deny>` with `{identity, rpc,
  permissions_required, permissions_granted}`.
- **Track C (Metrics)**: separate HTTP server on `:9090/metrics`
  (configurable) exposing Prometheus-format metrics. Counters: per-RPC
  call counts, per-RPC errors (split by gRPC status code), authz
  allow/deny by RPC, tier-1/2/3 ring depths, drops, subscriber count,
  audit emit count. Histograms: per-RPC latency (Prometheus buckets).
- Configuration: extend `open_switch.conf.xml` with `<tls>`, `<auth>`,
  `<metrics>` blocks. Parser additions in `src/core/config.cc`.
- Tests: unit tests against fakes (no real TLS handshake in unit
  tests); integration tests in W5 cover the end-to-end matrix.
- FF entries: FF-028 (gRPC ServerCredentials TLS), FF-029 (gRPC
  ServerInterceptor lifecycle), FF-030 (FS SIGHUP behaviour for our
  cert reloader).

### Out of scope

- **Authorization beyond RBAC**: no attribute-based access control
  (ABAC), no row-level filtering of SubscribeEvents (that's V2 if
  there's demand — currently subscribers see all events that match
  their `event_names/node_id/subclass_globs` filter).
- **Cert rotation via Vault / Kubernetes secrets**: cert reload from
  the filesystem on SIGHUP is the V1 mechanism. Operators wire their
  rotation tool to SIGHUP. V2 may add native Vault/secret-manager
  integration.
- **JWT key rotation across multiple signing keys**: V1 expects ONE
  signing key per cluster (the config file points at it). V2 adds key
  set rotation (kid-based selection).
- **Distributed tracing (OpenTelemetry)**: V2. The audit channel
  already carries `traceparent` on every event; W4 doesn't expand
  this.
- **Real TLS handshake fuzzing**: that's an explicit security review,
  not a wave deliverable.

---

## FF entries to add

| FF-ID  | Symbol / topic                                       | Track |
|--------|------------------------------------------------------|-------|
| FF-028 | `grpc::SslServerCredentials` / `grpc::ServerCredentialsOptions` | A     |
| FF-029 | `grpc::ServerInterceptorFactoryInterface` lifecycle  | B     |
| FF-030 | FS module SIGHUP delivery + our cert reloader contract | A     |

Each FF cited from `/usr/local/include/grpc++/*` or `/usr/local/include/switch_*.h`.

---

## Track split

### Track A — TLS / mTLS

**Owner.** Sonnet sub-agent.
**Branch.** `implementation/wave4-track-a-tls`.
**Output.**

- Replace `src/control/tls.cc` stub with real cert loading +
  `grpc::SslServerCredentials` builder.
- Add cert hot-reload helper at `src/control/tls_reloader.cc` —
  watches the cert/key paths via `inotify` (linux) and triggers
  rebuild of the grpc server credentials when files change.
- Extend `src/core/config.cc` `<tls>` block parser.
- Unit tests for cert loading happy + failure paths (cert file
  missing, malformed PEM, key/cert mismatch).
- FF-028 + FF-030 entries.

Lands first because it's the most independent.

### Track B — Auth + RBAC

**Owner.** Sonnet sub-agent.
**Branch.** `implementation/wave4-track-b-auth`. Off Track A's branch
(so B can consume mTLS-CN identity).
**Output.**

- `include/osw/control/auth_interceptor.h` + `.cc` — gRPC
  ServerInterceptor that runs on every RPC. Identity extraction:
  prefers mTLS CN, falls back to `authorization: Bearer <jwt>` header.
- `include/osw/control/rbac.h` + `.cc` — role → permissions map +
  per-RPC required-permission table. Static at boot; reload on SIGHUP.
- Extend `src/core/config.cc` `<auth>` block parser.
- Hook the interceptor into the gRPC server builder in
  `src/control/server.cc`.
- Audit emit on every authz decision; reuse the W2 `osw::audit::Emit`
  helper.
- Unit tests for: each permission combination, expired JWT, missing
  bearer header, CN-mismatch under mTLS, anonymous (no creds) under
  `require=false` and `require=true`.
- FF-029 entry.

### Track C — Metrics

**Owner.** Sonnet sub-agent.
**Branch.** `implementation/wave4-track-c-metrics`. Off Track B (so
metrics include the authz allow/deny counters cleanly).
**Output.**

- `include/osw/observability/metrics_server.h` + `.cc` — a tiny HTTP
  server (use the standard library or a small dependency-free
  framework; gRPC's internal HTTP server is intentionally not used
  to keep the metrics surface independent of the gRPC server's state).
- Prometheus exposition format. Reuse `osw::Health` counters; add
  per-RPC histograms in a new `osw::control::RpcMetrics` collector.
- Extend `src/core/config.cc` `<metrics>` block parser (enabled,
  listen address, port).
- Hook the metrics server into module Load / Shutdown lifecycle.
- Unit tests for the Prometheus format (no real HTTP fetch — call
  the format-rendering function directly and assert the output).

---

## Track order

```
A (TLS) ─→ B (Auth) ─→ C (Metrics)
```

Sequential because B uses A's identity context and C wants B's authz
counters. The trade-off vs full parallelism is small (the W4 wave is
narrower than W3) and the sequential ordering avoids any churn from
the merge of A's changes to `tls.cc` mid-B-flight.

If Track A turns out to be smaller than expected, the orchestrator
can spawn Track B and C in parallel after A lands.

---

## Verification gates

Same matrix as W1 / W2 / W3 (Build amd64+arm64 + ASAN unit tests +
TSAN race check + clang-format + clang-tidy + buf + markdownlint).

Plus:
- **TLS smoke test**: a test that spins up the gRPC server with TLS
  enabled, hands the client an in-memory cert, runs Health → expects
  OK. Failure path: client cert from a different CA → UNAVAILABLE.
- **Auth smoke test**: each permission combination, exercised via the
  interceptor without the FS module running (the interceptor is a
  pure gRPC component that takes an identity provider + RBAC table).
- **Metrics smoke test**: spin up the metrics server, fetch /metrics,
  assert the Prometheus format parses, asserts known counters appear.

---

## Definition of done

- TLS on every gRPC connection in production builds. mTLS required by
  default; can be turned off via config for dev environments.
- Every W3 RPC enforces an authz decision; deny path returns
  `PERMISSION_DENIED`. Anonymous RPCs return `UNAUTHENTICATED`.
- Audit log carries every authz decision.
- `/metrics` endpoint serves Prometheus format with at least the
  enumerated counters and histograms.
- All commits pass the W4 verification gate.
- Codex / Gemini wave-level review at close.

---

## Open questions for @luongdev (decide before implementation starts)

1. **JWT vs mTLS-CN identity priority** — W4 supports both. Default
   precedence: mTLS-CN preferred; JWT used when mTLS is disabled or
   client cert absent. Confirm or invert.
2. **Default deny vs default allow when no auth is configured** —
   draft assumes default-deny (UNAUTHENTICATED) so a misconfigured
   server is safe. Confirm.
3. **Metrics listen address** — default `127.0.0.1:9090` (loopback
   only). Operators expose via reverse proxy. Confirm or change to
   `0.0.0.0:9090`.
4. **JWT signing algorithm** — recommend ES256 (asymmetric, secret
   doesn't leave the signer). Alternative: HS256 (shared secret;
   simpler but every node needs the secret). Default to ES256?
5. **SIGHUP semantics** — does the FS module receive SIGHUP from FS
   itself, or do we need a separate signal handler? FF-030 will
   document; the answer affects whether the cert reloader uses
   `signal()` or `inotify` watching.
