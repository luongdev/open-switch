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

## V1 scope confirmed (W6.5 architectural decision)

After W6 wave-level review, the V1 ship gate is locked at the
following call topology:

- **Maximum 1 bot + 1 KH + 1 Agent per call** (3 participants).
- **Bot uses module-owned multi-target bug attachment**
  (`StartBot` RPC, W7 Track D). One bot logical entity attaches
  one `SMBF_READ_STREAM` + one `SMBF_WRITE_REPLACE` bug per target
  channel, multiplexes reads into one gRPC stream, and fans-out
  the gRPC reader's PCM into per-target SPSC queues.
- **No `mod_conference`, no phantom-session origination, no
  `switch_ivr_eavesdrop_session`**. The previous round of design
  exploration considered these; all three were ruled out for
  reasons documented in `designs/media-bridge.md` (conference =
  latency + RTP plumbing cost) and `designs/security-and-eavesdrop.md`
  (eavesdrop = FF-002 thread-id gate + FF-003 static `eavesdrop_callback`
  symbol). Multi-target bug attachment owned by `mod_open_switch`
  bypasses all three.
- **Bot passthrough when silent**: WRITE_REPLACE callback returns
  `SWITCH_TRUE` without calling `set_write_replace_frame()`; FS
  flows the channel's normal write source through unchanged (FF-036).

## V2 deferred

The following scope is explicitly out of V1:

- N ≥ 2 bots in the same call. (2 WRITE_REPLACE bugs on the same
  target would race in FF-001's single interleaved loop without a
  module-side mix arbiter.)
- Conference-based multi-party recording with ≥ 4 participants.
- Bot-as-SIP-leg pattern (originate the bot as a real SIP leg + use
  native FS stereo recording). Operators can use this pattern
  externally; the module does not add code for it.
- `UpdateBotTargets` RPC (dynamic target add/remove mid-call). V1
  requires `StopBot` + `StartBot` with new target list.

## Architectural decision log

| Date | Decision | Rationale |
|---|---|---|
| W1 | Spec-first via `openspec` | Phase-1 codex review found round-1 spec ambiguities; spec-first avoids spec drift |
| W2 | Tier classifier + per-tier MPSC rings (events) | Prevent slow consumer from blocking dispatch threads |
| W3 | gRPC interceptor for tenant ACL + idempotency | Surface for W4 auth + W5 idempotency without leaking into handler code |
| W4 | Strip JWT/RBAC; keep TLS + audit + metrics | OAuth2/RBAC = V1.5 (per primary memory in MEMORY.md) |
| W5 | IdempotencyCache LRU + TTL + in-flight condvar | Standard "exactly-once" guard for control RPCs |
| W6 | MediaBugManager with stage-rank gating | Multi-bug calls have well-defined order without numeric priority |
| **W6.5** | **Multi-target bug attachment over conference / phantom** | **V1 max 3-way; eavesdrop API unimplementable on vanilla FS v1.10.12 (FF-002+003); conference latency cost too high** |
| W7 | Eavesdrop = Layer 1 app + Layer 2 detector | FF-002 thread-id gate + FF-003 static symbol make removal unimplementable; detection-only via MEDIA_BUG_START |
| W7 | Recording = `RECORDING_RELAY` module-owned (W7 Track B) + FS-native `record_session` opt-out | Operator dialplan ordering rule documented in `recording-with-bot.md`; module-owned variant enforces stage rank |
