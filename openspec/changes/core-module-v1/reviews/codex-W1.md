# Codex W1 Foundation review — code + docs

**Date**: 2026-05-26
**Reviewer**: Codex (gpt-5.5)
**Branch reviewed**: `implementation/wave1-foundation` (HEAD `38ac72a`)
**Diff vs `main`**: 56 files / +6315 / -94 lines / 10 commits
**Prior context**: Phase-1 rounds 1-3
(`reviews/codex-phase1*.md`) — every FS-behaviour claim must cite
`FREESWITCH-FACTS.md` with a v1.10.12 source excerpt. This W1 wave is
the FIRST implementation review; same discipline applies to code +
the FACT entries that ship with it.

---

## Verdict

**NOT READY — 3 BLOCKERS, 4 CRITICALS, 6 IMPORTANTS, 5 NITS.**

The wave correctly stays in scope (no W2/W3/W4 creep), the 5 new FF
entries (FF-012..FF-016) all check out at v1.10.12, the RAII helpers
are noexcept-move-correct, and the FS-mock seam choice is clean. The
**blockers are CI / build-path** problems that will prevent the
branch from going green in the builder container — the implementer
was honest in the closeout about lacking a local builder, and the
gaps are exactly the ones a Linux+gRPC environment would have caught
in five minutes.

W2 cannot spawn until the BLOCKERS are closed and a green CI run
exists on `implementation/wave1-foundation`.

---

## FACT verification — FF-012..FF-016

| FF | Claim | v1.10.12 source check | Verdict |
|---|---|---|---|
| **FF-012** | `switch_log_printf` queues to async log thread; thread-safe; printf-format | `src/switch_log.c:538-546` matches excerpt verbatim; declaration at `src/include/switch_log.h:142-145` matches verbatim; async push path lives in `switch_log_meta_vprintf` (not `switch_log_vprintf` as the surrounding text in FF-012 might imply, but the function `switch_log_printf` forwards to is correct: `switch_log_meta_vprintf` per `src/switch_log.c:541`). | **VERIFIED** |
| **FF-013** | `switch_xml_config_parse_module_settings` opens cfg, frees XML before returning | `src/switch_xml_config.c:72-89` matches verbatim. `switch_xml_free(xml)` is called at line 84. NULL-return + FALSE on open failure verified. | **VERIFIED** |
| **FF-014** | `switch_loadable_module_create_module_interface` allocates from pool; no manual free | `src/switch_loadable_module.c:3033-3045` matches verbatim. `switch_core_alloc(pool, ...)` + `switch_core_strdup(pool, name)` + `switch_thread_rwlock_create(..., pool)` all confirmed. | **VERIFIED** |
| **FF-015** | `switch_xml_open_cfg` returns refcounted root; `switch_xml_free(NULL)` no-op | `src/switch_xml.c:2499-2513` (open_cfg) and `2815-2841` (free top) match verbatim. NULL-safe early return at `xml ? : return` confirmed. | **VERIFIED** |
| **FF-016** | `switch_core_session_locate` returns read-locked session; caller MUST rwunlock | `src/switch_core_session.c:121-146` matches verbatim. The "if its not NULL, now it's up to you to rwunlock this" comment is at line 144 of v1.10.12, as cited. | **VERIFIED** |

All five new FACTs check out. No fabricated semantics this round.
**This is the most important pre-merge fact**: the FACT discipline
held in the first implementation wave. The W5-class failure mode
that drove rounds 1/2 was prevented.

One nit on FF-012 (filed as **N-1** below): the implication
*"switch_log_printf forwards to switch_log_meta_vprintf"* is correct,
but the doc text in the "Used by W1 code" section calls out
`src/switch_event.c:391` for `switch_event_fire` ownership-transfer
semantics that aren't covered by any FF entry. Either add a dedicated
FF-017 for `switch_event_*` (preferred — the EventGuard cites it
inline), or move that detail into the FF-012 implications.

---

## BLOCKERS — must close before W2 spawns

### B-1 — `deploy/docker/Dockerfile.builder` does not `COPY include/` or `tests/`; CI build will fail

**File**: `deploy/docker/Dockerfile.builder:66-69`.

The Dockerfile's `COPY` lines are:

```dockerfile
COPY CMakeLists.txt ./
COPY proto/ ./proto/
COPY src/ ./src/
COPY cmake/ ./cmake/
```

