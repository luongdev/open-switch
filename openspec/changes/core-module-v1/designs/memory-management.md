# Memory management — mod_open_switch V1

## Why this document exists at this level

`mod_open_switch` runs **in-process with FreeSWITCH**. A single
use-after-free, double-free, or persistent leak does not affect just
"a request" — it affects every SIP call through the host. FreeSWITCH
hosts in production routinely run for weeks; a 100-byte leak per call
at 50 CCU × 8 calls/min becomes ~5 GB/week.

This is the single highest-risk axis of the entire module. Every
spec, every code review, every CI gate is designed around it.

This document is normative. Code that violates these rules cannot be
merged. CI gates enforce most of them mechanically; review enforces
the remainder.

## The cost model

| Bug class | Cost | Frequency we can tolerate |
|---|---|---|
| Use-after-free | FreeSWITCH segfault → all calls drop | **Zero** |
| Double-free | FreeSWITCH segfault → all calls drop | **Zero** |
| Buffer overrun | Memory corruption, eventual crash | **Zero** |
| Leak (per-call) | Memory grows linearly with call rate; OOM in hours | **Zero on the hot path** |
| Leak (one-shot at startup) | Bounded constant; acceptable if < few MB total | Tolerable but documented |
| Race on shared state | Data corruption, occasional crashes, hard to reproduce | **Zero** |

We treat all four "zero" rows the same in CI: any detection fails the
build.

## Tooling

### AddressSanitizer + LeakSanitizer (ASAN+LSAN)

Build flag: `-fsanitize=address,leak -fno-omit-frame-pointer -g`
CI runs **every PR** with:

```
ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=1:
  detect_stack_use_after_return=1:strict_string_checks=1:
  check_initialization_order=1
LSAN_OPTIONS=exitcode=23:print_suppressions=0
```

`exitcode=23` makes LeakSanitizer non-zero on detection so CI fails
distinctly from "test failed".

Suppression file at `tests/valgrind/lsan.supp` is empty by default.
Any addition requires a maintainer review and a comment explaining
why the leak is in a non-controllable upstream library (e.g., a
known glibc/static-init false positive).

### Valgrind memcheck (nightly)

Build the module without ASAN (Valgrind is incompatible with ASAN).
Nightly job runs the integration suite under Valgrind with:

```
--leak-check=full
--show-leak-kinds=all
--errors-for-leak-kinds=definite,possible
--error-exitcode=42
--track-origins=yes
--gen-suppressions=all
--suppressions=tests/valgrind/freeswitch.supp
```

