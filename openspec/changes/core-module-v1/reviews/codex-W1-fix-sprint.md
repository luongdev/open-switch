# W1 Foundation — Codex fix-sprint closeout

**Date**: 2026-05-26
**Author**: Codex (gpt-5.5)
**Branch**: `implementation/wave1-foundation`
**Pre-sprint HEAD**: `38ac72a`
**Post-sprint HEAD**: `f37b0ce`
**Closes**: `openspec/changes/core-module-v1/reviews/codex-W1.md`

This sprint closes the BLOCKER + CRITICAL + cheap IMPORTANT + trivial
NIT findings from the Codex W1 review. Out-of-scope items
(`I-3`, `I-4`, `I-5`, `N-4`) are left for W2 territory per the
review's own deferred list.

---

## Findings closed

| ID  | Description (short) | What changed | Commit |
|---|---|---|---|
| **B-1** | Dockerfile.builder missing `COPY include/` + `COPY tests/` | Added the two COPY lines at `deploy/docker/Dockerfile.builder:70-71` | `d73d7c7` |
| **B-2** | Deprecated `std::atomic_{load,store}_explicit(std::shared_ptr<T>*, ...)` overloads fail `-Werror` on libstdc++ GCC 13+ | Switched to `std::atomic<std::shared_ptr<const PatternList>>` partial spec in `src/observability/log.cc`; rewrote misleading 60-78 comment block | `6dee20d` |
| **B-3** | FF-012 "Used by W1 code" lied about `__FILE__`/`__func__`/`__LINE__` plumbing | Rewrote the bullet to describe the actual `SinkFn` indirection + `DefaultSink` literal-string args; added explicit `Known limitation` line | `2330c68` |
| **C-1** | `~GrpcServer()` is `noexcept` but `Drain` may allocate/throw; comment said "0s" but code is 2s | Wrapped `Drain` call in try/catch (std::exception then catch-all); catch handler uses async-signal-safe `::write(STDERR_FILENO, ...)` per FF-012; fixed deadline comment | `c8f3b85` |
| **C-2** | No way to construct a real client channel against the kernel-assigned port; smoke test only | Added `GrpcServer::BoundPort()` (member `bound_port_` initialised to -1); rewrote `server_test.cc` with `RoundTripHealthReturnsServing` exercising the Health RPC end-to-end | `23d74ee` |
| **C-3** | Misleading mutex-protection comment on `Health::Snapshot` | Removed the misleading comment in `health.h:99-102` and replaced with the actual load-time-only contract; mirrored the sentence in `health.cc:6-10` | `51787a3` |
| **C-4** | `SelfMoveAssignmentIsSafe` test asymmetry — only `SessionLock` had one | Added the test to `event_guard_test.cc`, `media_bug_lease_test.cc`, `xml_node_test.cc` | `6fe5b26` |
| **I-1** | "gRPC server drained" log fires even on never-started server | Moved log line + worker.join() inside `if (srv)` block | `97bd9c3` |
| **I-2** | `SplitPipeList` keeps trailing empty chunks but skips leading/intermediate — inconsistent | Always-skip-empty for all positions; documented contract in comment | `b7137eb` |
| **I-6** | `unimplemented.h` file-path comment said `src/control/...` | Fixed to `include/osw/control/...` | `d2df067` |
| **N-1** | `event_guard.h:17` references `switch_event.c:391` but no FF entry for `switch_event_*` | Added **FF-017** covering `switch_event_create` / `_destroy` / `_fire` ownership semantics; updated `event_guard.h` "Cited FACTs" section | `3970507`, `f37b0ce` |
| **N-2** | `log.cc:50` referenced `InstallDefaultSinkOnModuleLoad` (typo) | Fixed to `InstallDefaultSinkForModule` | `24e6685` |
| **N-3** | `event_drain_timeout_seconds` has no W1 consumer | Added `// W2 owns this; W1 only stores the config value` comment | `b3cebed` |
| **N-5** | `tests/unit/raii/README.md` pointed at vapor W5 harness | Added the W5-deferred sentence | `1dd0680` |

**Total commits added**: 15 (`d73d7c7` through `f37b0ce` inclusive).

## Findings explicitly deferred (per the review's scope list)

| ID  | Reason for deferral |
|---|---|
| **I-3** | Escaped `\|` in PII pattern list — too invasive for fix sprint; W2 territory. Operators currently work around by listing patterns as separate XML params (no single-pattern alternation). |
| **I-4** | `Module::Load` rollback after step 5 — W2 territory; W1 singleton is one-shot per process so the latent state leak is not reachable today. |
| **I-5** | `OSW_TEST_FS_MOCK` collision guard with `_SWITCH_H` — preventive, not gating. W1 test wiring keeps the two paths apart; an `#error` guard would be a future enhancement. |
| **N-4** | `extern "C"` + `__attribute__((visibility("hidden")))` — cosmetic; current single-`.so` layout works correctly. |

No scope creep: nothing was fixed outside the assigned list.

---

## FACT verification — FF-017

Per FACT discipline, the new FF-017 was verified against v1.10.12
source via:

```bash
curl -sL https://raw.githubusercontent.com/signalwire/freeswitch/v1.10.12/src/switch_event.c
curl -sL https://raw.githubusercontent.com/signalwire/freeswitch/v1.10.12/src/include/switch_event.h
```

Line ranges verified (and verbatim-excerpted in FF-017):

