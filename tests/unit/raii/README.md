# `tests/unit/raii/` — RAII helper unit tests + FS-mock seam

## Why this directory has a README

The W1 contract (§"include/osw/raii", under "RAII helpers (mandatory)")
requires the agent to document the choice of FS-mock seam used by the
unit tests of the RAII helpers. Codex review will check for this file.

## The choice: header-only mock keyed on `OSW_TEST_FS_MOCK`

The RAII helpers in `include/osw/raii/{session_lock,event_guard,media_bug_lease,xml_node}.h`
wrap the FreeSWITCH C API. Unit tests cannot link against `<switch.h>`
because there is no FreeSWITCH process and no installed FS at unit-test
time. Three seam options were considered:

| Option | Pros | Cons |
|---|---|---|
| **A. gmock / gtest hand-rolled mocks** | Familiar | Pulls gmock into the prod path or forces shared headers to ifdef. Verbose; small payoff for 4 helpers. |
| **B. Function-pointer hooks via constructor injection** | Clean DI | Production callers would have to pass null hooks; or, the helpers would always carry a function-pointer pair of bytes per instance. Adds runtime cost + API surface. |
| **C. Header-only mock layer keyed on `-DOSW_TEST_FS_MOCK=1`** | Zero overhead in prod; the prod .so includes `<switch.h>` and calls real FS APIs. Tests build with the macro and link the mock translation unit. | Two paths through the same headers; the build system must keep them straight. |

**Chosen: Option C.** The RAII helpers `#include "osw/raii/fs_api.h"`
which decides at preprocess time:

```cpp
#if defined(OSW_TEST_FS_MOCK)
#include "osw/raii/fs_mock.h"  // forward-decls + mockable hooks
#else
#include <switch.h>             // real FreeSWITCH API
namespace osw::raii::fs { /* thin inline trampolines to switch_* */ }
#endif
```

Both branches expose the same `osw::raii::fs::*` symbol set. The RAII
helpers call those wrappers, NOT the bare `switch_*` symbols. Result:

- Production build: `mod_open_switch.so` includes nothing from
  `fs_mock.h`. The trampolines are inlined at -O1+; production
  performance equals direct-call performance.
- Unit-test build: `OSW_TEST_FS_MOCK=1` is set on the unit-test
  targets only (see `tests/unit/raii/CMakeLists.txt`). The mock
  forward-declares the opaque FS types and exposes counters that
  tests assert on.

## What the mock layer does NOT simulate

- It does NOT execute media bug callbacks. Callbacks are passed to
  `MediaBugAdd` as function pointers; the mock just stores them. The
  W4 (media plane) tests will exercise callback semantics through a
  different seam.
- It does NOT honor FS's RTLD_LOCAL loader semantics (FF-010). These
  are link-time concerns; CI's build job validates them via
  `nm -D --defined-only`.
- It does NOT cover the thread-id gate in
  `switch_core_media_bug_remove_callback` (FF-002). RAII helpers do
  not use that function — they use `switch_core_media_bug_remove`
  with the bug ptr we own.
- It does NOT mock `switch_xml_config_parse_module_settings`. The
  config-loader tests use the same macro seam but install hooks at a
  different layer (`osw/core/config_fs_api.h`); see
  `tests/unit/core/README.md` *(if/when added in W1.5+)*.

## How a test uses the seam

```cpp
#include "osw/raii/session_lock.h"
#include "osw/raii/fs_mock.h"   // brings in MockReset() + Mock()
#include <gtest/gtest.h>

class SessionLockTest : public ::testing::Test {
 protected:
    void SetUp() override { osw::raii::fs::MockReset(); }
};

TEST_F(SessionLockTest, ConstructionLocksAndDestructionUnlocks) {
    auto& m = osw::raii::fs::Mock();
    m.next_session = reinterpret_cast<switch_core_session_t*>(0xDEADBEEF);
    {
        osw::SessionLock lock("uuid-xyz");
        EXPECT_TRUE(static_cast<bool>(lock));
        EXPECT_EQ(m.session_locate_calls.load(), 1);
        EXPECT_EQ(m.session_rwunlock_calls.load(), 0);
    }
    EXPECT_EQ(m.session_rwunlock_calls.load(), 1);
}
```

## When NOT to use this seam

Future RAII helpers that wrap more complex FS interactions (e.g., a
bug-lease that needs to invoke the bug callback to test the
exception-safety wrapper) should NOT extend `fs_mock.h` to simulate
callback delivery. Instead, those tests should run against a small
FS-in-a-thread harness (W5 territory) or a dedicated callback
test-shim. Keep `fs_mock.h` to a single responsibility: counting
acquires + releases on FS C API entry points.

W5 will introduce the FS-in-a-thread harness; until then, helpers
needing callback delivery are deferred to W5.

## See also

- `openspec/changes/core-module-v1/designs/memory-management.md`
  §"RAII helpers" — the verbatim source of the helpers.
- `openspec/changes/core-module-v1/FREESWITCH-FACTS.md` — FF-015
  (`switch_xml_open_*` refcount), FF-016 (`switch_core_session_locate`
  read-lock contract), FF-011 (media bug start event), FF-007 (bug
  insertion order) — the FS contracts the helpers must respect.
- `include/osw/raii/fs_api.h` — the shim header.
- `include/osw/raii/fs_mock.h` — the mock implementation included
  under `OSW_TEST_FS_MOCK=1`.