`freeswitch.supp` holds FreeSWITCH-internal leak patterns we cannot
fix (FS has its own pool model that Valgrind doesn't understand).
Any new suppression requires PR with justification.

Definite + possible leaks fail the build. Reachable allocations
(global state, etc.) are reported but do not fail by default.

### clang-tidy

Configured in `.clang-tidy` (lands with first src/ commit). Enabled
check groups:

- `bugprone-*`
- `cert-*`
- `clang-analyzer-*` (deep static analysis)
- `concurrency-*`
- `cppcoreguidelines-*` (subset; some are noisy)
- `modernize-use-*` (smart pointers, override, nullptr, …)
- `performance-*`
- `readability-redundant-*`

Warnings are errors in CI.

### clang-format

`.clang-format` config. CI runs `--dry-run --Werror`. Reformat
violations block merge.

### cppcheck

Secondary static analyzer. Catches different patterns than clang-tidy.
Errors block merge; warnings are reported.

### Coverage

`gcov` / `lcov` runs in CI; report uploaded to artifact. **Not
gating** in V1, but reviewers should ensure new logic has tests.

## RAII helpers (mandatory)

Bare FreeSWITCH C API calls are forbidden outside the helpers. New
code that calls these C functions directly must use the helpers below
or get an explicit exemption in the PR.

### `osw::SessionLock`

Pairs `switch_core_session_locate` / `switch_core_session_rwunlock`.

```cpp
namespace osw {

class SessionLock {
 public:
  explicit SessionLock(const char* uuid)
      : session_(uuid ? switch_core_session_locate(uuid) : nullptr) {}

  ~SessionLock() noexcept {
    if (session_) {
      switch_core_session_rwunlock(session_);
    }
  }

  SessionLock(const SessionLock&) = delete;
  SessionLock& operator=(const SessionLock&) = delete;

  SessionLock(SessionLock&& other) noexcept : session_(other.session_) {
    other.session_ = nullptr;
  }
  SessionLock& operator=(SessionLock&& other) noexcept {
    if (this != &other) {
      reset();
      session_ = other.session_;
      other.session_ = nullptr;
    }
    return *this;
  }

  switch_core_session_t* get() const noexcept { return session_; }
  switch_channel_t* channel() const noexcept {
    return session_ ? switch_core_session_get_channel(session_) : nullptr;
  }
  explicit operator bool() const noexcept { return session_ != nullptr; }

  void reset() noexcept {
    if (session_) {
      switch_core_session_rwunlock(session_);
      session_ = nullptr;
    }
  }

 private:
  switch_core_session_t* session_;
};

}  // namespace osw
```

Usage pattern:

```cpp
if (auto lock = osw::SessionLock(uuid)) {
  auto* chan = lock.channel();
  switch_channel_set_variable(chan, "foo", "bar");
}
// Automatic rwunlock at scope exit.
```

### `osw::EventGuard`

Pairs `switch_event_create*` / `switch_event_destroy`. Designed to
make "create then maybe fire" patterns safe:

```cpp
namespace osw {

class EventGuard {
 public:
  // Create a CUSTOM event.
  explicit EventGuard(switch_event_types_t type = SWITCH_EVENT_CUSTOM)
      : event_(nullptr) {
    if (switch_event_create(&event_, type) != SWITCH_STATUS_SUCCESS) {
      event_ = nullptr;
    }
  }

  // Take ownership of an existing event* (from a callback, etc.).
  static EventGuard adopt(switch_event_t* event) {
    EventGuard g;
    g.event_ = event;
    return g;
  }

  ~EventGuard() noexcept {
    if (event_) {
      switch_event_destroy(&event_);
    }
  }

  EventGuard(const EventGuard&) = delete;
  EventGuard& operator=(const EventGuard&) = delete;
  EventGuard(EventGuard&& other) noexcept : event_(other.event_) {
    other.event_ = nullptr;
  }

  switch_event_t* get() const noexcept { return event_; }
  explicit operator bool() const noexcept { return event_ != nullptr; }

  // Fire the event. Ownership transfers to FreeSWITCH; the guard
  // becomes empty.
  switch_status_t fire() {
    if (!event_) return SWITCH_STATUS_FALSE;
    switch_status_t s = switch_event_fire(&event_);
    // switch_event_fire sets event_ to NULL on success.
    return s;
  }

  // Release ownership without destroying or firing. Caller is
  // responsible for the event* thereafter.
  switch_event_t* release() noexcept {
    auto* e = event_;
    event_ = nullptr;
    return e;
  }

 private:
  EventGuard() = default;
  switch_event_t* event_;
};

}  // namespace osw
```

Usage:

```cpp
osw::EventGuard ev;
if (!ev) return;  // creation failed
switch_event_add_header_string(ev.get(), SWITCH_STACK_BOTTOM,
                               "Event-Subclass", "osw::bot_started");
switch_event_add_header_string(ev.get(), SWITCH_STACK_BOTTOM,
                               "Unique-ID", uuid);
ev.fire();  // ownership transfers; guard becomes empty automatically
```

### `osw::MediaBugLease`

Pairs `switch_core_media_bug_add` / `switch_core_media_bug_remove`.
Lives for the duration of the bug. Releasing the lease removes the
bug from the channel.

```cpp
namespace osw {

class MediaBugLease {
 public:
  MediaBugLease(switch_core_session_t* sess,
                const char* name,
                const char* function,
                switch_media_bug_callback_t cb,
                void* user_data,
                time_t stop_time,
                uint32_t flags)
      : session_(sess), bug_(nullptr) {
    switch_status_t s = switch_core_media_bug_add(
        sess, name, function, cb, user_data, stop_time, flags, &bug_);
    if (s != SWITCH_STATUS_SUCCESS) {
      bug_ = nullptr;
    }
  }

  ~MediaBugLease() noexcept { remove(); }

  MediaBugLease(const MediaBugLease&) = delete;
  MediaBugLease& operator=(const MediaBugLease&) = delete;
  MediaBugLease(MediaBugLease&& other) noexcept
      : session_(other.session_), bug_(other.bug_) {
    other.session_ = nullptr;
    other.bug_ = nullptr;
  }

  switch_media_bug_t* get() const noexcept { return bug_; }
  explicit operator bool() const noexcept { return bug_ != nullptr; }

  void remove() noexcept {
    if (bug_ && session_) {
      switch_core_media_bug_remove(session_, &bug_);
      bug_ = nullptr;
    }
  }

 private:
  switch_core_session_t* session_;
  switch_media_bug_t* bug_;
};

}  // namespace osw
```

### `osw::XmlNode`

Pairs `switch_xml_open_*` / `switch_xml_free`. Used only by the config
loader; not on the hot path.

### Smart pointers — when to use what

- `std::unique_ptr<T>` — single ownership. Default choice for C++ heap
  allocations.
- `std::shared_ptr<T>` — when ownership is genuinely shared (e.g.,
  gRPC completion-queue tag may outlive the originating handler).
  Avoid `shared_from_this` patterns unless necessary; document why.
- `std::weak_ptr<T>` — for back-pointers in cyclic graphs. Watch for
  the dangling check pattern (`if (auto p = weak.lock()) { ... }`).

Raw pointers are allowed only as:
- Non-owning views (e.g., a function parameter that does not store the
  pointer).
- Inside FS C API calls.
- Documented exceptions with a clear ownership comment.

## FreeSWITCH memory pools

For data scoped to a session or a channel, prefer FS memory pools
over C++ heap:

```cpp
char* dup = switch_core_session_strdup(session, "hello");
// freed automatically when session is destroyed
```

This avoids the leak risk if the session ends and our C++ destructor
doesn't run (FS owns the destruction trigger; C++ scope-based RAII
may not match the lifetime).

