## Summary
The W6 media plane implementation successfully delivers the baseline mechanics for TTS, STT, and voicebot duplex flows, aligning well with the module's modular design and stage-rank rules. However, there are critical architectural deviations regarding concurrency, sequence state, and memory lifetimes that must be corrected before proceeding to W7. Specifically, the `StreamClient` introduces a lock on the FreeSWITCH media thread, and handler teardown paths contain a use-after-free race condition.

## P1 â€” Architecture blockers
- **[CODEX-DOMAIN] `StreamClient` send queue uses a blocking lock instead of a lock-free ring**: `designs/media-bridge.md` mandates a "lock-free SPMC ring" to guarantee we never block the FS media thread on gRPC backpressure. In `src/media/stream_client.cc:162`, `SendAudio` locks `std::mutex ring_mu_`. If the writer thread is descheduled while holding the lock, the FS media thread will block, violating the core non-blocking requirement for audio callbacks.
- **`thread_local` sequence state in bug callback**: In `src/control/handlers/media_bug_callbacks.cc` (lines 106-107), `tl_seq` and `tl_ts` are defined as `static thread_local std::uint64_t`. Because FreeSWITCH reuses media threads across calls, sequence numbers and timestamps will be interleaved globally across entirely unrelated streams. This state must live per-stream (e.g., in a context struct).
- **[CODEX-DOMAIN] Use-After-Free race in `ActiveMediaStreams::TearDown`**: In `src/control/active_media_streams.cc` (lines 117-122), `s->write_ctx.reset()` is called *before* `s->bugs.clear()`. Because `bugs.clear()` is what actually triggers `switch_core_media_bug_remove_callback`, the FS media thread could execute `OswStreamingWriteReplace` concurrently after `write_ctx` is deleted, leading to a UAF. Bugs must be detached before their contexts are deleted.

## P2 â€” Important design gaps
- **Config Validation Coercion Side-effect**: In `src/core/config.cc`, `Validate()` checks `tts_underrun_policy` but returns `Ok()` even on unknown strings, relying on downstream components to default to `"silence"`. A validation pass should fail explicitly on operator typos to prevent silent misconfiguration.

## P3 â€” Spec hygiene
- **[SPEC] `media-bridge.md` claims SPMC for an SPSC ring**: The spec document mentions a "lock-free SPMC ring" in the `Read tap` section. Given there is one producer (the FS media thread callback) and one consumer (the gRPC writer thread), this is strictly an SPSC (Single-Producer, Single-Consumer) queue. The spec should be corrected to avoid confusion.
- **Missing `OswReadTapCtx` implementation**: The implementation brief `W6-track-C-handlers.md` instructed the use of `/*seq=*/NextSeq(client)` and `/*ts=*/CurrentTimestampSamples(client)` and mentioned `OswReadTapCtx`, but the code in `media_bug_callbacks.cc` passed `StreamClient*` directly and incorrectly used `thread_local` state.

## P4 â€” Minor consistency / style
- **Vector erase-remove idiom vs loop**: In `src/media/bug_manager.cc` `DetachInternal`, `std::remove` is used on a vector, which is correct. However, `RemoveForChannel` in `src/control/active_media_streams.cc` uses a manual `while(it != by_id_.end())` loop. While functionally correct for `unordered_map`, using `std::erase_if` (C++20) would be cleaner and more idiomatic across the codebase.

## Notes
- **Positive Observation**: The stage-rank enforcement in `MediaBugManager` (`src/media/bug_manager.cc`) is implemented extremely cleanly and adheres strictly to the canonical attach sequence defined in the architecture docs.
- **Orchestrator Risk**: Track C merged a blocking lock implementation for the ring buffer. Given the strictness of the "zero block on hot path" memory model, the orchestrator should ensure the lock-free implementation replaces the mutex in the fix sprint before proceeding to W7.