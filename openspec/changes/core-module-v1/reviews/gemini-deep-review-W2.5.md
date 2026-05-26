# W2.5 Deep Review — Pending Fixes

**Date**: 2026-05-26
**Reviewer**: Tech Lead / Gemini
**Branch reviewed**: `implementation/wave2-events`

## Overview

A deep review of the C++ source code following the W2.5 fix-sprint shows that the core concurrency architecture, lock-order discipline, and memory safety are in excellent shape. The most dangerous issues raised by Codex have been successfully resolved:

- **B-1 (Replay race)** is fixed elegantly with `AddSubscriberAtomic` and proper lock-order (`roster_mu_` -> `ring_mu_`).
- **B-2 (Dead-lettered Audit)** is correctly replaced with an FS-log warning.
- **C-1 (Filter on replay)** and **C-3 (TSAN Data race)** are handled perfectly.

**However**, the fix-sprint missed a few items and misreported one critical fix. These must be addressed in a W2.6 fix-sprint.

---

## MISSING FIXES (W2.6 Action Items)

### 1. 🚨 CRITICAL C-2: Subclass filtering was NOT implemented
**Context**: Codex noted that `SubscribeEventsRequest` lacks a way to filter by `Event-Subclass` (e.g., `osw.audit.*`), meaning audit subscribers currently receive all `CUSTOM` events.
**The Error**: In the W2.5 commit log, the agent confused **C-2** with **I-3** (prefix-wildcard limitations) and only updated the documentation, incorrectly claiming C-2 was fixed. The actual protobuf and filtering code was untouched.
**Action Required**:
- Add `repeated string subclass_globs = 6;` to `SubscribeEventsRequest` in `proto/open_switch/control/v1/control.proto`.
- Add `subclass_globs` to `osw::events::SubscriberFilter`.
- Implement field-tag-4 (subclass name) extraction in `src/events/subscribe/routing.cc` (`ExtractRoutingFields`).
- Update `Subscriber::MatchesFilter` to actually evaluate subclass globs (using the same prefix-wildcard logic as `event_names`).

### 2. ⚠️ IMPORTANT I-4: Busy-polling in `Module::Shutdown` remains
**Context**: `Module::Shutdown` (`src/core/module.cc`) still loops with `std::this_thread::sleep_for(std::chrono::milliseconds(10))` while waiting for `rings_->AllEmpty()`.
**Action Required**:
- Replace the 10ms busy-poll with a proper `std::condition_variable` wait or an atomic counter wait exposed by `RingSet` or `Ring`.

### 3. ⚠️ NIT N-4: Binary payload truncation in `BuildEnvelope`
**Context**: `src/events/envelope.cc` calls `env->set_body(body);`. Since `body` is passed as a `const char*`, protobuf implicitly uses `strlen()`, which will truncate any FS event bodies containing embedded `\0` (NUL) bytes.
**Action Required**:
- Check if FreeSWITCH exposes the body length (e.g., via a macro or `event->body_size`). If so, use `env->set_body(body, length)`. If not, clearly document the text-only assumption and the `strlen` limitation.

---

## Next Steps
Please spawn a W2.6 fix-sprint to implement the above three items, then commit the results to the `implementation/wave2-events` branch.