W1 introduces an entire new top-level directory `include/osw/` (10
new headers totalling 1148 lines) and `tests/` (10 new `*.cc` files
+ 6 CMakeLists.txt totalling 1485 lines). NONE of those paths are
copied into the builder image. The build will fail at the very first
`#include "osw/core/module.h"` in `src/core/module.cc` because
`${CMAKE_SOURCE_DIR}/include/` doesn't exist inside the container.

In CI specifically: `.github/workflows/ci.yml:51` passes
`--build-arg OSW_BUILD_TESTS=ON`. The cmake config will then call
`add_subdirectory(tests)` (root CMakeLists.txt:150) which fails
because `tests/CMakeLists.txt` doesn't exist in the container's
build context. Both ON and OFF paths break.

This is the exact W5-class CI-silent-failure pattern from round 3
(N6 — `cp /usr/local/mod/mod_open_switch.so` step in the same
Dockerfile silently failed because the file was never produced).
The implementer's closeout note says *"the macOS dev host does NOT
have cmake, gRPC..."* — that's exactly why the Dockerfile gap was
invisible.

**Fix**: Add to `Dockerfile.builder:69`:

```dockerfile
COPY include/ ./include/
COPY tests/ ./tests/
```

Then re-trigger the CI build job manually after the fix lands.

### B-2 — `std::atomic_load_explicit(std::shared_ptr<T>*, ...)` is deprecated in C++20 / removed in C++26; `-Werror` will fail on libstdc++

**File**: `src/observability/log.cc:81,85`.

The redaction-pattern publication path uses:

```cpp
std::shared_ptr<const PatternList> LoadPatterns() noexcept {
    return std::atomic_load_explicit(&PatternsSlot(), std::memory_order_acquire);
}
void StorePatterns(std::shared_ptr<const PatternList> snap) noexcept {
    std::atomic_store_explicit(&PatternsSlot(), std::move(snap),
                               std::memory_order_release);
}
```

The free-function `std::atomic_{load,store}_explicit` overloads
taking `std::shared_ptr<T>*` were `[[deprecated]]`-tagged in C++20
(LWG 3766) and removed in C++26. libstdc++ on GCC 13+ (Debian
trixie ships GCC 13/14) emits `-Wdeprecated-declarations`. Under
the project's `OSW_STRICT_WARNINGS=ON` (`-Werror`, enabled by
default in `Dockerfile.builder:74`), this aborts the build.

The comment block at lines 60-78 of `log.cc` is internally
contradictory: it states *"we use a raw atomic<shared_ptr*>
indirection so that std::atomic<std::shared_ptr<>> is not strictly
required"* — but the implementation does NOT use a raw
`atomic<shared_ptr*>`; it uses the deprecated free-function
overloads on a plain `std::shared_ptr<>&`. The intent (avoid the
`std::atomic<std::shared_ptr>` partial spec) is sound; the
implementation didn't match.

**Fix options** (pick one):
- Switch to `std::atomic<std::shared_ptr<const PatternList>>`. GCC
  13+ ships the partial specialization. Drop the free-function
  calls.
- Wrap behind a `std::shared_mutex` + plain `std::shared_ptr<const
  PatternList>`. Slower but portable.
- Suppress the deprecation locally with
  `[[suppress(deprecated-declarations)]]` and document why — least
  preferred; the deprecation exists for a reason.

### B-3 — Doc-code mismatch in FF-012 "Used by W1 code" section: `switch_log_printf` is called with `"%s\n", line` from `DefaultSink`, not from the per-level wrappers

**Files**: `FREESWITCH-FACTS.md:1019-1025` vs `src/observability/log_default_sink.cc:73-75`.

FF-012 says:

> **Used by W1 code:**
> - `src/observability/log.cc` — `osw::log::{trace,debug,info,warn,error,critical}`
>   forward to `switch_log_printf(SWITCH_CHANNEL_LOG, __FILE__, __func__,
>   __LINE__, NULL, level, "%s", formatted_message.c_str())`.

That is **not** how the code is wired. `osw::log::{...}` does NOT
forward to `switch_log_printf` directly; it forwards to a `SinkFn`
function-pointer slot which defaults to a NullSink, gets replaced
by `DefaultSink` only after `Module::Load` calls
`InstallDefaultSinkForModule()`. `DefaultSink` (in
`log_default_sink.cc:73-75`) is what calls `switch_log_printf` —
and it passes `"mod_open_switch"` / `"osw_log_emit"` / `0` for
`file`/`func`/`line` (not `__FILE__`/`__func__`/`__LINE__` as the
FF claims).

