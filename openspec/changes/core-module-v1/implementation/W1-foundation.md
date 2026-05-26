# W1 — Foundation (real module shell)

**Status.** Spawned 2026-05-26. Replaces the Phase-1 stub
(`src/mod_open_switch.cc` returning SUCCESS) with a real loadable
module that boots a gRPC server, parses XML config, exposes the
ControlService skeleton (with `Health` working), and ships the
mandatory RAII helpers from `designs/memory-management.md`.

This is the **first** implementation wave. Subsequent waves (events,
control RPCs, media) layer on top of the infrastructure W1 builds.

---

## Why a wave-based implementation

Two W5-class fabricated-FS-semantic mistakes during Phase 1 specs
established the discipline that every FS API claim cites a
`FREESWITCH-FACTS.md` entry (see `FREESWITCH-FACTS.md` rationale).
The implementation phase inherits that discipline: each wave lands a
**bounded** set of subsystems, ships through CI's ASAN + LSAN +
clang-tidy + clang-format gates, then receives a Codex review of
**both code and the docs that ship with it** before the next wave
begins. The wave boundary is where review happens; intra-wave commits
are atomic but not separately reviewed.

W1 is intentionally **conservative**: it adds the smallest amount of
code that lets `load mod_open_switch` actually do something useful
(start a gRPC server, answer Health), while putting in place all the
infrastructure later waves need (config, logging, RAII, server
lifecycle).

---

## Scope — what W1 ships

### IN — implement and verify

1. **`src/core/` — module lifecycle**
   - `module.h` / `module.cc` — singleton `osw::Module` class that owns
     Config, Logger, Health, GrpcServer, and any per-process state.
     Constructed in `mod_open_switch_load`, destroyed in
     `mod_open_switch_shutdown`.
   - `config.h` / `config.cc` — XML config loader using
     `switch_xml_config_parse_module_settings` (FF-cite the FS facility
     used; see "FACT discipline" below). Reads
     `${conf_dir}/autoload_configs/open_switch.conf.xml`. Falls back to
     compiled-in defaults if the file is absent. SIGHUP reload **not**
     required in W1 — defer to W4.
   - `lifecycle.h` / `lifecycle.cc` — drain orchestration. W1 implements
     only the `DRAINING` flag + `Health` status flip; full drain
     sequence (event ring flush, channel hupall, etc.) lands when those
     subsystems exist.

2. **`src/observability/` — logging + health**
   - `log.h` / `log.cc` — `osw::log::{trace,debug,info,warn,error,critical}`
     wrappers over `switch_log_printf`. PII redaction for caller-id +
     destination patterns (regex list from config; default empty).
     Trace correlation: thread-local `current_traceparent` via
     `osw::log::TraceScope`. Output mode: plain text in W1 (JSON is a
     stretch goal — only add if it does not pull in extra deps; we want
     no new transitive deps in W1).
   - `health.h` / `health.cc` — `osw::Health` aggregator. W1 exposes
     `module_version`, `freeswitch_version`, `status` (SERVING /
     DRAINING). All counter fields default to 0 (real updates land in
     W2/W3/W4 as their owning subsystems come online).

3. **`include/osw/raii/` — RAII helpers (mandatory)**

   Implement **verbatim** from
   `designs/memory-management.md` §"RAII helpers":
   - `osw::SessionLock` (locate + rwunlock)
   - `osw::EventGuard` (create + destroy / fire)
   - `osw::MediaBugLease` (add + remove)
   - `osw::XmlNode` (open + free)

   Header-only. Move-only (deleted copy ops). Unit tests cover:
   construction, destruction, move-construction, move-assignment,
   double-release, explicit `reset()`/`remove()`/`release()`,
   `operator bool` semantics.

   These tests **cannot** call real FreeSWITCH (no FS process at unit
   test time). Use a function-pointer test seam or a header-only mock
   layer keyed on a build-time `OSW_TEST_FS_MOCK` macro. The agent
   chooses the cleanest seam — document the choice in
   `tests/unit/raii/README.md`.

