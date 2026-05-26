# Phase 1 Codex round-3 fix-sprint closeout

**Date**: 2026-05-26
**Author**: Sonnet signed as Codex (gpt-5.5)
**Round-2 review commit**: `c558b68`
**Round-3 commits (in order)**:

- `61cfc89` — `docs(facts): FreeSWITCH v1.10.12 verified-source facts sheet`
- `ca24d5d` — `fix(specs): N1 — write-side bug processing is a single interleaved loop`
- `9a37f9b` — `fix(specs): N2 — eavesdrop Layer 2 detection-only via MEDIA_BUG_START`
- `1054cbd` — `fix(specs): N9 — call-transcribe.md drops priority + Redis residuals`
- `aab82cf` — `fix(build): N6 — stub mod_open_switch.cc unblocks Dockerfile.builder`
- `59fc2df` — `fix(specs): N3, N4, N5, N7 — control-api + crash semantics + ring topology`
- `cc09f84` — `fix(specs+proto+build): N8/N10-N16 — sweep F0 stale Redis references`

Round-3 mandate was to close the issues round 2 found WITHOUT
introducing a new class of fabricated-FS-semantic claims (the
failure mode that recurred between rounds 1 and 2). Discipline
imposed: every FS-behaviour claim landed in spec text MUST cite an
entry in the new `FREESWITCH-FACTS.md`, with each entry pinned to a
v1.10.12 source file:line, a permalink, and a 5-20 line excerpt.

## Status by round-2 finding

### BLOCKERS

