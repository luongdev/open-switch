## codex-W3 Review — W3 fix-sprint findings

*(Full review originally authored by Codex gpt-5.5 xhigh; P1-1 already
closed in PR #9 via `fix/mod-link-whole-archive`.)*

---

## Findings

| ID    | Severity | File / location                            | Summary                                      |
|-------|----------|--------------------------------------------|----------------------------------------------|
| P1-1  | P1       | `CMakeLists.txt`                           | Linker dead-code elim strips FS API registrations (fixed in PR #9) |
| P1-2  | P1       | `hangup_handler.cc:86-93`                  | Hangup reports FAILED_PRECONDITION on every real success |
| P1-3  | P1       | `hangup_many_handler.cc:60-95`             | HangupMany same FS-return-semantics bug      |
| P1-4  | P1       | `execute_handler.cc:60-71`                 | Execute allow-list permits Bridge/Transfer bypass |
| P1-5  | P1       | `set_variables_handler.cc:80-110`          | SetVariables reserved-name denylist missing  |
| P2-6  | P2       | `originate_handler.cc:78-110`              | Originate ovars event leak                   |
| P2-7  | P2       | `bridge_handler.cc:152`                    | BridgeResponse.bridged_uuid not set          |
| P2-8  | P2       | `execute_handler.cc:87-90`                 | Execute audit redaction too narrow           |
| P2-9  | P2       | `hangup_handler.cc:60-90`                  | Hangup variables not applied                 |

---

## Closeout

All 8 remaining findings (P1-2 through P2-9) closed in branch
`fix/w3-codex-findings` via 8 incremental commits:

| Commit   | Finding | Summary                                                          |
|----------|---------|------------------------------------------------------------------|
| f8809ff  | P1-2    | Pre-check `ChannelGetState` before `ChannelHangup` in Hangup     |
| aa5aca1  | P1-3    | Same pre-check fix in `HangupOne()` for HangupMany               |
| f926c3f  | P1-4    | Drop `transfer`/`bridge`/`play_and_get_digits` from Execute allow-list; add `set` args denylist guard |
| aadf094  | P1-5    | New `var_denylist.h/.cc` with `IsReservedVar()` + 13 reserved prefixes; SetVariables rejects reserved names atomically |
| c6d3cf3  | P2-6    | `OriginateOptions` retains ownership of `ovars_`; use `ovars_ptr()` borrow instead of `ReleaseOvars()` |
| 2981c3b  | P2-7    | `resp->set_bridged_uuid(b_uuid)` added on Bridge happy path      |
| 10c4a06  | P2-8    | Broaden redaction regex to `(\S*?(?:password|token|secret)\S*)=(\S+)` |
| 5561344  | P2-9    | Hangup iterates `req->variables()` and calls `ChannelSetVariable` before `ChannelHangup`; reserved-name denylist applied |

**New files:**
- `include/osw/control/var_denylist.h` — shared reserved-prefix predicate
- `src/control/var_denylist.cc` — implementation

**Tests added:** 17 new test cases across
`hangup_handler_test.cc`, `hangup_many_handler_test.cc`,
`execute_handler_test.cc`, `set_variables_handler_test.cc`,
`originate_handler_test.cc`, `bridge_handler_test.cc`.

P1-1 remains closed via PR #9 (separate branch/PR).