4. **`src/control/` — gRPC server skeleton**
   - `server.h` / `server.cc` — `osw::control::GrpcServer`. Lifecycle:
     `Start()` binds the address, registers `ControlService::Service`,
     starts on its own worker thread. `Drain(deadline)` calls
     `grpc::Server::Shutdown(deadline)` (deadline from config). Owns
     the `std::unique_ptr<grpc::Server>`.
   - `tls.h` / `tls.cc` — `osw::control::MakeServerCreds(const Config&)`.
     If `grpc-tls-cert` + `grpc-tls-key` paths set in config, builds
     `grpc::SslServerCredentials` (with optional `grpc-tls-ca` for
     mTLS); otherwise returns `grpc::InsecureServerCredentials()`. PEM
     loaded via `std::ifstream` — RAII covers cleanup.
   - `handlers/health_handler.cc` — REAL `Health` impl. Returns the
     current `osw::Health` snapshot.
   - `handlers/unimplemented.cc` — ALL OTHER RPCs (Originate, Hangup,
     Bridge, Execute, SetVariables, Hold, Unhold, BlindTransfer,
     HangupMany, SubscribeEvents) return `grpc::StatusCode::UNIMPLEMENTED`
     with message `"not yet implemented (wave W<N> coming)"`. Use a
     single `ControlServiceSkeleton` class that overrides every method
     to call a shared helper.

5. **Proto generation in CMake**
   - Add `cmake/proto.cmake` that wires up `protoc` + `grpc_cpp_plugin`.
     Input: `proto/open_switch/{control,events,media}/v1/*.proto`.
     Output: `build/_proto/open_switch/{control,events,media}/v1/*.pb.{h,cc}` +
     `*.grpc.pb.{h,cc}`. Generated sources compiled into an internal
     static lib `osw_proto` linked privately into `mod_open_switch`.
   - `buf` lint already configured (`proto/buf.yaml`). Add a CMake
     target `osw_proto_lint` that shells out to `buf lint` if `buf` is
     on PATH; non-blocking when absent (so OS-package builds without
     buf still work).

6. **Module entry rewrite**
   - `src/mod_open_switch.cc` becomes thin: declares the module table,
     calls `osw::Module::Instance().Load(pool, module_interface)` and
     `osw::Module::Instance().Shutdown()`. Exception-safe wrappers per
     `memory-management.md` §"Exception-safety boundary".
   - `SWITCH_MODULE_DEFINITION` macro stays as-is (FF-009 already
     verified).
   - `SWITCH_API_VISIBILITY=1` define remains in `src/CMakeLists.txt`.

7. **Default config file**
   - `deploy/freeswitch/conf/autoload_configs/open_switch.conf.xml`
     with documented defaults. Operators copy this into their FS conf
     tree.
   - Loadable example for the runner image: `deploy/docker/runner-conf/`
     mounting point (Dockerfile already exists — add a copy or
     volume-mount in the compose example).

8. **Unit tests (`tests/unit/`)**
   - `core/config_test.cc` — happy path, malformed XML, missing file
     fallback to defaults, validation failures (ring size < 256, TTL
     <= 0, missing TLS key when cert set, etc.).
   - `core/lifecycle_test.cc` — Status transitions SERVING → DRAINING.
   - `observability/log_test.cc` — PII redaction with at least 3
     regex test cases (E.164 number, US-style 10-digit, custom regex
     from config). No FS process required (log layer is mockable).
   - `observability/health_test.cc` — snapshot semantics.
   - `control/server_test.cc` — Start binds, Health RPC returns
     SERVING with non-empty version, Shutdown(deadline) returns within
     deadline.
   - `raii/*_test.cc` — one file per RAII helper, see "include/osw/raii"
     above.

9. **CI integration**
   - Update `.github/workflows/build-and-asan.yml` (or whatever the
     ASAN job is named — check existing) to: build with
     `OSW_BUILD_TESTS=ON OSW_ENABLE_ASAN=ON OSW_STRICT_WARNINGS=ON`,
     run `ctest --test-dir build --output-on-failure`,
     `clang-format --dry-run --Werror`, `clang-tidy -p build`. The
     "min integration test" gate stays at `if: false` (real FS
     integration is W5).

### OUT — explicitly deferred to later waves

- `switch_event_bind` callbacks, tier classifier, MPSC rings,
  `SubscribeEvents` broadcaster → **W2 (Events)**.
- Originate / Hangup / Bridge / Execute / SetVariables / Hold /
  Unhold / BlindTransfer / HangupMany real impls + idempotency LRU +
  per-tenant ACL → **W3 (Control)**.
- Media bug manager + stage ranks + bidirectional gRPC streams +
  resampler → **W4 (Media)**.
- Eavesdrop guard (Layer 1 `osw_eavesdrop` app + Layer 2
  `MEDIA_BUG_START` detector) → **W4.5 (Security)**.
- Real FreeSWITCH integration test → **W5**.
- SIGHUP hot-reload → **W4 or W5**.
- Prometheus / structured-JSON logging → **post-V1**.

W1 commits **must not** include code targeting OUT items even
speculatively. If a piece of OUT scaffolding would obviously be
needed (e.g., a `Tier` enum referenced by an event-plane file), the
agent stops and adds a one-line TODO with a comment pointing at the
wave that owns it.