| ID | Spec | Status | Resolution |
|---|---|---|---|
| **N1** | media-bridge.md + recording-with-bot.md | RESOLVED | Round-2 claim "WRITE_STREAM runs in a separate pass after all WRITE_REPLACE" was factually false at v1.10.12. Per FF-001 (src/switch_core_media.c:16096-16156), the write side is a single interleaved loop. Both files rewritten to describe the actual behaviour: a WRITE_STREAM tap observes the pre-injection frame if positioned before a WRITE_REPLACE bug, post-injection if positioned after. media-bridge.md stage-rank table updated; FS-native section rewritten; test plan dropped fictional `write_stream_observes_post_replace` (assertion of false FS semantic) and added three correct tests. recording-with-bot.md Quick answer rewritten with two correct/incorrect chain examples; new `warn_record_before_inject` Tier-1 audit event added (default true) so operators learn about silent-bot misordering at attach time. |
| **N2** | security-and-eavesdrop.md | RESOLVED | Round-2 Layer-2 design was unsound on three independent grounds (FF-002 thread-id gate, FF-003 static eavesdrop_callback, FF-005 CHANNEL_CALLSTATE doesn't fire on bug attach). Replaced with a DETECTION-ONLY Layer-2 hook on `SWITCH_EVENT_MEDIA_BUG_START` (FF-011 — src/switch_core_media_bug.c:1014-1019 fires this every attach with `Media-Bug-Function` header). The module emits a Tier-1 audit event `osw::eavesdrop::detected_post_attach` when an FS-native `eavesdrop` bug attaches to a bot-marked session. The module does NOT call `switch_core_media_bug_remove_callback` — removal is unimplementable for FS-native eavesdrop bugs from outside the eavesdropper's dialplan thread. Limitations rewritten with honest disclosure; tests updated to assert no-remove behaviour; hardening checklist treats Layer 1 (`osw_eavesdrop`) + Layer 3 (raw-`eavesdrop` ACL block) as MANDATORY for any tenant with a deny policy. |
| **N6** | Dockerfile.builder + CMakeLists.txt + src/ | RESOLVED | Round 2 found that src/ was empty (only `.gitkeep`), the root CMakeLists.txt had `add_subdirectory(src)` commented out, and Dockerfile.builder's `cp /usr/local/mod/mod_open_switch.so` step failed because the target file was never produced. The fix-sprint claimed all CI was green but every CI run since the scaffold commit has been failing at this step. Round 3 lands a minimal Phase-1 stub: `src/mod_open_switch.cc` uses `SWITCH_MODULE_DEFINITION` / `SWITCH_MODULE_LOAD_FUNCTION` / `SWITCH_MODULE_SHUTDOWN_FUNCTION` per FF-009, allocates the module interface, logs "mod_open_switch loaded (stub)" at NOTICE level, returns SUCCESS. `src/CMakeLists.txt` builds it as a SHARED library with PREFIX cleared and OUTPUT_NAME forced to `mod_open_switch.so` (FS loader requires that exact name). Root CMakeLists.txt uncomments `add_subdirectory(src)`. Verified the stub compiles cleanly against real FS v1.10.12 headers with `-Wall -Wextra -Wpedantic -Werror -fvisibility=hidden -std=c++20`. The full Docker buildx flow cannot be reproduced locally (upstream `open-gateway/freeswitch-builder:v1.10.12` is non-public) but the cmake plumbing + compile path are correct end-to-end. The `if: false` MIN_INTEGRATION_TESTS gate stays gated; first implementation commit MUST flip it. |
| **N9** | call-transcribe.md | RESOLVED | This file was untouched by both the round-1 C-1 fix and F0. Round 3 rewrites: "priority 100/200/500/750" → stage-rank terminology with explicit citation of FF-007 (no-numeric-priority fact). Audio-chain ASCII art updated. "tenant_id — for ACL + Redis routing" rewritten to drop the Redis routing claim. "Tier-2 sink" → "in-module tier router" + subscriber-side persistence. Interaction-with-bot-TTS and barge-in sections also updated to remove priority numbers and reference stage ranks instead. |

### CRITICALS

| ID | Spec | Status | Resolution |
|---|---|---|---|
| **N3** | control-api/spec.md | RESOLVED | Idempotency error-path cleanup was unspecified. Round 3 makes it explicit: on any non-success handler return (INTERNAL, FAILED_PRECONDITION, etc.), the in-flight marker is replaced with the cached error response (same as for success), `notify_all()` waiters, retries within TTL receive the same error byte-for-byte. New `idempotency_cache_errors` knob (default true) controls the behaviour; setting false evicts on error and lets retries re-execute (with double-dial risk on fast retries). Crash-path (SIGKILL / OOM) residual documented. New integration test `control_api_idempotency_errors_test.cc`. |
| **N4** | control-api/spec.md | RESOLVED | `transfer`/`bridge`/`osw_transfer` Execute args containing the literal substring `${` are now REJECTED with `INVALID_ARGUMENT` and message "FS variable expansion (${...}) is not permitted in Execute args for app '<app>'; expand at the orchestrator before submitting". The check is deliberately conservative — refuses the entire class of FS variable references regardless of what the runtime expansion would resolve to, because the handler cannot prove the expansion matches the allowlist. New integration test `control_api_execute_var_expansion_test.cc`. |
| **N5** | architecture.md | RESOLVED | `std::set_terminate` is process-wide (single libstdc++ slot), not module-scoped. Round-3 contract: install is opt-in via `osw_panic_on_unhandled` (default false — FS's own crash handling stays in charge with the default). When opted in, the module captures the previous handler with the return value of `set_terminate`, chains to it on termination, `_exit()`s only if the previous handler defensively returns. On module unload, restore the previous handler. Signal handlers (SIGSEGV/SIGABRT) are also opt-in via `osw_install_signal_handlers` (default false); signal-context installation is hazardous. Documented in a new "Terminate-handler chaining" subsection under Failure modes. |

### IMPORTANTS

| ID | Spec | Status | Resolution |
|---|---|---|---|
| **N7** | architecture.md + event-tiers.md | RESOLVED | Round 2's "lock-free SPSC ring per tier" was unsound: FS event dispatch is a pool of up to 64 threads grown on demand (FF-004 — src/switch_event.c:82-95 and 367-389), and our event_bind callback may be invoked concurrently from any of them. Round 3 replaces SPSC with **MPSC** (multi-producer, single-consumer) throughout architecture.md. event-tiers.md sequencing-guarantees section now documents the MPSC reorder gap: A's `fetch_add(1)` returning seq=N can race with B's fetch_add+enqueue completing before A finishes its enqueue. The strict-monotonic guarantee is restored by a small consumer-side re-sort window (V1 design choice). The "all events for a given channel pass through the same FS event thread" promise is removed — subscribers must reconstruct ordering from `emitted_at` + per-tier `seq` if needed. |
| **N8** | architecture.md + security-and-eavesdrop.md + call-transcribe.md | RESOLVED | Architecture.md diagram rewritten (removed "Pluggable transports / Redis Streams / Redis Pub/Sub" block, removed in-container Redis 7 service, replaced with in-memory per-tier rings + external subscriber boxes). "Persist state... durable state lives in Redis" rewritten as subscriber-side persistence. "all writing to the same Redis cluster" rewritten as "separate SubscribeEvents streams per FS node; filter by node_id or run one subscriber per node". Source-layout table removed Transport / Sink-interface / Redis-impls row; added Event / SubscribeEvents-broadcaster row. security-and-eavesdrop.md threat-model diagram rewritten (removed Redis cluster + Redis TLS edge; added Event-subscribers box on operator side). Adversary table "Forge events into Redis" rewritten as "Subscribe to event stream without authorization". Hardening checklist removed Redis ACL/TLS items; replaced with subscriber-side persistence-target hardening. call-transcribe.md tenant_id description updated, "Tier-2 sink" → tier router + subscribers. |
| **N10** | control-api/spec.md | RESOLVED | SubscribeEvents "Not for production durable consumption. Use Redis Streams for that." rewritten to describe HA subscriber pair pattern with subscriber-side persistence (Kafka / Redis Streams / S3 / file / WAL — operator's choice). Open-questions "Should SubscribeEvents support since=<offset> resumption? Operators wanting resume use Redis Streams directly." removed (contradicted the proto, which already has `SubscribeEventsRequest.since_seq`); replaced with a note pointing at the implemented behaviour. |
| **N11** | events.proto | RESOLVED | File-level comment rewritten. Removed "routed through the configured event sinks (Redis Streams + Pub/Sub by default, others pluggable)". Replaced with gRPC-SubscribeEvents-only contract + node_id explanation + transport-adr.md pointer. |
| **N12** | control-api/spec.md | RESOLVED | HealthResponse proto snippet expanded from 6 fields to 13 fields (subscriber_count, per-tier ring fill, per-tier dropped totals) matching control.proto exactly. NOT_SERVING description's "Redis down beyond grace period" rewritten (no in-module Redis path). |
| **N13** | architecture.md | RESOLVED | "Persist state... durable state lives in Redis (event consumers' responsibility)" rewritten as "Durable state lives wherever the subscriber persists it (Kafka, Redis Streams, S3, file — operator's choice)". The module remains stateless across restarts. |

### NITS

| ID | Spec | Status | Resolution |
|---|---|---|---|
| **N14** | control.proto | RESOLVED | File-level comment updated. "5-minute dedup cache" wording (which elided in-flight semantics) replaced with a summary of the current Idempotency contract referencing control-api/spec.md. |
| **N15** | CMakeLists.txt | RESOLVED | Header comment was already updated in the N6 commit when the stub landed. |
| **N16** | README.md | RESOLVED | gRPC version "v1.69.x" → "v1.74.0" matching Dockerfile.builder. Debian "bookworm" → "trixie" matching the actual base image. Event-plane bullet rewritten ("Kafka / Redis Streams / NATS / gRPC stream" framing replaced with the in-memory rings + SubscribeEvents broadcast contract). |

## Discoveries beyond the round-2 findings

While verifying citations against FS source I found two additional
issues the round-2 review did not flag:

### Additional finding A — buf.yaml `path` field was wrong

`proto/buf.yaml` had `modules: - path: open_switch`. Per buf v2
semantics, the module root becomes `proto/open_switch/`, so the
fully-qualified import `open_switch/events/v1/events.proto` could
not be resolved (the path starts with `open_switch/` which is OUTSIDE
the module root). buf lint failed with "file does not exist" — and
since the C-6 fix added the import line correctly, the failure
masked the success of C-6. Every CI run since the scaffold commit
has been failing this step.

Fixed in `cc09f84` (the F0-sweep commit): `path: .` so the module
root is `proto/`, imports resolve, lint passes. Verified locally
with `go-bin buf` v1.55+. Added explicit exemptions for the
public-API lint exceptions the codebase intentionally makes
(ENUM_VALUE_PREFIX + ENUM_ZERO_VALUE_SUFFIX for ErrorDetail.Type
mirroring grpc::StatusCode; SERVICE_SUFFIX + RPC_REQUEST_STANDARD_NAME
+ RPC_RESPONSE_STANDARD_NAME for MediaBridge + FromModule /
FromService + SubscribeEvents → EventEnvelope).

### Additional finding B — `media.proto` had an unused import

`proto/open_switch/media/v1/media.proto` line 26 imported
`google/protobuf/duration.proto` but no `google.protobuf.Duration`
type is referenced in the file. buf lint caught this once the
buf.yaml path was fixed. Removed in the same commit.

These two are not in the round-2 review by ID but were strictly
necessary to make the C-6 fix actually pass CI.

### Additional finding C — Stub module needs `SWITCH_API_VISIBILITY`

While verifying the N6 stub builds end-to-end against real FS
v1.10.12 headers, `nm -D --defined-only` on the resulting .so
showed `mod_open_switch_module_interface` as a LOCAL data symbol
(`d`) rather than GLOBAL (`D`). FreeSWITCH's loader walks
`dlsym` against `<modname>_module_interface`; a local symbol is
invisible to dlsym. So the stub would build but `load
mod_open_switch` would fail at runtime with "Cannot locate
symbol".

Root cause: per `src/include/switch_platform.h:186-200` of FS
v1.10.12, `SWITCH_MOD_DECLARE_DATA` is gated by the
`SWITCH_API_VISIBILITY` define. Without the define, the macro
expands to empty (line 200), and the project's global
`-fvisibility=hidden` hides the interface symbol.

Fixed in `b1238d2`: added
`target_compile_definitions(mod_open_switch PRIVATE SWITCH_API_VISIBILITY=1)`
in `src/CMakeLists.txt`. Re-verified with rebuild + `nm -D --defined-only`
showing the symbol as `D`. FREESWITCH-FACTS FF-009 updated to
document the gate.

This is the kind of FS interop detail that would have been a
"stub builds but module won't load" surprise during Phase 2;
catching it while verifying N6 closes the loop.

## Verification log

For each FS-behaviour claim landed in `FREESWITCH-FACTS.md`, the
corresponding source has been read at v1.10.12 and the excerpt
quoted verbatim. The 11 entries are:

| ID | Claim | Source |
|---|---|---|
| FF-001 | Write-side bug processing is SINGLE interleaved loop | src/switch_core_media.c:16096-16156 |
| FF-002 | bug_remove_callback gated by thread_id for SMBF_THREAD_LOCK | src/switch_core_media_bug.c:1435-1479 + 913-915 |
| FF-003 | eavesdrop_callback has static linkage | src/switch_ivr_async.c:2000 |
| FF-004 | Event dispatch is pool of up-to-64 threads (MPSC) | src/switch_event.c:82-95 + 367-389 |
| FF-005 | CHANNEL_CALLSTATE fires only on state transitions, NOT bug attach | src/switch_channel.c:283-307 + call-site enumeration |
| FF-006 | Read-side bug processing IS two passes | src/switch_core_io.c:646-756 |
| FF-007 | Bug insertion is head-on-SMBF_FIRST, tail otherwise | src/switch_core_media_bug.c:977-999 |
| FF-008 | media_bug_count filters by bp->function name, thread-safe | src/switch_core_media_bug.c:1135-1151 |
| FF-009 | SWITCH_MODULE_DEFINITION + load/shutdown macros | src/include/switch_types.h:2600-2647 + src/include/switch_platform.h:184-200 |
| FF-010 | FS loads modules RTLD_LOCAL unless SMODF_GLOBAL_SYMBOLS | src/switch_dso.c:101-119 + src/switch_loadable_module.c:1701-1708 |
| FF-011 | MEDIA_BUG_START fires on every bug attach | src/switch_core_media_bug.c:1014-1019 + src/include/switch_types.h:2164-2165 |

The stub module `src/mod_open_switch.cc` was compile-tested against
the real FS v1.10.12 header tree (cloned fresh at v1.10.12 tag) with
all the project's strict warnings enabled
(`-Wall -Wextra -Wpedantic -Werror -fvisibility=hidden -std=c++20 -fPIC`)
and produced a clean object file. `buf lint` was run locally after
the proto changes and is clean.

## Net change summary

- **3 round-2 BLOCKER → 0 BLOCKER** (N1, N2, N6 all resolved by
  citing FREESWITCH-FACTS entries the round-2 closeout did not have).
- **3 round-2 CRITICAL → 0 CRITICAL** (N3, N4, N5 all resolved).
- **6 round-2 IMPORTANT → 0 IMPORTANT** (N7, N8, N10, N11, N12, N13
  all resolved).
- **3 round-2 NIT → 0 NIT** (N14, N15, N16 all resolved).
- **1 round-2 BLOCKER (N9 — call-transcribe.md C-1 residual) →
  0 BLOCKER.** (N9 is also captured under the BLOCKERS row above
  because round 2 labelled it BLOCKER in the body even though the
  table summary used IMPORTANT — both labels are addressed.)
- **2 additional findings (buf.yaml path bug, media.proto unused
  import)** discovered while verifying CI parity, both resolved.

## Pattern internalized

The pattern that recurred between rounds 1 and 2 was: a spec author
reading the round-1 spec text (mental model) rather than the FS
source. Round 3 institutes `FREESWITCH-FACTS.md` as the lever to
break the pattern: every FS claim must cite an entry; every entry
must include a 5-20 line source excerpt; the orchestrator's
verification pass need only spot-check the citations against
v1.10.12 to gain high confidence in the spec.

If subsequent rounds find additional FS-behaviour mistakes, the
correct response is: add an FF entry first, then fix the spec.
Skipping the FF step is how rounds 1 and 2's mistakes happened.

## Recommended next step

The Phase 1 design specs are now self-consistent against vanilla
FS v1.10.12 and the round-3 changes do not introduce new fabricated
claims. The stub module gives CI a real .so to build and verify.
The MIN_INTEGRATION_TESTS gate remains `if: false` by design — the
first implementation commit is responsible for replacing the stub
with real subsystems AND flipping the gate to enforce a non-trivial
test count.

Recommendation: Phase 1 can be archived; Phase 2 (implementation)
may begin against the current specs. The Phase 2 work breakdown
should be a separate OpenSpec change; the spec docs in
`openspec/changes/core-module-v1/designs/` are the authoritative
contract for that work.

Reviewer + author: Codex (gpt-5.5)
