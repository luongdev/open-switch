## Summary

<!-- 1-3 sentences: what + why. -->

## Related issues / specs

<!-- Link to issue. If this touches a spec, link the spec file under openspec/. -->

## Memory-management checklist

**This block is mandatory. Do not edit "N/A" without justification in PR body.**

- [ ] All `new` wrapped in smart pointers or owned by RAII class
- [ ] All `switch_core_session_locate` paired with `_rwunlock` (or RAII)
- [ ] All `switch_event_create*` either fired or wrapped in `osw::EventGuard`
- [ ] All `switch_xml_open_*` paired with `switch_xml_free`
- [ ] All `switch_core_media_bug_add` paired with `_remove` (or RAII)
- [ ] No `malloc/calloc/free` in new code (use FS pool or C++ RAII)
- [ ] gRPC completion-queue tags owned by `shared_ptr`
- [ ] gRPC reactors release call object exactly once
- [ ] All C-callable callbacks wrapped in `try { ... } catch (...)`
- [ ] Mutex usage via `lock_guard` / `unique_lock`

## CI signals

- [ ] `make build` clean
- [ ] `make test` passes
- [ ] ASAN+LSAN integration passes (CI green is necessary but not
      sufficient — confirm `==LeakSanitizer== detected 0 leaks` in logs)
- [ ] `clang-format` / `clang-tidy` clean

## Backwards compatibility

<!--
Does this change:
  - Modify the gRPC proto in a breaking way? → must use a new service
    version. See openspec/changes/core-module-v1/specs/control-api/spec.md.
  - Change event payload schemas? → must use reserved fields, never
    repurpose tag numbers.
  - Change config XML schema? → migration path documented?
-->

## Testing

<!-- How did you test this? Manual + automated. -->

## Risk assessment

<!-- What's the worst case if this PR has a latent bug? Crash FS? Leak per call? Mis-route events? -->