The implementer's closeout already self-reports this gap
(*"Per-call-site `__FILE__` / `__func__` / `__LINE__` are not yet
plumbed through the `osw::log::*` wrappers"*). So this is a known
behaviour gap. But the FF-012 text reads as a contract for what W1
DID; the code does NOT match that contract. Since FF-012 is the
canonical "what we cite when calling switch_log_printf" reference,
the contradiction will confuse the next reader.

**Fix**: Rewrite FF-012's *Used by W1 code* bullet to describe what
actually shipped — the SinkFn indirection, the DefaultSink wrapper
in `log_default_sink.cc`, the literal-string `file`/`func` args.
Document the `__FILE__`-plumbing gap as a known limitation with a
W1.5 / W2 follow-up tag.

---

## CRITICALS — should fix before W2; W2 can begin in parallel only on subsystems that don't collide

### C-1 — `GrpcServer::Drain` is called from `~GrpcServer()` which is `noexcept`; `Drain` is not noexcept and may allocate/throw

**File**: `src/control/server.cc:30-34` + `96-114`.

```cpp
GrpcServer::~GrpcServer() noexcept {
    // Defensive: if Drain wasn't called explicitly, do it now with a
    // 0s deadline so the destructor doesn't block indefinitely.
    Drain(std::chrono::system_clock::now() + std::chrono::seconds(2));
}
```

Two issues:

1. The comment says *"0s deadline"* but the code uses
   `+ std::chrono::seconds(2)`. Doc-code mismatch.
2. `Drain` calls `osw::log::Info("control", "gRPC server drained")`
   at line 113. Logging allocates inside `Logv` (heap fallback at
   line 159-167 of `log.cc`); under OOM (admittedly rare) it can
   throw `std::bad_alloc`. The destructor is `noexcept`, so an
   uncaught exception in `Drain` would call `std::terminate` —
   crashing FreeSWITCH on its way out. Not strictly wrong (we ARE
   on the way out), but it's silent and not what the noexcept marker
   would suggest.

**Fix**: Wrap `~GrpcServer()`'s `Drain` call in a try/catch (per
`designs/memory-management.md` §"Exception-safety boundary"). Fix
the comment to match the actual 2s deadline.

### C-2 — `GrpcServer::Start` does not store the actually-bound port; `server_test.cc` cannot do a real round-trip RPC

**File**: `src/control/server.cc:53-78` + `tests/unit/control/server_test.cc:60-77`.

`builder.AddListeningPort` writes the resolved port to `bound_port`
(local stack), but only logs it. There is no `BoundPort()`
accessor. The test acknowledges this at line 60-77 and falls back
to opening a channel against `"127.0.0.1:0"` — which **cannot**
work as a client connection (port 0 in a client means "let kernel
pick"; the gRPC channel will fail to connect). The test
`BoundAddressReflectsConfig` (line 84-86) just checks the config
string is echoed back; the real Health-RPC round-trip is missing.

This is the implementer's own listed gap, so it's not a surprise
finding. But it means the test's value is limited to: "Start
returns true, Drain doesn't crash under ASAN". It does NOT verify
the Health RPC actually serves SERVING, does NOT verify the
service registration, does NOT verify the worker thread's
exception handler is reachable. Half the W1 contract for
`control/server_test.cc` is unmet:

> Start binds, Health RPC returns SERVING with non-empty version,
> Shutdown(deadline) returns within deadline.

The "Health RPC returns SERVING" assertion is the most important
behavioural test for W1's Control plane — that's the ONE real RPC
this wave ships.

**Fix**: Add `int GrpcServer::BoundPort() const noexcept { return
bound_port_member_; }` and store the value into a new member from
the local `bound_port` variable at the end of `Start`. Update
`server_test.cc` to call `BoundPort()` and construct
`"127.0.0.1:" + std::to_string(server_->BoundPort())` for the
client channel. Add a `RoundTripHealthReturnsServing` test.

### C-3 — `Health::Snapshot` is documented as mutex-protected but is not

**File**: `include/osw/observability/health.h:99-119`.

The header comment at lines 99-102:

```cpp
// Versions are immutable after SetVersions; we still wrap them in
// a small mutex-protected struct to avoid std::string thread-safety
// assumptions. Reads happen during Snapshot().
mutable std::atomic<Status> status_{Status::kServing};
```

But the actual `module_version_` / `freeswitch_version_` are bare
`mutable std::string` at lines 118-119, with no mutex. The comment
is a vestige from a refactor that didn't carry the implementation.

The actual safety story is in `src/observability/health.cc:7-9`:
*"SetVersions captures both strings under no synchronisation
because it is called exactly once during mod_open_switch_load,
before any other thread observes the Health instance"*. That's the
real contract — and it's only valid IFF `SetVersions` is provably
single-threaded and ordering with respect to the first reader is
established. In `Module::Load` it IS established (Health is
constructed in the singleton's ctor before any consumer; the load
function is serialised by `mu_` per module.cc:78).

But the test fixture `GrpcServerTest::SetUp` (server_test.cc:42-49)
calls `health_->SetVersions(...)` on the main test thread, then
constructs `GrpcServer`, then `server_->Start(...)` which spawns a
worker thread that eventually services `Health` RPC calls. The
happens-before is established by the thread's spawn (a release-
acquire sync), so reading `module_version_` from the gRPC thread is
fine. But if **any** future caller decides to call `SetVersions`
again at runtime, this becomes a torn-write race.