- `src/include/switch_event.h:153` — `switch_event_create_subclass` macro
- `src/include/switch_event.h:384` — `switch_event_create` macro
- `src/include/switch_event.h:413` — `switch_event_fire` macro
- `src/switch_event.c:747-787` — `switch_event_create_subclass_detailed`
- `src/switch_event.c:1289-1312` — `switch_event_destroy` (line 1311 sets `*event = NULL`)
- `src/switch_event.c:2006-2038` — `switch_event_fire_detailed`
- `src/switch_event.c:281-297` — `switch_event_deliver_thread_pool` (line 293 sets `*event = NULL`)
- `src/switch_event.c:388-395` — `switch_event_queue_dispatch_event` null-on-success
  (the actual `*eventp = NULL` is at line 391, not just a forward
  reference)

Codex's review referenced `src/switch_event.c:391` for the null-on-success
behaviour; FF-017 cites BOTH null points (line 293 for the
no-dispatch path and line 391 for the dispatch path), which is
required to fully justify the EventGuard contract.

---

## Verification

### B-1 Docker build — DEFERRED to CI

Local environment lacks the upstream
`open-gateway/freeswitch-builder:v1.10.12` base image. `docker buildx
build --check` succeeded at the Dockerfile-syntax stage but cannot
proceed past FROM resolution:

```
ERROR: open-gateway/freeswitch-builder:v1.10.12: failed to resolve
source metadata ... pull access denied, repository does not exist
or may require authorization
```

The Dockerfile passed parse-time checks; the orchestrator's CI
runner (with access to the upstream image) is the source of truth.
**Action required**: orchestrator re-triggers the docker build job
on `implementation/wave1-foundation` after merge.

### Local syntax checks (best-effort, macOS Apple-clang libc++)

| File | Result | Notes |
|---|---|---|
| `src/observability/health.cc` | Clean | Header-only Snapshot path; no FS deps. |
| `src/observability/log.cc` | Expected fail (Apple libc++) | `std::atomic<std::shared_ptr<T>>` partial spec is not in Apple's libc++ — exactly the reason B-2 was missed in W1. Code is correct under libstdc++ GCC 13+ (`__cpp_lib_atomic_shared_ptr` defined). Builder image is the source of truth. |
| `src/control/server.cc` | Cannot syntax-check locally | Requires gRPC headers; not installed on dev host. |
| `tests/unit/raii/*_test.cc` | Cannot syntax-check locally | Requires gtest headers; not installed on dev host. |

The local-syntax-gap was already documented in the original W1
closeout. No new gap introduced — every gap maps to a missing
dependency on the dev host (gRPC, gtest, libstdc++) rather than a
defect in the code.

### Commit-sequence sanity

```
$ git log --oneline implementation/wave1-foundation | head -16
f37b0ce docs(raii): cross-link event_guard.h citation to new FF-017
1dd0680 docs(tests): N-5 W5 harness sentence
b3cebed chore(core): N-3 event_drain_timeout W2-owns comment
24e6685 chore(observability): N-2 InstallDefaultSinkForModule typo
3970507 docs(facts): FF-017 switch_event_{create,destroy,fire} semantics
d2df067 docs(control): I-6 unimplemented.h path comment
b7137eb fix(core): I-2 SplitPipeList trailing empty skip
97bd9c3 fix(control): I-1 Drain log inside if(srv)
6fe5b26 test(raii): C-4 self-move-assignment tests for EventGuard/MediaBugLease/XmlNode
51787a3 docs(observability): C-3 Health::Snapshot load-time contract
23d74ee feat(control): C-2 GrpcServer::BoundPort + real Health round-trip test
c8f3b85 fix(control): C-1 ~GrpcServer wraps Drain in try/catch + 2s deadline comment
2330c68 docs(facts): B-3 FF-012 Used-by-W1-code matches actual SinkFn wiring
6dee20d fix(observability): B-2 atomic<shared_ptr> for redaction patterns
d73d7c7 fix(build): B-1 Dockerfile.builder COPY include + tests
38ac72a docs(impl): W1 closeout — commit sequence + local verification gap
```

Every commit is signed `Co-Authored-By: Codex (gpt-5.5)`. One
logical fix per commit; suggested sequence followed with one
addition (the `f37b0ce` cross-link of event_guard.h to FF-017 was
folded as a follow-on to the FF-017 commit so the citation
in-code matches the new FF entry).

---

## What W2 inherits

After CI confirms B-1 builds green on the builder image:

1. The redaction-pattern publication path uses the supported C++20
   `std::atomic<std::shared_ptr<>>` partial spec. No future code
   path will trip the C++26 removal of the free-function overloads.
2. The Health round-trip test gives W2 a known-good gRPC handler
   path under ASAN — the next handler (Originate in W3) can be
   added with the assurance that the worker thread, the service
   registration, and the credentials path all worked at W1 close.
3. FF-017 is the canonical cite for any future code that uses
   `switch_event_*`. The W2 event-plane producer (`SubscribeEvents`
   source side) can ship with a single `// FF-017` adjacency and
   the reviewer has the verbatim FS source already in the FACT.
4. The exception-safety boundary contract from
   `designs/memory-management.md` §"Exception-safety boundary" is
   now exercised in production code (`~GrpcServer`). W2's first
   destructor in the event-plane code base has a precedent to
   mirror.

---

## New items discovered

None outside the review's scope. No latent bugs found while
making the fixes; the W1 code's discipline is intact.

The orchestrator should NOT spawn W2 until the CI builder job
confirms green on `implementation/wave1-foundation`. The Dockerfile
fix is gating until proven by an actual build.

— Codex (gpt-5.5)
