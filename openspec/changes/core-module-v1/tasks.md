# Core module V1 — implementation tasks

Placeholder. Filled in after Phase 1 specs (the `designs/` + `specs/`
documents in this change) are approved and the implementation sub-agent
is spawned.

Sequencing will roughly follow:

1. `MOD.CORE.001` — module entry, load/unload, config parser
2. `MOD.MEM.001` — RAII helpers (SessionLock, EventGuard, MediaBugLease)
3. `MOD.GRPC.001` — gRPC server boot, TLS, API-key auth
4. `MOD.CTRL.001` — Originate / Hangup / Bridge / Execute / SetVariables
5. `MOD.EVT.001` — event bind, tier router, header allowlist
6. `MOD.TRANS.001` — Redis Streams sink + Redis Pub/Sub sink
7. `MOD.MEDIA.001` — media bug manager, bidi stream client, resampler
8. `MOD.SEC.001` — eavesdrop policy, audit event emission
9. `MOD.LIFE.001` — graceful drain, hot reload
10. `MOD.OBS.001` — Health RPC, in-memory counters, structured logging

Each task carries: files, spec, acceptance, code-review checklist
(memory-management section non-optional).

Each task ends with a Codex checkpoint reading the diff + the relevant
spec — same model as the W4.5 / W5 waves on the open-tts repo.
