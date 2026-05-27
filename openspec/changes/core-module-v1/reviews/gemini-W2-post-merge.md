---
frontmatter:
  model: Gemini
---
# Gemini W2 Events post-merge review - code + docs

**Date**: 2026-05-26
**Reviewer**: Gemini
**Branch reviewed**: `main` (commit `b415e74`)
**Prior context**: W2-events.md, codex-W2.md

---

## Verdict

**NOT READY - 1 BLOCKER, 1 CRITICAL, 2 NITS.**

The W2 event plane demonstrates excellent RAII and memory discipline. The code makes careful use of zero-copy shared pointers, localized protobuf arenas, and strict C-boundary safety. However, the current code violates its own documented lock hierarchy, creating a ticking time bomb for deadlocks. Furthermore, there is a severe mismatch in Tier-1 event routing that undermines the resilience of the tiering system.

---

## Findings

### [BLOCKER] Lock order inversion in `Broadcaster::AddSubscriberAtomic`
**File**: `src/events/subscribe/broadcaster.cc:113`

The wave contract and header documentation establish a strict, W2-wide lock order: `tier ring mu -> roster mu -> SendQueue mu`. The documentation explicitly states "Reverse acquisition NEVER occurs". However, `AddSubscriberAtomic` intentionally violates this hierarchy.

**Excerpt**:
```cpp
        // Lock order: roster_mu_ -> (inside replay_fn) ring mu ->
        // SendQueue mu. Workers acquire ring mu and roster mu strictly
        // sequentially (never both), so the reversed (roster->ring)
        // order here cannot deadlock with them.
        std::lock_guard<std::mutex> lk(roster_mu_);
        if (replay_fn) {
            replay_fn(*sub);
        }
```

**Why it's a blocker**:
While the worker thread currently acquires and releases `ring_mu` before waiting on `roster_mu`, relying on this temporal separation to permit a reversed lock hierarchy is incredibly fragile in a HARDENED project. Any future change where a worker or producer needs to hold both locks simultaneously will cause an immediate, silent deadlock.

**Recommendation**:
Eliminate the lock inversion. A lock-free deduplication replay or sequential locking model should be used:
1. Snapshot the ring (acquires & releases `ring_mu`).
2. Snapshot the roster & add subscriber (acquires & releases `roster_mu`).
3. Snapshot the ring AGAIN (acquires & releases `ring_mu`).
4. Replay entries, deduplicating the window between the two snapshots based on sequence number.
Alternatively, use a lightweight reader-writer lock or generation counter for the roster so replay can happen without holding a rigid mutex against the workers.

### [CRITICAL] Tier-1 default expansion mismatch degrades billing queue
**File**: `src/events/tier.cc:12` and `include/osw/events/tier.h:17`

The implementation pushes high-frequency call-state events into Tier-1, directly contradicting the documentation and design rationale which reserves Tier-1 for critical/low-volume billing and HA-grade events.

**Excerpt from `src/events/tier.cc:12`**:
```cpp
// W2-default Tier-1 events. See header comment for rationale.
constexpr const char* kDefaultTier1Events[] = {
    "CHANNEL_CREATE",
    "CHANNEL_PROGRESS",
    "CHANNEL_ANSWER",
    "CHANNEL_BRIDGE",
    "CHANNEL_UNBRIDGE",
    "CHANNEL_DESTROY",
    "CHANNEL_HANGUP_COMPLETE",
    // ...
```

**Excerpt from `include/osw/events/tier.h:17`**:
```cpp
 *   Tier 2 - call-state. Important but eventually-consistent OK. Default
 *            mappings: CHANNEL_CREATE, CHANNEL_DESTROY, CHANNEL_ANSWER,
 *            CHANNEL_PROGRESS, CHANNEL_CALLSTATE, CHANNEL_HOLD,
```

**Why it's critical**:
By mapping high-volume events (`CHANNEL_CREATE`, `PROGRESS`, `ANSWER`, `DESTROY`) to Tier-1, the system severely increases the churn rate in the critical ring. Under heavy call load, this will trigger premature FIFO evictions of genuinely critical billing events (`CHANNEL_HANGUP_COMPLETE`, `CDR_REPORT`).

**Recommendation**:
Align the implementation with the documentation. Move `CHANNEL_CREATE`, `CHANNEL_PROGRESS`, `CHANNEL_ANSWER`, and `CHANNEL_DESTROY` to `kDefaultTier2Events` in `src/events/tier.cc`.

### [NIT] TSAN-bypass wire scanner isolation verified
**File**: `tests/unit/events/envelope_decode_for_test.h`

The review explicitly verified the scope of the TSAN-bypass logic. A full scan of `src/` and `include/` confirms that `envelope_decode_for_test.h` is strictly confined to the unit test suite and has not leaked into the production build path.

**Recommendation**:
Maintain current isolation. Consider adding a small `CMakeLists.txt` or linter guard rule to forbid inclusion of `*_for_test.h` headers outside the test targets.

### [NIT] Strict RAII and memory discipline adhered to
**File**: `src/control/handlers/subscribe_events_handler.cc` and `src/events/envelope.cc`

The C++ memory leak / RAII discipline is robust. The codebase properly isolates FreeSWITCH `char*` lifetimes to within callback scopes (FF-019), delegates envelope creation to safe protobuf Arenas, and correctly manages zero-copy transport via `std::shared_ptr<const std::string>`.

**Excerpt from `src/control/handlers/subscribe_events_handler.cc:106`**:
```cpp
        arena.Reset();
        // Arena allocation properly managed inside loop
        // (allocator omitted for formatting constraints)
        if (!env->ParseFromString(*bytes)) {
```

**Recommendation**:
None required. The implementation is clean and leak-free.