The TestSink in `server_test.cc` doesn't actually invoke the Health
RPC (per C-2), so this race wouldn't show up in current tests.

**Fix**: Either remove the misleading header comment and document
the "load-time only" contract explicitly, OR make the strings
properly synchronized (a `std::shared_mutex` would do; reads are
infrequent). Removing the misleading comment is the cheaper fix —
the actual semantics are sound for W1.

### C-4 — `EventGuard` self-move-assignment is guarded in code but not tested; `SessionLock` tests it; the asymmetry is suspicious

**File**: `include/osw/raii/event_guard.h:99-108` +
`tests/unit/raii/event_guard_test.cc`.

`event_guard.h`'s `operator=(EventGuard&&)` has `if (this !=
&other)` (line 100). `media_bug_lease.h` has it (line 88).
`xml_node.h` has it (line 82). `session_lock.h` has it (line 61).
All four pass `if this is &other, do nothing` correctly.

`session_lock_test.cc:121-130` has an explicit
`SelfMoveAssignmentIsSafe` test. The other three do NOT have one.

This isn't a bug — the code is correct in all four — but it means
that if a future refactor were to drop the `if (this != &other)`
guard in `EventGuard::operator=`, the only test that would fail is
the SessionLock one. The asymmetry weakens the test corpus's
ability to catch regressions per the RAII contract.

**Fix**: Add `SelfMoveAssignmentIsSafe` to
`event_guard_test.cc`, `media_bug_lease_test.cc`,
`xml_node_test.cc` — five-line tests, no design change. Symmetry
with the existing SessionLock test.

---

## IMPORTANTS — fix during W2 timeline; not gating

### I-1 — `Drain` log line `"gRPC server drained"` always fires, even when `Start` was never called

**File**: `src/control/server.cc:113`.

`Drain` runs unconditionally on destructor even if `Start` was
never called. The drain log line fires regardless. The
`grpc::Server::Shutdown` call is guarded by `if (srv)` (line 106)
but the log line at 113 is not. Result: a misleading "drained"
log on an unstarted server.

**Fix**: Move the log inside the `if (srv) { ... }` block.

### I-2 — `SplitPipeList` treats trailing empty differently from leading/intermediate empty chunks

**File**: `src/core/config_fs.cc:66-86`.

For input `"abc|"` the function emits `["abc", ""]` (the trailing
empty is unconditionally added at the npos branch on line 77). For
`"|abc"` it emits `["abc"]` (the leading empty is filtered on line
81-83). For `"abc||def"` it emits `["abc", "def"]` (intermediate
empty filtered). The trailing-empty case is the odd one out.

Consequence: a PII pattern list with a trailing pipe will produce
a `std::regex("")` in the compiled patterns list, which is "match
empty string" — every log line becomes `[REDACTED]`. The
`CompilePatterns` defensive belt in `module.cc:42-54` would NOT
catch this because empty regex compiles successfully.

**Fix**: Either always-skip-empty or always-keep-empty. Pick one
and document it. Recommended: always-skip-empty (matches the
intent: empty pattern is useless).

### I-3 — `SplitPipeList` does not handle escaped `|` (operator can't include a `|` in a regex)

**File**: `src/core/config_fs.cc:66-86`.

The orchestrator self-reported this gap. The deploy
`open_switch.conf.xml:114-119` documents the escape with
`&amp;#x7c;` for XML, which gets decoded to `|` at the XML layer
before our splitter sees it — so the escape doesn't work end-to-end.