For module-scoped data (config, gRPC server state), use C++ smart
pointers — the module's load/unload functions provide deterministic
construction/destruction points.

For per-RPC state, gRPC's arenas (enabled via `cc_enable_arenas=true`
in our protos) handle protobuf allocation. Other heap allocs use
`std::make_unique` inside the handler.

## Exception-safety boundary

FreeSWITCH is pure C. C++ exceptions MUST NOT propagate into FS
callbacks. Every C-callable entry point wraps its body:

```cpp
extern "C" switch_status_t my_state_handler_on_hangup(
    switch_core_session_t* session) {
  try {
    return osw::on_hangup_impl(session);
  } catch (const std::exception& e) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                      "open_switch: on_hangup exception: %s\n", e.what());
    return SWITCH_STATUS_GENERR;
  } catch (...) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                      "open_switch: on_hangup unknown exception\n");
    return SWITCH_STATUS_GENERR;
  }
}
```

The C-callable entry points are:
- Module load / shutdown.
- Event bind callbacks.
- State handlers (on_init, on_hangup, on_reporting, on_destroy, ...).
- Media bug callbacks.
- App handlers (if we register any).
- API handlers (if we register any).

Per-subsystem helper layers (`osw::events`, `osw::media`, ...) can
use exceptions internally, but they MUST catch at the C boundary.

## gRPC completion queue safety

Asynchronous gRPC services use a completion queue with tag-based
event dispatching. A common bug pattern is:

```cpp
// WRONG
auto* call = new MyCall(...);
cq->AsyncNext(...);
delete call;  // race: may be on CQ thread or shutdown thread
```

Pattern we use:

```cpp
// RIGHT — call object owned by shared_ptr; tag is the raw pointer,
// but ownership lives in a registry keyed by call_id. Each completion
// either advances state (keeps the shared_ptr alive) or releases it
// (drops the registry entry).
auto call = std::make_shared<MyCall>(...);
{
  std::lock_guard lk(registry_mu_);
  registry_[call->id()] = call;
}
call->Proceed(...);
// On Done: registry_.erase(call_id) → last shared_ptr drops → ~MyCall.
```