---

## SSOT (read these before writing any code)

| File | Why |
|---|---|
| `openspec/changes/core-module-v1/FREESWITCH-FACTS.md` | Every FS API call cites an FF-NNN entry |
| `openspec/changes/core-module-v1/designs/architecture.md` | Plane breakdown + source-dir layout + drain order |
| `openspec/changes/core-module-v1/designs/memory-management.md` | RAII helpers verbatim + checklist |
| `openspec/changes/core-module-v1/designs/transport-adr.md` | Why gRPC is the only event transport |
| `openspec/changes/core-module-v1/specs/control-api/spec.md` | Idempotency, ACL, error codes (W1 only needs Health, but read for context) |
| `proto/open_switch/control/v1/control.proto` | Service skeleton |
| `proto/open_switch/events/v1/events.proto` | Envelope shape (W1 generates code, doesn't use it) |
| `proto/open_switch/media/v1/media.proto` | (W1 generates code only) |
| `src/CMakeLists.txt` | `SWITCH_API_VISIBILITY=1` discipline |
| `CMakeLists.txt` | Existing options + sanitizer wiring |
| `deploy/docker/Dockerfile.builder` | What system deps already exist |

---

## FACT discipline (non-negotiable)

Every FreeSWITCH C API call in W1 code MUST be paired with a comment
citing the relevant `FF-NNN` entry. If the FS function isn't covered
by an existing FACT, you must:

1. Open the FS v1.10.12 source on github.com/signalwire/freeswitch.
2. Verify the function's actual behaviour with a 5-20 line excerpt.
3. Add a new FF-NNN entry following the format in
   `FREESWITCH-FACTS.md`, in its OWN commit (`docs(facts): FF-NNN ...`).
4. Only then write the calling code in a follow-up commit citing the
   new entry.

Already-cited FACTs you'll use in W1:

- **FF-009** — module entry macros, `SWITCH_API_VISIBILITY=1` discipline.
  Used in `src/mod_open_switch.cc` (already present in the stub) and
  validated by the build.
- **FF-010** — `RTLD_LOCAL` loader semantics. Drives our
  `-Wl,--exclude-libs,ALL` link rule. Comment in
  `src/CMakeLists.txt` (already present).

FACTs you will likely **introduce** during W1 (verify each at
v1.10.12 before citing):

- `switch_xml_config_parse_module_settings` behaviour (does it
  allocate? does it free input? where does error reporting go?).
- `switch_log_printf` thread-safety + format-string contract.
- `switch_loadable_module_create_module_interface` lifetime model
  (pool-owned, no manual free).

If you find yourself writing "I think FS does X" without citing source,
**stop and verify**. This is the failure mode that cost two rounds
during specs. The discipline is the same here.

---

## Memory safety (re-read `memory-management.md` before each commit)

Reviewer checklist (from `memory-management.md`):

- [ ] All `new` is wrapped in smart pointers or RAII class
- [ ] All `switch_core_session_locate` paired with `_rwunlock` (RAII preferred)
- [ ] All `switch_event_create*` either fired or in `EventGuard`
- [ ] All `switch_xml_open_*` in `XmlNode`
- [ ] All `switch_core_media_bug_add` in `MediaBugLease` *(none in W1)*
- [ ] No `malloc/calloc/free` (use FS pool or C++ new/delete-RAII)
- [ ] gRPC completion-queue tags owned by `shared_ptr` or registry
  *(W1 uses sync gRPC; no CQ tags yet)*
- [ ] All C-callable callbacks wrapped in `try { ... } catch (...)`
- [ ] Mutex via `lock_guard`/`unique_lock`/`scoped_lock`
- [ ] Lock order respected (or new order documented)
- [ ] Exceptions never thrown across module boundary

ASAN + LSAN is the **gate**. If a unit test fails under
`ASAN_OPTIONS=halt_on_error=1:detect_leaks=1`, the commit does not
ship. Period.

---

## Verification (run all of these before each commit)

```bash
# Clean build with sanitizers + strict warnings + tests
cmake -S . -B build \
    -DOSW_BUILD_TESTS=ON \
    -DOSW_ENABLE_ASAN=ON \
    -DOSW_STRICT_WARNINGS=ON \
    -DCMAKE_BUILD_TYPE=Debug

cmake --build build -j$(nproc)

# Run tests with ASAN/LSAN options
ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1 \
LSAN_OPTIONS=exitcode=23:print_suppressions=0 \
    ctest --test-dir build --output-on-failure

# Format check
clang-format --dry-run --Werror $(git ls-files '*.cc' '*.h')

# Static analysis (skip if clang-tidy not on PATH; CI handles this)
clang-tidy -p build $(git ls-files 'src/**/*.cc')

# Module export sanity check (after `make install` or similar)
nm -D --defined-only build/src/mod_open_switch.so | \
    grep mod_open_switch_module_interface
# Expected: a single line ending in 'D' (global data symbol). If 'd',
# SWITCH_API_VISIBILITY was lost — fix before commit.
```

If any of the above fails, **fix before committing**. Do not push
broken HEAD.

---

## Commit discipline

- **Branch**: `implementation/wave1-foundation` off current `main`.
- **Commit unit**: one logical deliverable per commit. Suggested
  sequence (adjust as you learn):
  1. `feat(build): proto codegen in CMake (cmake/proto.cmake)`
  2. `feat(raii): osw::SessionLock + tests`
  3. `feat(raii): osw::EventGuard + tests`
  4. `feat(raii): osw::MediaBugLease + tests`
  5. `feat(raii): osw::XmlNode + tests`
  6. `feat(observability): osw::log wrapper + PII redaction`
  7. `feat(observability): osw::Health aggregator`
  8. `feat(core): osw::Config XML loader + tests`
  9. `feat(core): osw::Module singleton + lifecycle`
  10. `feat(control): GrpcServer skeleton + TLS creds builder`
  11. `feat(control): Health RPC + UNIMPLEMENTED handlers for rest`
  12. `feat(mod): wire osw::Module into mod_open_switch.cc`
  13. `feat(deploy): default open_switch.conf.xml`
  14. `chore(ci): wire ASAN + clang-tidy + clang-format gates`
  15. `docs(facts): FF-NNN ... <per added entries>`
- **Sign every commit** as:
  ```
  Co-Authored-By: Codex (gpt-5.5) <noreply@openai.com>
  ```
  This is the established convention for C++ FreeSWITCH-semantic work
  on this repo (the round-3 fix sprint used the same signature). It
  helps `git log` traceability so the next review knows which agent
  did the work.
- **Body**: include the spec / FACT references the commit relies on.
  Example body shape:
  ```
  Implements osw::SessionLock per memory-management.md §"RAII helpers".
  Pairs switch_core_session_locate (FF-XXX to be added) with
  switch_core_session_rwunlock. Move-only; explicit reset() drops the
  lock early.

  Tests cover construction, double-reset, move-construction,
  move-assignment.
  ```

---

## What Codex will review after W1

After all W1 commits land on `implementation/wave1-foundation`, Codex
will be invoked to review:

1. **Code**:
   - Memory safety per `memory-management.md` checklist
   - Every FS API call paired with FF-NNN citation
   - RAII helpers match `designs/memory-management.md` verbatim
   - ASAN + clang-tidy clean
   - gRPC server lifecycle correct (no use-after-Shutdown)
   - Exception boundary at every C-callable entry
2. **Docs**:
   - Header file Doxygen-style comments on public types
   - `tests/unit/raii/README.md` documents the FS-mock seam choice
   - Any new FF-NNN entries follow the file's format
   - Default `open_switch.conf.xml` has comments explaining each param
   - This contract file (`W1-foundation.md`) gets a closeout note if
     scope shifted

Codex's report lands as
`openspec/changes/core-module-v1/reviews/codex-W1.md`. Blockers
must be closed before W2 spawns.

---

## When to stop and ask

The agent SHOULD proceed autonomously for the cases above. The agent
MUST stop and surface the question if any of the following arise:

- A FACT entry can't be verified at v1.10.12 (FS source has changed
  shape, or the function is in a non-public header).
- A required gRPC C++ API doesn't exist at the version the builder
  image ships (check `Dockerfile.builder`).
- A test seam choice would require restructuring an entire subsystem
  to be mockable (red flag — usually a simpler seam exists).
- An ASAN error persists after good-faith debugging.

Surfacing happens by writing a short `BLOCKER-<topic>.md` under
`openspec/changes/core-module-v1/implementation/` and stopping work.
The orchestrator picks up the blocker on the next pass.

---

## Out-of-band hygiene

- Update `openspec/changes/core-module-v1/tasks.md` line items as W1
  deliverables land (it currently has stale Redis references — leave
  those alone unless your commit is touching them; spec hygiene is a
  separate task).
- The CHANGELOG file (if it exists) gets a new `[Unreleased]` entry
  per landed wave. If it doesn't exist, don't create one in W1 — that
  decision is the orchestrator's.

Good luck. Bring the FACTS sheet.