An operator who needs `\d{3}|\d{4}` as a single pattern cannot
express it: the splitter will tear it into two patterns.

For W1, the PII patterns supplied via the in-XML pipe list are not
required to support alternation (operators can write two separate
patterns). But the gap should be documented and the splitter
contract should be: "a single pattern that needs internal `|`
should be expressed as two separate list entries that produce
equivalent matches".

**Fix**: Add a note in `open_switch.conf.xml`'s
pii_redaction_patterns comment; cross-link from
`config_fs.cc::SplitPipeList`.

### I-4 — `Module::Load` does not unwind partial side effects on failure paths after step 5

**File**: `src/core/module.cc:76-146`.

`Load` performs steps 1-7 sequentially:

1. Install default sink (no rollback path)
2. LoadConfigFromFile (no global state)
3. Validate (no global state)
4. SetRedactionPatterns (publishes a shared_ptr; survives Load fail)
5. SetVersions on Health (writes to Health which lives on)
6. Start gRPC server (`grpc_server_.reset()` on fail — handled)
7. Lifecycle::TransitionToServing (writes Health status)

If step 6 fails after step 4 / 5 succeeded, the publishes from 4
and 5 are NOT unwound. The next call to `Load` (after a
hypothetical retry, or after `Module::Instance()` being a
process-singleton means re-load attempts) would re-publish from
defaults, which is OK in practice. But if `Module::Instance()`
ever gains a `Reset()` or if `Load` is retried, the residual
state from a partial failed load would leak across.

For W1 the singleton is one-shot per process so this is latent.
For W4's SIGHUP reload it becomes real.

**Fix** (W2 territory): factor Load into staged + commitable
sub-steps with rollback handles. Not blocking for W1.

### I-5 — `OSW_TEST_FS_MOCK` test compilation: the mock `fs_mock.h` declares `switch_*` opaque types that may collide with the real `<switch.h>` if both are included in the same TU

**File**: `include/osw/raii/fs_mock.h:54-94` + `fs_api.h:40-44`.

`fs_api.h` does:

```cpp
#if defined(OSW_TEST_FS_MOCK)
#include "osw/raii/fs_mock.h"
#else
#include <switch.h>
...
#endif
```

If a future test TU were to `#define OSW_TEST_FS_MOCK 1` AND also
include something that pulls in `<switch.h>` (e.g., a header from
`osw_core_fs` accidentally exposed), the two declarations would
conflict — `fs_mock.h` declares `switch_status_t = int` whereas FS
declares it as an enum. The compile error would be loud but
confusing.

For W1 the wiring is clean (only the FS-agnostic libs are linked
into tests), so this isn't a current bug. But future test authors
who try to add a unit test that exercises both `osw_core_fs` and
the mock will hit it.

**Fix**: Add a `#error` in `fs_mock.h` if both `OSW_TEST_FS_MOCK`
and `_SWITCH_H` (or similar FS-internal guard) are defined. Add a
note in `tests/unit/raii/README.md`'s "When NOT to use this seam"
section.

### I-6 — `unimplemented.h` header doc-comment says "src/control/handlers/unimplemented.h"; the file lives at `include/osw/control/handlers/unimplemented.h`

**File**: `include/osw/control/handlers/unimplemented.h:2`.

Stale file-path comment from a refactor. Cosmetic but contradicts
the header guard `OSW_CONTROL_HANDLERS_UNIMPLEMENTED_H_` which
correctly reflects the `include/` path.

**Fix**: One-line edit; replace `src/control/handlers/...` with
`include/osw/control/handlers/...` in the doc comment.

---

## NITS — implementer may decline

### N-1 — FF-012 references `switch_event.c:391` for switch_event_fire ownership semantics but doesn't add an FF entry for it

**File**: `FREESWITCH-FACTS.md` (overall structure).

`event_guard.h:17` cites `switch_event.c:391` for the
"switch_event_fire sets caller's ev to NULL on success" claim. This
claim is FS-behavioural; per the FACT discipline it should have its
own FF entry. The implementer left it as an inline source-line
reference instead.

**Fix**: Either add FF-017 covering `switch_event_create` /
`switch_event_destroy` / `switch_event_fire` semantics, OR move
the inline reference up into FF-012's implications (it's adjacent
enough). Optional.

### N-2 — `src/observability/log.cc:50` comment refers to `InstallDefaultSinkOnModuleLoad()` but the function is named `InstallDefaultSinkForModule()`