Reactor pattern (gRPC 1.40+) is preferred over the manual completion
queue for new services because the reactor's `OnDone` callback gives
us a deterministic cleanup point.

## Concurrency: mutex usage

- `std::mutex` / `std::shared_mutex` for in-process state.
- Always lock via `std::lock_guard` or `std::unique_lock`.
- `std::scoped_lock` for multi-mutex acquisition (deadlock-free).
- Use atomic types (`std::atomic<int>`, etc.) for hot counters.
- Document lock ordering in subsystem-level comments. The order is:
  1. Idempotency cache mutex
  2. Tenant ACL mutex
  3. Event ring mutex (per-tier, may hold concurrently)
  4. Stream registry mutex
  5. Channel-state mutex (acquired last; releasing back to FS holds
     FS-internal locks that must not be held alongside ours)

## What CI checks (mechanically)

1. **Build with ASAN+LSAN** — every PR, every push to main.
2. **Run all unit tests under ASAN+LSAN** — every PR.
3. **Run all integration tests under ASAN+LSAN** — every PR. The
   integration runner exits 23 on leak detection.
4. **clang-format dry-run** — every PR.
5. **clang-tidy** — every PR (once src/ lands).
6. **cppcheck** — every PR.
7. **Nightly Valgrind** — once a day on `main`. Definite + possible
   leaks fail the build, page on-call.
8. **Markdown lint** — every PR (spec docs must remain readable).

## Code review checklist (in `.github/pull_request_template.md`)

Reviewer responsibility:

- [ ] All `new` is wrapped in smart pointers or RAII class
- [ ] All `switch_core_session_locate` paired with `_rwunlock` (RAII
      preferred)
- [ ] All `switch_event_create*` either fired or in `EventGuard`
- [ ] All `switch_xml_open_*` in `XmlNode`
- [ ] All `switch_core_media_bug_add` in `MediaBugLease`
- [ ] No `malloc/calloc/free` (use FS pool or C++ new/delete-RAII)
- [ ] gRPC completion-queue tags owned by `shared_ptr` or registry
- [ ] All C-callable callbacks wrapped in `try { ... } catch (...)`
- [ ] Mutex via `lock_guard`/`unique_lock`/`scoped_lock`
- [ ] Lock order respected (or new order documented)
- [ ] Exceptions never thrown across module boundary

## What CI does NOT catch (review must)

1. **Pointer ownership confusion**: a raw pointer passed around may
   not leak ASAN-visibly if it's still reachable via some path. Review
   that ownership is documented.
2. **Logic leaks**: e.g., an entry added to a map but the removal path
   has a bug. Bounded LRU caches mitigate; review still required.
3. **Lifetime mismatch across language boundary**: FS pool destroys
   `char*` we passed to FS; our C++ struct still holds a copy of the
   pointer. Review that we always `strdup` (or copy) when storing.
4. **Deadlocks**: ASAN+LSAN don't detect deadlocks. ThreadSanitizer
   (TSAN) does, but TSAN is mutually exclusive with ASAN. We run TSAN
   manually before each release.
5. **Async cleanup races**: detector-friendly only if both threads
   touch the same byte. If one thread leaks the object and the other
   never sees it, ASAN may miss. Review for hand-off correctness.

## When ASAN gives a false positive (rare but happens)

If ASAN+LSAN report a leak inside a third-party library we can't
modify (e.g., gRPC's internal cache, glibc static-init):

1. Open an issue with reproduction.
2. Add a suppression to `tests/valgrind/lsan.supp` with the issue
   link and a justification comment.
3. Maintainer reviews the suppression in PR.

A suppression is technical debt. We aim for an empty `lsan.supp`.

## On the cost of strictness

Strict memory safety adds ~10-15% to development time. We accept
this because the alternative — a leak in production — costs orders
of magnitude more (call drops, customer churn, on-call burn).

Reviewers should expect to push back on PRs that take shortcuts.
The author should expect "use RAII here" comments even if the code
"works" — readability and review safety are first-class concerns
on this codebase.
