# Contributing to open-switch

Thank you for considering a contribution. This module runs inside the
FreeSWITCH process: a bug here can crash FreeSWITCH for every caller on the
host. Standards are high.

## License

By contributing, you agree your contribution is licensed under
[AGPL-3.0-or-later](LICENSE). For commercial-license contributions, contact
the maintainer before submitting.

## Workflow

1. Open an issue describing the change before non-trivial work.
2. Branch from `main`. Branch name: `feat/<short>` / `fix/<short>` /
   `docs/<short>`.
3. Make commits scoped per logical change. Conventional Commits format:
   `feat(events): add tier-2 routing rule for CHANNEL_STATE`.
4. Open a PR. Fill out the PR template completely — the memory-management
   checklist is non-optional.
5. CI must pass (build + ASAN + LSAN + unit + integration).
6. Reviewer must explicitly sign off on the memory-management checklist.

## Memory-management checklist (mandatory for every PR)

This codebase loads into FreeSWITCH as a shared object. Leaks accumulate
across calls; double-frees segfault FreeSWITCH. The following are
non-negotiable:

- [ ] Every `new` is wrapped in `std::make_unique` / `std::make_shared`,
      or owned by an explicit RAII class. No raw `new` / `delete` pairs in
      new code.
- [ ] Every `switch_core_session_locate(...)` call is paired with
      `switch_core_session_rwunlock(...)` in the same scope. Prefer the
      `osw::SessionLock` RAII wrapper.
- [ ] Every `switch_event_create*` whose event is not subsequently fired
      via `switch_event_fire` uses `osw::EventGuard` to ensure destruction.
- [ ] Every `switch_xml_open_*` is paired with `switch_xml_free`.
- [ ] Every `switch_core_media_bug_add` is paired with
      `switch_core_media_bug_remove` on the same channel scope. Prefer the
      `osw::MediaBugLease` RAII wrapper.
- [ ] No `malloc` / `calloc` / `free`. Use FreeSWITCH memory pools
      (`switch_core_alloc(pool, size)`) for session-scoped data, or C++
      RAII for module-scoped data.
- [ ] gRPC completion-queue events use `std::shared_ptr` for tag lifetime.
      No raw tag pointers without a clear owner.
- [ ] gRPC async stream reactors release the call object exactly once
      (either OnDone or via cancellation path, not both).
- [ ] Exceptions never propagate to FreeSWITCH C callbacks. Wrap every
      C-callable entry point in `try { ... } catch (...)`.
- [ ] Mutexes use `std::lock_guard` / `std::unique_lock`. Bare `lock()` /
      `unlock()` pairs only with documented justification.

CI runs AddressSanitizer + LeakSanitizer on every PR. A single byte leaked
across the integration test suite fails the build. Nightly Valgrind catches
issues ASAN may miss.

## Code style

- C++20. Prefer the standard library; no Boost unless justified.
- `clang-format` config is `.clang-format` at repo root. CI enforces.
- `clang-tidy` runs in CI; warnings are errors.
- Header guards use `#pragma once`.
- Namespaces: `osw::` for module code; sub-namespaces per subsystem
  (`osw::control`, `osw::events`, `osw::media`, `osw::transports`).

## Testing

- Every new logic file has a `*_test.cc` in `tests/unit/`.
- Integration tests live in `tests/integration/` and require a running
  FreeSWITCH + Redis (provided by the test fixture).
- Memory tests in `tests/memory/` are dedicated leak / lifecycle scenarios
  run under ASAN and Valgrind separately.
- Coverage target: 70%+ line coverage on `src/`. CI reports but does not
  yet block on coverage.

## Reviewing

Reviewers MUST check:

1. The memory-management checklist is filled out and accurate.
2. Any new C-callable callback is wrapped in `try/catch`.
3. Any new gRPC service method has timeout and cancellation handling.
4. Any new event type is classified into a tier (Tier 1 / Tier 2 / Tier 3).
5. Any new public API has a corresponding spec in `openspec/`.
6. ASAN+LSAN CI is green. Read the report, not just the green check —
   `LSAN_OPTIONS=detect_leaks=1` must be observed in CI logs.

## Disclosure

For security vulnerabilities, do NOT open a public issue. See
[SECURITY.md](SECURITY.md).