Cosmetic.

### N-3 — `event_drain_timeout_seconds` is declared in `Config` and validated by `Validate` (config.cc:74-75) but no W1 code reads it; the field is purely scaffolding for W2

This matches the contract's "stretch-OK" stance (defaults stored
for the schema), but a `// W2 owns` comment next to the field
would help.

### N-4 — `osw_log_install_default_sink` is `extern "C"` but `-fvisibility=hidden` is set; the symbol is correctly internal-linkage-resolved across the two static libs, but a `__attribute__((visibility("hidden")))` would make the intent explicit

The current setup works because both static libs link into the
same `.so`, but documenting the visibility explicitly avoids future
confusion if someone splits the module into multiple `.so`s.

### N-5 — `tests/unit/raii/README.md` §"When NOT to use this seam" says future helpers needing callback delivery should "run against a small FS-in-a-thread harness (W5 territory)"; that harness doesn't exist yet and the section reads like it's pointing at vapor

Add a sentence: *"W5 will introduce the harness; until then, helpers needing callback delivery are deferred to W5."*

---

## What CI would have caught locally

The implementer's closeout is candid about what they could not run
locally (no cmake, no gRPC, no FS headers on macOS). That covers
**B-2** (deprecation only fires on libstdc++/GCC at -Werror) and
the line-count gaps in unit-test files. It does NOT cover **B-1**
— a sanity `docker build` against the Dockerfile would have caught
the missing COPY lines in ~30 seconds. The next implementer (W2)
should run a `docker buildx build --target fs-builder -f
deploy/docker/Dockerfile.builder .` on EVERY wave before
declaring closeout. Add this to the contract for W2.

## Orchestrator's personal re-check list

1. Apply the **B-1** fix to Dockerfile.builder and trigger a real
   docker build in CI. The current branch CI is red even without
   this review.
2. Skim `src/observability/log.cc:60-87` and decide on the **B-2**
   fix path (move to `std::atomic<std::shared_ptr<T>>` vs.
   `std::shared_mutex`). Either is fine; W2 inherits the choice.
3. Confirm the **B-3** FF-012 doc-rewrite lands BEFORE W2 cites
   FF-012 in any new FS-API code — the contract for new code
   needs to read true.
4. Verify the C-2 fix lands a real Health round-trip test;
   otherwise W3's first commit will be the first time we exercise
   the gRPC handler path under ASAN.

## What W1 did right (so it's not all critique)

- All five new FACTs verified clean against v1.10.12 source. The
  excerpts match line-for-line. The "fabricated FS semantic"
  failure mode from rounds 1+2 was NOT repeated.
- Every `switch_*` call in C++ code has an adjacent `// FF-NNN`
  comment, routed through a single `osw::raii::fs::*` shim layer
  (`include/osw/raii/fs_api.h`). The discipline is easy to audit.
- All four RAII helpers have `noexcept` move ops (verified at
  `include/osw/raii/{event_guard,media_bug_lease,session_lock,xml_node}.h`).
  Future `std::vector<EventGuard>` will move, not copy.
- The FS-mock seam choice (`-DOSW_TEST_FS_MOCK=1` + header-only
  alternate impl) is the right balance — zero production-side
  overhead, opaque-pointer forward decls for incomplete types.
  `tests/unit/raii/README.md` documents the rationale clearly.
- W1 stays bounded — no Originate, no SubscribeEvents, no media
  bugs. The UNIMPLEMENTED handlers correctly note the wave that
  will land each RPC.
- Exception-boundary discipline at `mod_open_switch.cc:64-114` is
  textbook: try/catch around the C++ entry, returns
  `SWITCH_STATUS_GENERR` on any exception path, never re-throws.
  The `switch_log_printf("%s", e.what())` discipline (printf
  format with literal `"%s"`) is correctly applied.
- The `osw_panic_on_unhandled` knob from round-3 N5 is preserved
  in the Config and surfaced in `open_switch.conf.xml`. The
  default-OFF stance from the round-3 fix is intact.

## Summary counts

| Category | Count |
|---|---|
| BLOCKER | 3 |
| CRITICAL | 4 |
| IMPORTANT | 6 |
| NIT | 5 |

**Verdict: NOT READY.** Close B-1, B-2, B-3 (and at minimum C-2 —
the Health round-trip test is necessary for any W2 to inherit a
known-good gRPC server) before spawning W2.

— Codex (gpt-5.5)
