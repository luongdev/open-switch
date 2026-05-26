# FreeSWITCH facts sheet (v1.10.12)

**Purpose.** Round 1 and round 2 of the Codex Phase 1 review both
exposed the same failure mode: spec text confidently asserting a
FreeSWITCH internal behaviour without first verifying it against
the actual `signalwire/freeswitch` source code. Round 3 institutes
this document as the single source of truth for "what FreeSWITCH
v1.10.12 actually does". Every FS-behaviour claim in `designs/*.md`
or `specs/*.md` MUST cite an FF-ID from this file. FS-behaviour
claims that do not cite an FF entry are not allowed.

**Version pin.** All citations are against tag `v1.10.12` of
`signalwire/freeswitch`, the version we target. If we ever bump the
FS version, every FF entry must be re-verified at the new tag and
this document updated. Silent FS-version drift is how round 1 / 2
mistakes happened.

**Format.** Each entry has:

- `FF-NNN` id.
- A plain-language claim.
- Source location `<file>:<line>` and a permalink to the
  `signalwire/freeswitch` repo at tag `v1.10.12`.
- A 5-20 line excerpt of the actual source, quoted verbatim.
- Design implications.

The line numbers refer to v1.10.12. If you patch FS locally, that
local diff is yours; this document tracks vanilla v1.10.12.

---

## FF-001 — Write-side bug processing is a SINGLE interleaved loop

**Claim.** On the write side of a channel, FreeSWITCH walks the
session's `bugs` linked list exactly once. For each bug it checks
`SMBF_WRITE_STREAM` first (buffering + invoking the bug's callback
with `SWITCH_ABC_TYPE_WRITE`), then `SMBF_WRITE_REPLACE` (which
may mutate `write_frame` for subsequent iterations of the same
loop). There is **no** separate "all WRITE_REPLACE bugs run, then
all WRITE_STREAM bugs run" pass. The two flag kinds are interleaved
per-bug in chain order.

**Source.** `src/switch_core_media.c:16096-16156` in
`switch_core_session_write_frame`.

**Permalink.**
<https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_core_media.c#L16096-L16156>

**Excerpt** (verbatim, lines 16096-16156):

```c
if (session->bugs) {
    switch_media_bug_t *bp;
    int prune = 0;

    switch_thread_rwlock_rdlock(session->bug_rwlock);
    for (bp = session->bugs; bp; bp = bp->next) {
        switch_bool_t ok = SWITCH_TRUE;

        if (!bp->ready) {
            continue;
        }

        if (switch_core_media_bug_test_flag(bp, SMBF_PAUSE) || (switch_channel_test_flag(session->channel, CF_PAUSE_BUGS) && !switch_core_media_bug_test_flag(bp, SMBF_NO_PAUSE))) {
            continue;
        }

        if (!switch_channel_test_flag(session->channel, CF_ANSWERED) && switch_core_media_bug_test_flag(bp, SMBF_ANSWER_REQ)) {
            continue;
        }

        if (switch_test_flag(bp, SMBF_PRUNE)) {
            prune++;
            continue;
        }

        if (switch_test_flag(bp, SMBF_WRITE_STREAM)) {
            switch_mutex_lock(bp->write_mutex);
            switch_buffer_write(bp->raw_write_buffer, write_frame->data, write_frame->datalen);
            switch_mutex_unlock(bp->write_mutex);

            if (bp->callback) {
                ok = bp->callback(bp, bp->user_data, SWITCH_ABC_TYPE_WRITE);
            }
        }

        if (switch_test_flag(bp, SMBF_WRITE_REPLACE)) {
            do_bugs = 0;
            if (bp->callback) {
                bp->write_replace_frame_in = write_frame;
                bp->write_replace_frame_out = write_frame;
                if ((ok = bp->callback(bp, bp->user_data, SWITCH_ABC_TYPE_WRITE_REPLACE)) == SWITCH_TRUE) {
                    write_frame = bp->write_replace_frame_out;
                }
            }
        }

        if (bp->stop_time && bp->stop_time <= switch_epoch_time_now(NULL)) {
            ok = SWITCH_FALSE;
        }


        if (ok == SWITCH_FALSE) {
            switch_set_flag(bp, SMBF_PRUNE);
            prune++;
        }
    }
    switch_thread_rwlock_unlock(session->bug_rwlock);
    if (prune) {
        switch_core_media_bug_prune(session);
    }
}
```

**Implications.**

A bug positioned earlier in the chain than a `WRITE_REPLACE` bug
will observe (via `SMBF_WRITE_STREAM`) the PRE-replace `write_frame`
because at that earlier iteration `write_frame` has not yet been
mutated. Only bugs positioned AFTER the `WRITE_REPLACE` bug in the
chain see the post-replace frame.

Concretely for our V1 module (TTS = `SMBF_WRITE_REPLACE`, recording
= `SMBF_WRITE_STREAM`):

- If the recording bug is added BEFORE the TTS bug, the chain is
  `[record, TTS]`. The record bug's `WRITE_STREAM` callback fires
  with the pre-injection (i.e., unmodified) frame. **Recording
  does NOT capture bot audio.**
- If the recording bug is added AFTER the TTS bug, the chain is
  `[TTS, record]`. The TTS bug's `WRITE_REPLACE` callback mutates
  `write_frame`, then the record bug's `WRITE_STREAM` callback
  fires with the post-injection (bot-audio) frame. **Recording
  captures bot audio.**

The outcome is **add-order-dependent**. There is no FS-side
"semantic" that makes WRITE_STREAM-after-WRITE_REPLACE
order-independent on the write side. (See FF-006 for the contrast
with the read side.)

A spec section that claims "WRITE_STREAM always sees post-injection
audio regardless of add order" is factually wrong.

---

## FF-002 — `switch_core_media_bug_remove_callback` is gated by `thread_id == switch_thread_self()` for `SMBF_THREAD_LOCK` bugs

**Claim.** `switch_core_media_bug_remove_callback(session, cb_fn)`
walks `session->bugs` and only removes entries whose
`thread_id == switch_thread_self()` (or whose `thread_id == 0`,
meaning unrestricted). For bugs added with `SMBF_THREAD_LOCK` the
`thread_id` is set to the attaching thread; an external caller
running on a different thread silently skips those bugs (the
function returns success, but the bug is NOT removed).

**Source.** `src/switch_core_media_bug.c:1435-1479`
(`switch_core_media_bug_remove_callback`), and lines 913-915 in
the same file (where `thread_id` is assigned at attach time).

**Permalinks.**

- Remove gate: <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_core_media_bug.c#L1435-L1479>
- Attach assignment: <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_core_media_bug.c#L913-L915>

**Excerpt — the gate (lines 1435-1479):**

```c
SWITCH_DECLARE(switch_status_t) switch_core_media_bug_remove_callback(switch_core_session_t *session, switch_media_bug_callback_t callback)
{
    switch_media_bug_t *cur = NULL, *bp = NULL, *last = NULL, *closed = NULL, *next = NULL;
    int total = 0;

    switch_thread_rwlock_wrlock(session->bug_rwlock);
    if (session->bugs) {
        bp = session->bugs;
        while (bp) {
            cur = bp;
            bp = bp->next;

            if ((!cur->thread_id || cur->thread_id == switch_thread_self()) && cur->ready && cur->callback == callback) {
                if (last) {
                    last->next = cur->next;
                } else {
                    session->bugs = cur->next;
                }
                if (switch_core_media_bug_close(&cur, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
                    total++;
                }

                cur->next = closed;
                closed = cur;

            } else {
                last = cur;
            }
        }
    }
    switch_thread_rwlock_unlock(session->bug_rwlock);
    /* ... */
    return total ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}
```

**Excerpt — the assignment (lines 913-915):**

```c
if ((bug->flags & SMBF_THREAD_LOCK)) {
    bug->thread_id = switch_thread_self();
}
```

**Implications.**

The eavesdrop bug created by `switch_ivr_eavesdrop_session`
unconditionally ORs in `SMBF_THREAD_LOCK` (see FF-003 excerpt).
Therefore `eavesdrop` bugs have `thread_id = switch_thread_self()`
of the attaching thread — typically the eavesdropper channel's
dialplan-execution thread. Any other thread (FS event dispatch
thread, a custom worker thread, our gRPC server thread) cannot
remove the bug via `switch_core_media_bug_remove_callback`. The
function will silently succeed at the API level (no error) but the
bug remains attached.

A round-2 spec proposal that called
`switch_core_media_bug_remove_callback(sess, eavesdrop_callback)`
from an FS event dispatch thread to "remove the eavesdrop bug
when policy=deny" is a no-op against any eavesdrop bug FS actually
attaches.

---

## FF-003 — `eavesdrop_callback` has static (file-scope) linkage

**Claim.** The bug callback function used by FS's native eavesdrop
implementation has C `static` linkage. It is not exported from
the FreeSWITCH binary and cannot be referenced by symbol name from
an out-of-tree module at link time or via `dlsym` at run time.

**Source.** `src/switch_ivr_async.c:2000`.

**Permalink.**
<https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_ivr_async.c#L2000-L2020>

**Excerpt** (lines 1998-2020):

```c
}

static switch_bool_t eavesdrop_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    struct eavesdrop_pvt *ep = (struct eavesdrop_pvt *) user_data;
    uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
    switch_frame_t frame = { 0 };
    switch_core_session_t *session = switch_core_media_bug_get_session(bug);
    switch_channel_t *e_channel = switch_core_session_get_channel(ep->eavesdropper);
    int show_spy = 0;
    switch_frame_t *nframe = NULL;

    frame.data = data;
    frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

    show_spy = switch_core_media_bug_test_flag(bug, SMBF_SPY_VIDEO_STREAM) || switch_core_media_bug_test_flag(bug, SMBF_SPY_VIDEO_STREAM_BLEG);

    if (show_spy) {
        if (!ep->set_decoded_read) {
            ep->set_decoded_read = 1;
```

For context, the same file attaches the bug at line 2503-2506 with
`SMBF_THREAD_LOCK`:

```c
if (switch_core_media_bug_add(tsession, "eavesdrop", uuid,
                              eavesdrop_callback, ep, 0,
                              read_flags | write_flags | SMBF_READ_PING | SMBF_THREAD_LOCK | SMBF_NO_PAUSE | stereo_flag,
                              &bug) != SWITCH_STATUS_SUCCESS) {
```

**Implications.**

Combined with FF-002: even ignoring the thread-id gate, an external
module cannot call
`switch_core_media_bug_remove_callback(session, &eavesdrop_callback)`
because `eavesdrop_callback` is not a visible symbol. The compiler
cannot resolve it; `dlsym(RTLD_DEFAULT, "eavesdrop_callback")`
returns NULL.

Any spec proposal that names `eavesdrop_callback` as a function
pointer parameter is unimplementable against vanilla FS v1.10.12.
The honest options are: (a) call the function `osw_eavesdrop_callback`
that lives in OUR module (only removes bugs WE attached), (b) ship
a patched FS that exports the symbol (V1.5+ scope at earliest), or
(c) abandon the remove-by-callback approach for FS-native eavesdrop
and choose another control mechanism.

---

## FF-004 — Event dispatch is a pool of threads (multi-producer for any event-bind callback)

**Claim.** FreeSWITCH's event facility services the dispatch queue
with **multiple threads**, not a single thread. The pool starts
at one thread and grows on demand up to `MAX_DISPATCH_VAL = 64`.
Any registered subscriber (via `switch_event_bind`) may have its
callback invoked concurrently from any of those threads.

**Source.** `src/switch_event.c:82-95` (constants) and
`src/switch_event.c:367-389` (the grow-on-demand path).

**Permalink.**
<https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_event.c#L82-L95>

**Excerpt — constants (lines 82-95):**

```c
#define MAX_DISPATCH_VAL 64
static unsigned int MAX_DISPATCH = MAX_DISPATCH_VAL;
static unsigned int SOFT_MAX_DISPATCH = 0;
static char guess_ip_v4[80] = "";
static char guess_ip_v6[80] = "";
static switch_event_node_t *EVENT_NODES[SWITCH_EVENT_ALL + 1] = { NULL };
static switch_thread_rwlock_t *RWLOCK = NULL;
static switch_mutex_t *BLOCK = NULL;
static switch_mutex_t *POOL_LOCK = NULL;
static switch_memory_pool_t *RUNTIME_POOL = NULL;
static switch_memory_pool_t *THRUNTIME_POOL = NULL;
static switch_thread_t *EVENT_DISPATCH_QUEUE_THREADS[MAX_DISPATCH_VAL] = { 0 };
static uint8_t EVENT_DISPATCH_QUEUE_RUNNING[MAX_DISPATCH_VAL] = { 0 };
```

**Excerpt — grow path (lines 367-389):**

```c
while (event) {
    int launch = 0;

    switch_mutex_lock(EVENT_QUEUE_MUTEX);

    if (!PENDING && switch_queue_size(EVENT_DISPATCH_QUEUE) > (unsigned int)(DISPATCH_QUEUE_LEN * DISPATCH_THREAD_COUNT)) {
        if (SOFT_MAX_DISPATCH + 1 < MAX_DISPATCH) {
            launch++;
            PENDING++;
        }
    }

    switch_mutex_unlock(EVENT_QUEUE_MUTEX);

    if (launch) {
        if (SOFT_MAX_DISPATCH + 1 < MAX_DISPATCH) {
            switch_event_launch_dispatch_threads(SOFT_MAX_DISPATCH + 1);
        }

        switch_mutex_lock(EVENT_QUEUE_MUTEX);
        PENDING--;
        switch_mutex_unlock(EVENT_QUEUE_MUTEX);
    }
}
```

**Implications.**

The producer side of any module-internal ring fed by an
`switch_event_bind` callback is **multi-producer**. SPSC
(single-producer, single-consumer) rings are unsound: multiple
dispatch threads may concurrently call our callback and enqueue.

The correct choice is MPSC (multi-producer, single-consumer) or a
per-producer-thread sharded design. Per-tier `std::atomic<uint64_t>`
sequence allocation is fine (atomic fetch_add is wait-free), but
the ring's enqueue itself must be MPSC.

The "all events for the same channel pass through the same FS event
thread" assumption is also wrong. The dispatch queue is shared
across all events; any of the up-to-64 dispatch threads can pick
up the next event regardless of which channel it pertains to.
Subscribers needing per-channel ordering must reconstruct it from
the envelope's `emitted_at` and `seq` (per-tier) fields.

---

## FF-005 — `CHANNEL_CALLSTATE` events fire only on call-state transitions, NOT on bug attach

**Claim.** The `CHANNEL_CALLSTATE` event is created and fired
exclusively from `switch_channel_perform_set_callstate` (alias
`switch_channel_set_callstate`), which is invoked from a small,
enumerable set of call-state transition sites. The act of adding
a media bug to a session does NOT call
`switch_channel_set_callstate`. Therefore `CHANNEL_CALLSTATE`
**does not fire** when a media bug is attached.

**Source.** Definition: `src/switch_channel.c:283-307`. Call
sites: `src/switch_channel.c` and `src/switch_core_state_machine.c`
(enumerated below); `src/switch_core_media_bug.c` contains zero
references.

**Permalink (definition).**
<https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_channel.c#L283-L307>

**Excerpt — the fire site (lines 283-307):**

```c
SWITCH_DECLARE(void) switch_channel_perform_set_callstate(switch_channel_t *channel, switch_channel_callstate_t callstate,
                                                          const char *file, const char *func, int line)
{
    switch_event_t *event;
    switch_channel_callstate_t o_callstate = channel->callstate;

    if (o_callstate == callstate || o_callstate == CCS_HANGUP) return;

    channel->callstate = callstate;
    if (channel->device_node) {
        channel->device_node->callstate = callstate;
    }
    switch_log_printf(SWITCH_CHANNEL_ID_LOG, file, func, line, switch_channel_get_uuid(channel), SWITCH_LOG_DEBUG,
                      "(%s) Callstate Change %s -> %s\n", channel->name,
                      switch_channel_callstate2str(o_callstate), switch_channel_callstate2str(callstate));

    switch_channel_check_device_state(channel, channel->callstate);

    if (switch_event_create(&event, SWITCH_EVENT_CHANNEL_CALLSTATE) == SWITCH_STATUS_SUCCESS) {
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Original-Channel-Call-State", switch_channel_callstate2str(o_callstate));
        switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Channel-Call-State-Number", "%d", callstate);
        switch_channel_event_set_data(channel, event);
        switch_event_fire(&event);
    }
}
```

**Call sites (`grep -rn switch_channel_set_callstate src/`):**

```
src/switch_core_state_machine.c:429 — CCS_ACTIVE in soft_execute()
src/switch_core_state_machine.c:732 — CCS_DOWN in transmit_state()
src/switch_core_state_machine.c:844 — CCS_HANGUP in transmit_state()
src/switch_channel.c:1973            — CCS_RING_WAIT in switch_channel_perform_mark_pre_answered
src/switch_channel.c:1994            — CCS_HELD
src/switch_channel.c:2179            — CCS_UNHELD
src/switch_channel.c:2190            — CCS_ACTIVE
src/switch_channel.c:2197            — CCS_ACTIVE
src/switch_channel.c:2399            — CCS_RINGING
src/switch_channel.c:2401            — CCS_ACTIVE
src/switch_channel.c:2403            — CCS_EARLY
src/switch_channel.c:3513            — CCS_RINGING
src/switch_channel.c:3585            — CCS_EARLY
src/switch_channel.c:3912            — CCS_ACTIVE
src/switch_core_io.c:941             — CCS_ACTIVE (DTMF early-media trigger)
```

`src/switch_core_media_bug.c` contains zero call sites.

**Implications.**

A spec proposal that subscribes to `CHANNEL_CALLSTATE` and expects
to be notified when a media bug is attached **does not work**. The
event fires on call-state transitions only. Bug attachment is
silent from the event-bus perspective.

For a "detect bug-attach on a bot-marked target session" mechanism,
the relevant event is `SWITCH_EVENT_MEDIA_BUG_START` (see FF-011)
or one must implement another polling/inspection mechanism.

---

## FF-006 — Read-side bug processing IS two passes (`READ_REPLACE` then `READ_STREAM`)

**Claim.** On the read side of a channel, FreeSWITCH does walk
the bug list twice: a first traversal handling `SMBF_READ_REPLACE`
(which may mutate `read_frame`), and a second traversal handling
`SMBF_READ_STREAM` (read-only tap). This is the symmetric
counterpart to FF-001 — but unlike the write side, the read side
IS in two passes.

**Source.** `src/switch_core_io.c:646-756`.

**Permalink.**
<https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_core_io.c#L646-L756>

**Excerpt** (lines 646-756, eliding pause/answer/bridge skip logic
between the two loops):

```c
if (session->bugs) {
    switch_media_bug_t *bp;
    switch_bool_t ok = SWITCH_TRUE;
    int prune = 0;
    switch_thread_rwlock_rdlock(session->bug_rwlock);

    for (bp = session->bugs; bp; bp = bp->next) {
        ok = SWITCH_TRUE;
        /* ... pause / answer / bridge / prune skip checks ... */

        if (switch_test_flag(bp, SMBF_READ_REPLACE)) {
            do_bugs = 0;
            if (bp->callback) {
                bp->read_replace_frame_in = read_frame;
                bp->read_replace_frame_out = read_frame;
                bp->read_demux_frame = NULL;
                if ((ok = bp->callback(bp, bp->user_data, SWITCH_ABC_TYPE_READ_REPLACE)) == SWITCH_TRUE) {
                    read_frame = bp->read_replace_frame_out;
                }
            }
        }
        /* ... stop_time check ... */
    }
    switch_thread_rwlock_unlock(session->bug_rwlock);
    if (prune) { switch_core_media_bug_prune(session); }
}

if (session->bugs) {
    switch_media_bug_t *bp;
    switch_bool_t ok = SWITCH_TRUE;
    int prune = 0;
    switch_thread_rwlock_rdlock(session->bug_rwlock);

    for (bp = session->bugs; bp; bp = bp->next) {
        ok = SWITCH_TRUE;
        /* ... pause / answer / bridge / prune skip checks ... */

        if (bp->ready && switch_test_flag(bp, SMBF_READ_STREAM)) {
            switch_mutex_lock(bp->read_mutex);
            /* read_demux_frame branch handling ... */
            switch_buffer_write(bp->raw_read_buffer, read_frame->data, read_frame->datalen);
            if (bp->callback) {
                ok = bp->callback(bp, bp->user_data, SWITCH_ABC_TYPE_READ);
            }
            switch_mutex_unlock(bp->read_mutex);
        }
        /* ... stop_time check ... */
    }
    switch_thread_rwlock_unlock(session->bug_rwlock);
    if (prune) { switch_core_media_bug_prune(session); }
}
```

**Implications.**

`READ_STREAM` callbacks observe the post-`READ_REPLACE` value of
`read_frame` regardless of chain position. The "post-injection
tap is order-independent" mental model is true for the READ side,
but the write side (FF-001) does NOT have that property.

The V1 module currently does not use `SMBF_READ_REPLACE`, so this
distinction does not affect our internal flow — but documenting it
prevents the wrong analogy from being drawn (i.e., "write side
must be like read side").

---

## FF-007 — Bug insertion is head-on-`SMBF_FIRST`, tail otherwise

**Claim.** `switch_core_media_bug_add` inserts the new bug at the
head of `session->bugs` if `SMBF_FIRST` is set (or if the chain is
empty); otherwise it appends to the tail. There is no numeric
priority, no priority byte, no out-of-order insertion mechanism.

**Source.** `src/switch_core_media_bug.c:977-999`.

**Permalink.**
<https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_core_media_bug.c#L977-L999>

**Excerpt** (lines 977-999):

```c
switch_thread_rwlock_wrlock(session->bug_rwlock);

if (!session->bugs) {
    session->bugs = bug;
    added = 1;
}

if (!added && switch_test_flag(bug, SMBF_FIRST)) {
    bug->next = session->bugs;
    session->bugs = bug;
    added = 1;
}

for(bp = session->bugs; bp; bp = bp->next) {
    if (bp->ready && !switch_test_flag(bp, SMBF_TAP_NATIVE_READ) && !switch_test_flag(bp, SMBF_TAP_NATIVE_WRITE)) {
        tap_only = 0;
    }

    if (!added && !bp->next) {
        bp->next = bug;
        break;
    }
}

switch_thread_rwlock_unlock(session->bug_rwlock);
```

**Implications.**

The chain order is determined by attach order plus the `SMBF_FIRST`
flag — exactly two knobs. Any "I want this bug to run before another
specific bug" requirement must either:

- Add it first (relying on operator dialplan order), or
- Add it with `SMBF_FIRST` (puts it at the head, but this is a
  blunt all-or-nothing instrument; two `SMBF_FIRST` bugs added in
  succession both end up at the head, the latter ending up FIRST
  of the two).

Our `MediaBugManager`'s stage-rank coordinator (see
`designs/media-bridge.md`) is the only place to enforce
finer-grained ordering, and only for bugs WE attach. FS-native
bugs (`record_session`, `mod_dptools::eavesdrop`, operator
`uuid_bug` calls) are not routed through our manager and place
themselves at the tail (or head if they use `SMBF_FIRST`).

---

## FF-008 — `switch_core_media_bug_count` filters by `function` name string

**Claim.** `switch_core_media_bug_count(session, function)` walks
`session->bugs` and counts entries whose `bp->function` string is
`strcmp`-equal to the provided `function` argument, skipping bugs
flagged `SMBF_PRUNE` or `SMBF_LOCK`.

**Source.** `src/switch_core_media_bug.c:1135-1151`.

**Permalink.**
<https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_core_media_bug.c#L1135-L1151>

**Excerpt** (lines 1135-1151):

```c
SWITCH_DECLARE(uint32_t) switch_core_media_bug_count(switch_core_session_t *orig_session, const char *function)
{
    switch_media_bug_t *bp;
    uint32_t x = 0;

    if (orig_session->bugs) {
        switch_thread_rwlock_rdlock(orig_session->bug_rwlock);
        for (bp = orig_session->bugs; bp; bp = bp->next) {
            if (!switch_test_flag(bp, SMBF_PRUNE) && !switch_test_flag(bp, SMBF_LOCK) && !strcmp(bp->function, function)) {
                x++;
            }
        }
        switch_thread_rwlock_unlock(orig_session->bug_rwlock);
    }

    return x;
}
```

**Implications.**

`switch_core_media_bug_count(sess, "eavesdrop")` is a thread-safe
detector: it acquires `bug_rwlock` itself and returns a count.
This is usable from any thread including our event-bind callback
thread. It does NOT remove the bug; it only counts.

This is the building block for a **detection-only** Layer 2 of the
eavesdrop policy: a periodic / event-triggered probe that surfaces
the presence of an eavesdrop bug without trying to remove it. The
audit / forensic value is real; the real-time policy enforcement
is not (FF-002 + FF-003 explain why removal can't work for FS-native
eavesdrop bugs from outside their attach thread).

---

## FF-009 — Module entry-point macros (`SWITCH_MODULE_DEFINITION` et al.)

**Claim.** A FreeSWITCH loadable module exports three things:

1. `<modname>_module_interface` — a global data symbol of type
   `switch_loadable_module_function_table_t`.
2. The `load` function whose pointer that struct holds.
3. The `shutdown` function whose pointer that struct holds.

The `SWITCH_MODULE_DEFINITION(name, load, shutdown, runtime)`
macro emits #1. `SWITCH_MODULE_LOAD_FUNCTION(name)` and
`SWITCH_MODULE_SHUTDOWN_FUNCTION(name)` declare #2/#3 with the
required signatures. On Linux, `SWITCH_MOD_DECLARE_DATA` expands to
`__attribute__((visibility("default")))` so the module's interface
symbol is visible to FreeSWITCH's `dlsym`.

**Source.**

- `src/include/switch_types.h:2607-2647` (macros).
- `src/include/switch_platform.h:184-202` (visibility attributes on
  the GCC/Linux path; note the `SWITCH_API_VISIBILITY` gate at line
  186 — without it the macros expand to empty and the module
  interface data symbol is hidden by `-fvisibility=hidden`).

**Permalinks.**

- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/include/switch_types.h#L2607-L2647>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/include/switch_platform.h#L184-L200>

**Excerpt — `switch_types.h:2600-2647`:**

```c
#define SWITCH_API_VERSION 5
#define SWITCH_MODULE_LOAD_ARGS (switch_loadable_module_interface_t **module_interface, switch_memory_pool_t *pool)
#define SWITCH_MODULE_RUNTIME_ARGS (void)
#define SWITCH_MODULE_SHUTDOWN_ARGS (void)
typedef switch_status_t (*switch_module_load_t) SWITCH_MODULE_LOAD_ARGS;
typedef switch_status_t (*switch_module_runtime_t) SWITCH_MODULE_RUNTIME_ARGS;
typedef switch_status_t (*switch_module_shutdown_t) SWITCH_MODULE_SHUTDOWN_ARGS;
#define SWITCH_MODULE_LOAD_FUNCTION(name) switch_status_t name SWITCH_MODULE_LOAD_ARGS
#define SWITCH_MODULE_RUNTIME_FUNCTION(name) switch_status_t name SWITCH_MODULE_RUNTIME_ARGS
#define SWITCH_MODULE_SHUTDOWN_FUNCTION(name) switch_status_t name SWITCH_MODULE_SHUTDOWN_ARGS

/* ... */

#define SWITCH_MODULE_DEFINITION_EX(name, load, shutdown, runtime, flags)                   \
static const char modname[] =  #name ;                                                      \
SWITCH_MOD_DECLARE_DATA switch_loadable_module_function_table_t name##_module_interface = { \
    SWITCH_API_VERSION,                                                                     \
    load,                                                                                   \
    shutdown,                                                                               \
    runtime,                                                                                \
    flags                                                                                   \
}

#define SWITCH_MODULE_DEFINITION(name, load, shutdown, runtime)                             \
        SWITCH_MODULE_DEFINITION_EX(name, load, shutdown, runtime, SMODF_NONE)
```

**Excerpt — `switch_platform.h:184-204` (the GCC/Linux branch with
the `SWITCH_API_VISIBILITY` gate):**

```c
#else //not win32
#define O_BINARY 0
#if (defined(__GNUC__) || defined(__SUNPRO_CC) || defined (__SUNPRO_C)) && defined(SWITCH_API_VISIBILITY)
#define SWITCH_DECLARE(type)        __attribute__((visibility("default"))) type
#define SWITCH_DECLARE_NONSTD(type) __attribute__((visibility("default"))) type
#define SWITCH_DECLARE_DATA         __attribute__((visibility("default")))
#define SWITCH_MOD_DECLARE(type)    __attribute__((visibility("default"))) type
#define SWITCH_MOD_DECLARE_NONSTD(type) __attribute__((visibility("default"))) type
#define SWITCH_MOD_DECLARE_DATA     __attribute__((visibility("default")))
#define SWITCH_DECLARE_CLASS        __attribute__((visibility("default")))
#else
#define SWITCH_DECLARE(type)        type
#define SWITCH_DECLARE_NONSTD(type) type
#define SWITCH_DECLARE_DATA
#define SWITCH_MOD_DECLARE(type)    type
#define SWITCH_MOD_DECLARE_NONSTD(type) type
#define SWITCH_MOD_DECLARE_DATA
#define SWITCH_DECLARE_CLASS
#endif
#define SWITCH_THREAD_FUNC
#endif
```

**Implications.**

For the Phase-1 stub module:

- Use `SWITCH_MODULE_LOAD_FUNCTION(mod_open_switch_load)` and
  `SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_open_switch_shutdown)`
  declarations.
- Emit `SWITCH_MODULE_DEFINITION(mod_open_switch, mod_open_switch_load, mod_open_switch_shutdown, NULL)`.
- Compile with `-fvisibility=hidden` for our own internal symbols
  (preventing gRPC/protobuf statics from leaking); the
  `SWITCH_MOD_DECLARE_DATA` macro overrides visibility for the
  loadable-module interface symbol so FS can still find it.
- The runtime arg `NULL` is acceptable; a module without a
  per-tick runtime function passes NULL there.
- **MUST also define `SWITCH_API_VISIBILITY`** (via
  `-DSWITCH_API_VISIBILITY=1` or
  `target_compile_definitions(... SWITCH_API_VISIBILITY=1)`)
  for the visibility-default attribute to be emitted on the module
  interface symbol. Without it, `SWITCH_MOD_DECLARE_DATA` expands
  to empty (line 200 of switch_platform.h) and our global
  `-fvisibility=hidden` will hide
  `mod_open_switch_module_interface`. The result is that FS's
  loader cannot find the symbol and `load mod_open_switch` fails.
  Verified empirically: building with `-fvisibility=hidden` and
  WITHOUT `SWITCH_API_VISIBILITY`, `nm -D --defined-only` shows
  the interface symbol as local (`d`); WITH the define it becomes
  global (`D`).

A minimal stub returning `SWITCH_STATUS_SUCCESS` from both
load/shutdown is sufficient for FS to `load mod_open_switch`
without crashing. It is the seed for Phase-2 implementation.

---

## FF-010 — FS loads modules with `RTLD_LOCAL` unless `SMODF_GLOBAL_SYMBOLS` is set

**Claim.** `switch_loadable_module.c` calls `switch_dso_open(path, load_global=FALSE, ...)` by default. The wrapper passes that into `dlopen` with `RTLD_NOW | RTLD_LOCAL`. A module's symbols are scoped to its `.so` namespace and do not interfere with other modules. Only modules that explicitly set `SMODF_GLOBAL_SYMBOLS` in their interface flags re-open themselves with `RTLD_GLOBAL`.

**Source.**

- `src/switch_dso.c:101-109` (the wrapper).
- `src/switch_loadable_module.c:1660-1708` (the loader path).

**Permalinks.**

- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_dso.c#L101-L120>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_loadable_module.c#L1660-L1708>

**Excerpt — `switch_dso.c:101-119`:**

```c
switch_dso_lib_t switch_dso_open(const char *path, int global, char **err)
{
    void *lib;

    if (global) {
        lib = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    } else {
        lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }

    if (lib == NULL) {
        const char *dlerr = dlerror();
        /* Work around broken uclibc returning NULL on both dlopen() and dlerror() */
        if (dlerr) {
            *err = strdup(dlerr);
        } else {
            *err = strdup("Unknown error");
        }
    }
```

**Excerpt — `switch_loadable_module.c:1701-1708`:**

```c
if (!load_global && interface_struct_handle && switch_test_flag(interface_struct_handle, SMODF_GLOBAL_SYMBOLS)) {
    load_global = SWITCH_TRUE;
    switch_dso_destroy(&dso);
    interface_struct_handle = NULL;
    dso = switch_dso_open(path, load_global, &derr);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Loading module with global namespace at request of module\n");
    continue;
}
```

**Implications.**

- Our module ships with `SMODF_NONE` (the default flags in
  `SWITCH_MODULE_DEFINITION`), so it gets `RTLD_LOCAL`.
- gRPC + protobuf + abseil symbols statically linked into our `.so`
  are NOT exported to other modules. If another C++-using FS module
  (e.g., `mod_grpc`) links a different gRPC version, both happily
  coexist with no symbol collision.
- The CMakeLists already pairs this with `-fvisibility=hidden` and
  `-Wl,--exclude-libs,ALL` to belt-and-braces the hiding.

**The downside for `std::set_terminate` etc.:** `RTLD_LOCAL` means
our `.so`'s static initializers run in our private namespace, but
they may still mutate process-global state (the libstdc++
terminate handler is a single per-process slot — regardless of
whether the call site is `RTLD_LOCAL` or `RTLD_GLOBAL`). So
`std::set_terminate` is **process-wide**, not module-scoped. See
the design implication discussed in `architecture.md` §"Module
crash but FS survives".

---

## FF-011 — `SWITCH_EVENT_MEDIA_BUG_START` fires on every bug attach

**Claim.** `switch_core_media_bug_add` emits a
`SWITCH_EVENT_MEDIA_BUG_START` event after the bug is added to
`session->bugs`. The event carries `Media-Bug-Function` (the
function name string passed to `switch_core_media_bug_add`) and
`Media-Bug-Target` headers, plus the standard channel-data
headers (Unique-ID, etc.) via `switch_channel_event_set_data`.
There is a symmetric `SWITCH_EVENT_MEDIA_BUG_STOP` event in
`switch_core_media_bug_close`.

**Source.**

- Start fire: `src/switch_core_media_bug.c:1014-1019`.
- Stop fire: `src/switch_core_media_bug.c:84-88`.
- Event constants: `src/include/switch_types.h:2164-2165`.

**Permalink (start fire).**
<https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_core_media_bug.c#L1014-L1019>

**Excerpt — start fire (lines 1014-1019):**

```c
if (switch_event_create(&event, SWITCH_EVENT_MEDIA_BUG_START) == SWITCH_STATUS_SUCCESS) {
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Media-Bug-Function", "%s", bug->function);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Media-Bug-Target", "%s", bug->target);
    switch_channel_event_set_data(session->channel, event);
    switch_event_fire(&event);
}
```

**Excerpt — event enum (lines 2161-2167 of `switch_types.h`):**

```c
SWITCH_EVENT_CALL_UPDATE,
SWITCH_EVENT_FAILURE,
SWITCH_EVENT_SOCKET_DATA,
SWITCH_EVENT_MEDIA_BUG_START,
SWITCH_EVENT_MEDIA_BUG_STOP,
SWITCH_EVENT_CONFERENCE_DATA_QUERY,
SWITCH_EVENT_CONFERENCE_DATA,
```

**Implications.**

A module that subscribes to `SWITCH_EVENT_MEDIA_BUG_START` via
`switch_event_bind` IS notified when any bug is attached to any
channel (including FS-native eavesdrop). The handler can read
`Media-Bug-Function` to identify the bug type and `Unique-ID` to
identify the affected channel. This is the correct hook for an
"eavesdrop attached to a bot-marked session" detector.

Compared to FF-005's `CHANNEL_CALLSTATE`, this event:

- **Does** fire on bug attach (the round-2 spec required a signal
  that fires on attach; this is it).
- Fires per attach (no rate-limit). For a busy host attaching many
  bugs (e.g., bulk `record_session` activation), the handler must
  be cheap.
- Does NOT itself remove the bug. Removal is subject to FF-002
  (thread-id gate) + FF-003 (static `eavesdrop_callback`); the
  detector path can audit, but cannot reliably remove the bug from
  outside its attaching thread.

This unblocks Layer 2 of the eavesdrop policy as a **detection +
audit** mechanism: subscribe to `MEDIA_BUG_START`, match
`Media-Bug-Function == "eavesdrop"` on a session whose
`osw_bot_session` variable is set, emit a Tier-1 audit event.
Real-time prevention still requires Layer 1 (`osw_eavesdrop` app
+ raw `eavesdrop` ACL deprecation).

---

## FF-012 — `switch_log_printf` queues to a single async log thread (thread-safe; printf-style)

**Claim.** `switch_log_printf(channel, file, func, line, userdata, level, fmt, ...)`
is a thread-safe printf-style logging entry point. It formats the
message (via `switch_vasprintf`) and either writes to stderr/console
directly when the async log subsystem isn't running, OR pushes a
`switch_log_node_t` onto the global `LOG_QUEUE` via
`switch_queue_trypush` (`do_mods` branch). The async log thread later
drains the queue and calls each registered backend (mod_console,
mod_logfile, etc.). All locking is done by FS internally — callers
just call `switch_log_printf` from any thread without taking any
extra lock.

The function declares format-string checking via `PRINTF_FUNCTION(7, 8)`
so the compiler validates arguments at the call site (with `-Wformat`).

**Source.**

- Declaration: `src/include/switch_log.h:142-145`.
- Definition: `src/switch_log.c:538-546`.
- Async queue push path: `src/switch_log.c:706-735` (inside
  `switch_log_meta_vprintf`, the worker that `switch_log_printf` forwards
  to).

**Permalinks.**

- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/include/switch_log.h#L142-L145>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_log.c#L538-L546>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_log.c#L706-L735>

**Excerpt — declaration (lines 142-145):**

```c
SWITCH_DECLARE(void) switch_log_printf(_In_ switch_text_channel_t channel, _In_z_ const char *file,
                                       _In_z_ const char *func, _In_ int line,
                                       _In_opt_z_ const char *userdata, _In_ switch_log_level_t level,
                                       _In_z_ _Printf_format_string_ const char *fmt, ...) PRINTF_FUNCTION(7, 8);
```

**Excerpt — definition (lines 538-546):**

```c
SWITCH_DECLARE(void) switch_log_printf(switch_text_channel_t channel, const char *file, const char *func, int line,
                                       const char *userdata, switch_log_level_t level, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    switch_log_meta_vprintf(channel, file, func, line, userdata, level, NULL, fmt, ap);
    va_end(ap);
}
```

**Excerpt — async push (lines 706-735, eliding allocation details):**

```c
if (do_mods && level <= MAX_LEVEL) {
    switch_log_node_t *node = switch_log_node_alloc();

    node->data = data;
    /* ... fill node fields ... */

    if (switch_queue_trypush(LOG_QUEUE, node) != SWITCH_STATUS_SUCCESS) {
        switch_log_node_free(&node);
    }
}
```

**Implications.**

- `switch_log_printf` is safe to call from any thread inside our
  module — including the gRPC server thread, our event consumer
  threads, the FS event dispatch threads (FF-004), and the module's
  load/shutdown entry points.
- It is **not** signal-safe (it allocates internally via
  `switch_log_node_alloc` + `malloc`). A SIGSEGV handler must NOT
  call `switch_log_printf`; use `write(STDERR_FILENO, ...)` instead
  (see `architecture.md` §"Terminate-handler chaining").
- The variadic signature uses a printf format string; bare
  user-controlled strings MUST go through `"%s"` to avoid format-string
  bugs. Our `osw::log::*` wrappers enforce this — handlers never pass
  user data as the format argument.
- The function returns void: there is no way to detect a dropped log
  record. CI's `clang-tidy` and `bugprone-*` should warn on bare
  `switch_log_printf("%s", ...)` mistakes; our `osw::log` wrappers
  remove that risk at the call sites.
- The `userdata` parameter is the channel-specific userdata pointer
  (e.g., a session pointer for `SWITCH_CHANNEL_ID_SESSION`); for
  `SWITCH_CHANNEL_ID_LOG` it is ignored. Our `osw::log::*` wrappers
  always pass `NULL` here.

**Used by W1 code:**

- `src/observability/log.cc` — `osw::log::{trace,debug,info,warn,error,critical}`
  forward to a `SinkFn` function-pointer slot. The slot defaults to a
  do-nothing `NullSink` at static-init time and is swapped to the
  production sink by `osw::log::InstallDefaultSinkForModule()` early
  in `mod_open_switch_load`.
- `src/observability/log_default_sink.cc` — `DefaultSink` is the
  production sink. It formats the level/subsystem/traceparent/message
  into a single line then calls
  `switch_log_printf(SWITCH_CHANNEL_LOG, "mod_open_switch",
  "osw_log_emit", 0, nullptr, MapLevel(level), "%s\n", line)`. Note
  the literal strings for the `file`/`func`/`line` arguments — these
  are NOT per-call-site `__FILE__`/`__func__`/`__LINE__` values.
- `src/mod_open_switch.cc` exception wrappers, before / instead of
  using the C++ logger when the logger itself may have failed; these
  call `switch_log_printf` directly with literal "%s" + `e.what()`.

**Known limitation.** The W1 wrapper does NOT plumb per-call-site
`__FILE__` / `__func__` / `__LINE__` through `osw::log::*` to
`switch_log_printf`. FS's mod_console / mod_logfile receive the
literal `"mod_open_switch"` / `"osw_log_emit"` / `0` instead. Adding
per-call-site location forwarding is a future enhancement; it requires
turning `osw::log::*` into macros (so they capture `__FILE__` at the
caller) or threading a `std::source_location` parameter through the
SinkFn signature. Tracked as W1.5 / W2 follow-up; not gating.

---

## FF-013 — `switch_xml_config_parse_module_settings` opens the config, parses settings, frees XML before returning

**Claim.** `switch_xml_config_parse_module_settings(file, reload, instructions)`
is the canonical FS facility for loading a module's `<settings>` block
out of `${conf_dir}/autoload_configs/<file>`. It:

1. Calls `switch_xml_open_cfg(file, &cfg, NULL)`. If that returns NULL
   (file not found, malformed, etc.), logs an error via `switch_log_printf`
   and returns `SWITCH_STATUS_FALSE`.
2. Looks up the `<settings>` child of `<configuration>`.
3. For each `<param name="..." value="..."/>` under `<settings>`,
   calls `switch_xml_config_parse` which iterates the caller's
   `switch_xml_config_item_t[]` and writes the parsed value into the
   `item->ptr` destination via the appropriate `SWITCH_CONFIG_*` type
   handler.
4. Calls `switch_xml_free(xml)` on the root before returning — so the
   caller does NOT free the XML tree, and there is no leak when the
   function returns FALSE for the "file not found" case (it returns
   before opening anything).
5. Returns `SWITCH_STATUS_SUCCESS` if parsing succeeded (including
   "no <settings> block, used all defaults"), or `SWITCH_STATUS_FALSE`
   if the file couldn't be opened or a required item was missing.

The `reload` parameter, when `SWITCH_TRUE`, causes the iteration to
skip items that don't have `CONFIG_RELOADABLE` set — for SIGHUP-style
reloads, this leaves non-reloadable items at their initial values.

**Source.**

- Declaration: `src/include/switch_xml_config.h:161`.
- Definition: `src/switch_xml_config.c:72-89`.

**Permalinks.**

- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/include/switch_xml_config.h#L161>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_xml_config.c#L72-L89>

**Excerpt — definition (lines 72-89):**

```c
SWITCH_DECLARE(switch_status_t) switch_xml_config_parse_module_settings(const char *file, switch_bool_t reload, switch_xml_config_item_t *instructions)
{
    switch_xml_t cfg, xml, settings;
    switch_status_t status = SWITCH_STATUS_SUCCESS;

    if (!(xml = switch_xml_open_cfg(file, &cfg, NULL))) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not open %s\n", file);
        return SWITCH_STATUS_FALSE;
    }

    if ((settings = switch_xml_child(cfg, "settings"))) {
        status = switch_xml_config_parse(switch_xml_child(settings, "param"), reload, instructions);
    }

    switch_xml_free(xml);

    return status;
}
```

**Implications.**

- The caller does NOT manage the XML tree's lifetime. Wrapping this
  function in an `osw::XmlNode` is unnecessary — the function frees
  internally. Our `osw::XmlNode` RAII helper is for the lower-level
  `switch_xml_open_*` family used by future config-loader code that
  walks raw XML directly.
- On "file not found": `SWITCH_STATUS_FALSE` is returned and the
  destinations (`item->ptr`) are NOT written. Callers that want
  defaults applied even when the file is missing MUST initialise the
  destinations themselves before calling. Our `osw::Config` does
  exactly this: it sets all fields to compiled-in defaults BEFORE
  calling `switch_xml_config_parse_module_settings`, so a missing
  config file degrades gracefully to defaults.
- The `instructions` array MUST be terminated by an entry with
  `key == NULL` (the `SWITCH_CONFIG_ITEM_END()` macro emits exactly
  this). Forgetting the terminator results in walking past the array.
- The destinations (`item->ptr`) must outlive any subsequent reads of
  the parsed values. For our `osw::Config`, the destinations are
  members of the `Config` instance, which lives for the module
  lifetime — so the lifetime is correct.

**Used by W1 code:**

- `src/core/config.cc` — `osw::Config::LoadFromFile()` builds the
  `switch_xml_config_item_t[]` table and invokes
  `switch_xml_config_parse_module_settings("open_switch.conf",
  SWITCH_FALSE, items)`.

---

## FF-014 — `switch_loadable_module_create_module_interface` allocates from the module pool (no manual free)

**Claim.** `switch_loadable_module_create_module_interface(pool, name)`
allocates a new `switch_loadable_module_interface_t` from the supplied
APR pool via `switch_core_alloc`. The interface's `module_name` is
strdup'd into the same pool. A read-write lock is created (also pool-
backed). The module pool is owned by FreeSWITCH and is destroyed when
the module is unloaded; the caller does NOT free the returned interface
manually.

**Source.**

- Declaration: `src/include/switch_loadable_module.h` (search
  `switch_loadable_module_create_module_interface`).
- Definition: `src/switch_loadable_module.c:3033-3045`.

**Permalink.**
<https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_loadable_module.c#L3033-L3045>

**Excerpt — definition (lines 3033-3045):**

```c
SWITCH_DECLARE(switch_loadable_module_interface_t *) switch_loadable_module_create_module_interface(switch_memory_pool_t *pool, const char *name)
{
    switch_loadable_module_interface_t *mod;

    mod = switch_core_alloc(pool, sizeof(switch_loadable_module_interface_t));
    switch_assert(mod != NULL);

    mod->pool = pool;

    mod->module_name = switch_core_strdup(mod->pool, name);
    switch_thread_rwlock_create(&mod->rwlock, mod->pool);
    return mod;
}
```

**Implications.**

- The returned `switch_loadable_module_interface_t*` lives for the
  duration of the FS pool the module was loaded with. Calling
  `free()` on it is a double-free against the pool.
- Storing the returned pointer into a module-scoped C++ smart pointer
  is wrong: a `unique_ptr` would call `delete` (the wrong dealloc). If
  we need to keep the pointer around for later registration of
  endpoints / apps / APIs, we hold it as a raw `T*` non-owning view —
  ownership belongs to FS.
- `switch_assert(mod != NULL)` means the function aborts the process
  on allocation failure rather than returning NULL on success. The
  stub's defensive NULL check after the call is documentation, not a
  necessary safety net — but the check is cheap and matches FS's own
  module style, so we keep it.
- The function is intended to be called exactly once per module load,
  inside `SWITCH_MODULE_LOAD_FUNCTION`. Calling it twice produces two
  interface structs and only one will be discoverable by FS.

**Used by W1 code:**

- `src/mod_open_switch.cc::mod_open_switch_load()` — calls this
  exactly once and stores the result in
  `*module_interface` (the out-parameter FS expects). The Module
  singleton holds a non-owning view (`const switch_loadable_module_interface_t*`)
  for later subsystem registration; it never deletes the pointer.

---

## FF-015 — `switch_xml_open_*` / `switch_xml_free` pairing; refcounted root

**Claim.** `switch_xml_open_cfg(file_path, &node, params)` returns
the **root** XML handle and writes the `<configuration>` child into
`*node`. The root is reference-counted: each `switch_xml_free(root)`
decrements `refs`; only when `refs` reaches zero are the underlying
buffers freed. `switch_xml_free(NULL)` is a safe no-op (early-return).

`switch_xml_open_*` returns NULL on failure (file not found, malformed
XML, etc.). The caller MUST `switch_xml_free` exactly one time for
each successful (non-NULL) open.

**Source.**

- `switch_xml_open_cfg`: `src/switch_xml.c:2499-2513`.
- `switch_xml_free`: `src/switch_xml.c:2815-2841`.

**Permalinks.**

- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_xml.c#L2499-L2513>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_xml.c#L2815-L2841>

**Excerpt — open_cfg (lines 2499-2513):**

```c
SWITCH_DECLARE(switch_xml_t) switch_xml_open_cfg(const char *file_path, switch_xml_t *node, switch_event_t *params)
{
    switch_xml_t xml = NULL, cfg = NULL;

    *node = NULL;

    assert(MAIN_XML_ROOT != NULL);

    if (switch_xml_locate("configuration", "configuration", "name", file_path, &xml, &cfg, params, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
        *node = cfg;
    }

    return xml;
}
```

**Excerpt — free, top (lines 2815-2841):**

```c
SWITCH_DECLARE(void) switch_xml_free(switch_xml_t xml)
{
    switch_xml_root_t root;
    int i, j;
    char **a, *s;
    switch_xml_t orig_xml;
    int refs = 0;

  tailrecurse:
    root = (switch_xml_root_t) xml;
    if (!xml) {
        return;
    }

    if (switch_test_flag(xml, SWITCH_XML_ROOT)) {
        switch_mutex_lock(REFLOCK);

        if (xml->refs) {
            xml->refs--;
            refs = xml->refs;
        }
        switch_mutex_unlock(REFLOCK);
    }

    if (refs) {
        return;
    }
    /* ... children / buffer free ... */
```

**Implications.**

- The `osw::XmlNode` RAII helper takes a `switch_xml_t` and calls
  `switch_xml_free(root)` in its destructor. Moving from one
  `osw::XmlNode` to another transfers the obligation. Resetting drops
  it immediately.
- `switch_xml_free(NULL)` is a safe no-op, so the destructor doesn't
  need an explicit NULL check (it has one anyway as a clarity measure
  and to avoid the function-call overhead).
- The refcount means that if multiple consumers call `switch_xml_open_*`
  on the same logical config concurrently, the lifetime is properly
  shared — but our W1 code does NOT do that. We do single-threaded
  config load at module init.

**Used by W1 code:**

- `include/osw/raii/xml_node.h` — `osw::XmlNode` RAII wrapper, exercised
  by `tests/unit/raii/xml_node_test.cc` (with the FS-mock seam — see
  `tests/unit/raii/README.md`).
- `src/core/config.cc` does NOT directly use this — it uses
  `switch_xml_config_parse_module_settings` (FF-013), which manages
  the XML tree internally. `XmlNode` is the RAII for hypothetical
  future code that walks the XML tree directly (W4 SIGHUP reload may
  need it).

---

## FF-016 — `switch_core_session_locate` returns a read-locked session; caller MUST rwunlock

**Claim.** `switch_core_session_locate(uuid_str)` expands (via macro)
to `switch_core_session_perform_locate(uuid_str, __FILE__, __SWITCH_FUNC__, __LINE__)`,
which:

1. Locks the global session-hash mutex.
2. Looks up the session by UUID via `switch_core_hash_find`.
3. If found, acquires a **read lock** on the session via
   `switch_core_session_read_lock` (or its debug variant). If the read
   lock acquisition fails (session in tear-down), it returns NULL.
4. Unlocks the global session-hash mutex.
5. Returns the locked session pointer, OR NULL if not found / lock
   could not be acquired.

The comment in the function body reads: *"if its not NULL, now it's
up to you to rwunlock this"*. Every successful (non-NULL) return MUST
be paired with exactly one `switch_core_session_rwunlock(session)`.

A NULL return is well-defined ("UUID not found" or "session
tearing down") and requires no cleanup.

**Source.**

- Declaration: `src/include/switch_core.h:921` (perform_locate) plus
  the macro at line 932:
  `#define switch_core_session_locate(uuid_str) switch_core_session_perform_locate(uuid_str, __FILE__, __SWITCH_FUNC__, __LINE__)`.
- Definition: `src/switch_core_session.c:121-146`.

**Permalink.**
<https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_core_session.c#L121-L146>

**Excerpt — definition (lines 121-146):**

```c
SWITCH_DECLARE(switch_core_session_t *) switch_core_session_perform_locate(const char *uuid_str, const char *file, const char *func, int line)
{
    switch_core_session_t *session = NULL;

    if (uuid_str) {
        switch_mutex_lock(runtime.session_hash_mutex);
        if ((session = switch_core_hash_find(session_manager.session_table, uuid_str))) {
            /* Acquire a read lock on the session */
#ifdef SWITCH_DEBUG_RWLOCKS
            if (switch_core_session_perform_read_lock(session, file, func, line) != SWITCH_STATUS_SUCCESS) {
#if EMACS_CC_MODE_IS_BUGGY
            }
#endif
#else
            if (switch_core_session_read_lock(session) != SWITCH_STATUS_SUCCESS) {
#endif
                /* not available, forget it */
                session = NULL;
            }
        }
        switch_mutex_unlock(runtime.session_hash_mutex);
    }

    /* if its not NULL, now it's up to you to rwunlock this */
    return session;
}
```

**Implications.**

- `osw::SessionLock` exactly captures this contract: ctor takes a UUID
  string, calls `switch_core_session_locate`, stores the result. dtor
  calls `switch_core_session_rwunlock` iff the pointer is non-NULL.
- The read lock prevents the session from being destroyed while we
  hold it — but does NOT block writes / state transitions. The
  channel can still hang up under us. Code must tolerate "I hold a
  locked session but the channel is hanging up" (typically: bail with
  no error).
- Calling `switch_core_session_locate` with a NULL UUID safely returns
  NULL (it's the outer `if (uuid_str)` guard at line 125).
- The read-lock is **acquired** by `switch_core_session_read_lock` —
  internally that calls `switch_thread_rwlock_rdlock`. Multiple
  threads can hold read locks on the same session simultaneously. The
  destroy path acquires a write lock; that blocks until all readers
  release.

**Used by W1 code:**

- `include/osw/raii/session_lock.h` — `osw::SessionLock`, exercised by
  `tests/unit/raii/session_lock_test.cc` (with the FS-mock seam).
- W1 does NOT yet have a production caller for `SessionLock` — that
  arrives in W3 (Control plane RPCs that look up a session by UUID).
  The helper ships in W1 because it is part of the mandatory RAII
  toolkit and Codex review expects every helper from
  `memory-management.md` §"RAII helpers" to be present.

---

## FF-017 — `switch_event_{create,destroy,fire}` ownership semantics

**Claim.** The FreeSWITCH event-lifecycle macros expand to
`*_detailed` functions that have well-defined ownership transfer
semantics on the caller's `switch_event_t**` argument:

1. `switch_event_create(event, id)` expands to
   `switch_event_create_subclass_detailed(__FILE__, __SWITCH_FUNC__,
   __LINE__, event, id, SWITCH_EVENT_SUBCLASS_ANY)`. On entry it sets
   `*event = NULL`; on success it returns `SWITCH_STATUS_SUCCESS`
   with `*event` pointing at a newly-allocated event. On the early
   subclass-validation failure it returns `SWITCH_STATUS_GENERR`
   with `*event` already NULL'd from the entry assignment.
2. `switch_event_destroy(event)` walks the event headers, frees them,
   then unconditionally sets `*event = NULL` (line 1311). Calling it
   with `*event == NULL` is a no-op (the `if (ep)` guard at line 1294).
3. `switch_event_fire(event)` expands to
   `switch_event_fire_detailed(__FILE__, __SWITCH_FUNC__, __LINE__,
   event, NULL)`. On every success path the caller's `*event` is
   set to NULL — either by `switch_event_queue_dispatch_event`
   (dispatch path) or by `switch_event_deliver_thread_pool`
   (no-dispatch path). On the `SYSTEM_RUNNING <= 0` and queue-push
   failure paths, `switch_event_destroy(event)` runs (which also
   nulls `*event`).

In every success-or-failure return from `switch_event_fire`, the
caller's `switch_event_t*` is NULL. The RAII helper `osw::EventGuard`
relies on this: after `fire()` returns, the guard's internal pointer
is set to nullptr and the destructor does not call
`switch_event_destroy`.

**Source.**

- Macros: `src/include/switch_event.h:153` (`switch_event_create_subclass`),
  `:384` (`switch_event_create`), `:413` (`switch_event_fire`).
- `switch_event_create_subclass_detailed`: `src/switch_event.c:747-787`.
- `switch_event_destroy`: `src/switch_event.c:1289-1312`.
- `switch_event_fire_detailed`: `src/switch_event.c:2006-2038`.
- Inner null-on-success points:
  `switch_event_queue_dispatch_event` at `src/switch_event.c:391` and
  `switch_event_deliver_thread_pool` at `src/switch_event.c:293`.

**Permalinks.**

- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/include/switch_event.h#L153>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/include/switch_event.h#L384>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/include/switch_event.h#L413>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_event.c#L747-L787>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_event.c#L1289-L1312>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_event.c#L2006-L2038>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_event.c#L281-L297>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_event.c#L358-L398>

**Excerpt — `switch_event_destroy` (lines 1289-1312):**

```c
SWITCH_DECLARE(void) switch_event_destroy(switch_event_t **event)
{
	switch_event_t *ep = *event;
	switch_event_header_t *hp, *this;

	if (ep) {
		for (hp = ep->headers; hp;) {
			this = hp;
			hp = hp->next;
			free_header(&this);
		}
		FREE(ep->body);
		FREE(ep->subclass_name);
#ifdef SWITCH_EVENT_RECYCLE
		if (switch_queue_trypush(EVENT_RECYCLE_QUEUE, ep) != SWITCH_STATUS_SUCCESS) {
			FREE(ep);
		}
#else
		FREE(ep);
#endif

	}
	*event = NULL;
}
```

**Excerpt — `switch_event_fire_detailed` (lines 2006-2038):**

```c
SWITCH_DECLARE(switch_status_t) switch_event_fire_detailed(const char *file, const char *func, int line, switch_event_t **event, void *user_data)
{

	switch_assert(BLOCK != NULL);
	switch_assert(RUNTIME_POOL != NULL);
	switch_assert(EVENT_QUEUE_MUTEX != NULL);
	switch_assert(RUNTIME_POOL != NULL);

	if (SYSTEM_RUNNING <= 0) {
		/* sorry we're closed */
		switch_event_destroy(event);
		return SWITCH_STATUS_SUCCESS;
	}

	if (user_data) {
		(*event)->event_user_data = user_data;
	}



	if (runtime.events_use_dispatch) {
		check_dispatch();

		if (switch_event_queue_dispatch_event(event) != SWITCH_STATUS_SUCCESS) {
			switch_event_destroy(event);
			return SWITCH_STATUS_FALSE;
		}
	} else {
		switch_event_deliver_thread_pool(event);
	}

	return SWITCH_STATUS_SUCCESS;
}
```

**Excerpt — `switch_event_queue_dispatch_event` null-on-success
(lines 388-395):**

```c
		}

		*eventp = NULL;
		switch_queue_push(EVENT_DISPATCH_QUEUE, event);
		event = NULL;

	}
```

**Excerpt — `switch_event_deliver_thread_pool` null-on-success
(lines 281-297):**

```c
static void switch_event_deliver_thread_pool(switch_event_t **event)
{
	switch_thread_data_t *td;

	td = malloc(sizeof(*td));
	switch_assert(td);

	td->alloc = 1;
	td->func = switch_event_deliver_thread;
	td->obj = *event;
	td->pool = NULL;

	*event = NULL;

	switch_thread_pool_launch_thread(&td);

}
```

**Implications.**

- After `switch_event_fire(&ev)` returns (success or any failure
  path), `ev` is NULL. Callers MUST NOT re-use the pointer to call
  `switch_event_destroy` or access fields. The `osw::EventGuard`
  helper sets its internal pointer to nullptr inside `fire()` to
  reflect this.
- `switch_event_destroy` is idempotent: calling it on `*event == NULL`
  is a no-op due to the `if (ep)` guard. RAII destructors can always
  call destroy unconditionally as long as they pass the address of
  a possibly-NULL pointer.
- `switch_event_create` always sets `*event = NULL` on entry; callers
  do NOT need to pre-initialise the pointer. The guard's `release()`
  returns the raw pointer without nulling FS-side state — only the
  guard's internal slot is cleared.

**Used by W1 code:**

- `include/osw/raii/event_guard.h` — `osw::EventGuard`, exercised by
  `tests/unit/raii/event_guard_test.cc` (via the FS-mock seam, which
  emulates the null-on-fire behaviour). The header's adjacent code
  comment at line 17 references `switch_event.c:391`; this FF entry
  is the authoritative cite. The mock layer's `next_event_create_status`
  knob covers the early-failure path described above.
- W1 ships no production caller for `EventGuard` — the W2 event-plane
  producer (`SubscribeEvents` source side) is the first user.

---

## FF-018 — `switch_event_bind` lifecycle + callback ownership

**Claim.** `switch_event_bind(id, event, subclass_name, callback,
user_data)` registers a `switch_event_callback_t` (typedef
`void (*)(switch_event_t *)`) into the per-event-id `EVENT_NODES[]`
linked list under a global rwlock. Once registered, the callback is
invoked by any of the up-to-64 dispatch threads (FF-004) from
`switch_event_deliver(switch_event_t **event)` which:

1. Acquires `RWLOCK` for read.
2. Walks `EVENT_NODES[event->event_id]` and then `EVENT_NODES[SWITCH_EVENT_ALL]`.
3. For each matching node, sets `(*event)->bind_user_data = node->user_data`
   then calls `node->callback(*event)` — passing a **single
   pointer**, not a pointer-to-pointer.
4. After the loop, releases `RWLOCK`.
5. Calls `switch_event_destroy(event)` — FS destroys the event
   itself after all bound callbacks have run.

The callback therefore receives a borrowed `switch_event_t *` whose
lifetime extends only until the callback returns. The callback MUST
NOT retain the pointer, free it, fire it, or queue it for later
consumption from a different thread. The callback MUST read what it
needs synchronously (or copy it) before returning.

Unbinding is by `switch_event_unbind(&node)` (the
`event_node` returned via `switch_event_bind_removable`) or
`switch_event_unbind_callback(callback)` which walks all events and
removes every node whose `callback == callback`. Both take the
RWLOCK in write mode, which serialises with active dispatch.
After unbind returns, the callback is guaranteed not to be invoked
again — any in-flight dispatch was reading under the rdlock, and
the wrlock waited for those readers to release.

**Source.**

- Callback typedef: `src/include/switch_types.h:2477`.
- Bind: `src/switch_event.c:2060-2125`
  (`switch_event_bind_removable`) and `:2127-2131`
  (`switch_event_bind` thin wrapper).
- Dispatcher: `src/switch_event.c:299-345` (`switch_event_dispatch_thread`
  body) and `:400-422` (`switch_event_deliver`).
- Unbind: `src/switch_event.c:2134-2173` (`switch_event_unbind_callback`)
  and `:2175-2210` (`switch_event_unbind`).

**Permalinks.**

- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/include/switch_types.h#L2477>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_event.c#L2060-L2131>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_event.c#L400-L422>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_event.c#L2134-L2210>

**Excerpt — callback typedef (line 2477 of switch_types.h):**

```c
typedef void (*switch_event_callback_t) (switch_event_t *);
```

**Excerpt — `switch_event_bind_removable` insertion path (lines
2095-2120):**

```c
if (event <= SWITCH_EVENT_ALL) {
    switch_zmalloc(event_node, sizeof(*event_node));
    switch_thread_rwlock_wrlock(RWLOCK);
    switch_mutex_lock(BLOCK);
    /* <LOCKED> ----------------------------------------------- */
    event_node->id = DUP(id);
    event_node->event_id = event;
    if (subclass_name) {
        event_node->subclass_name = DUP(subclass_name);
    }

    event_node->callback = callback;
    event_node->user_data = user_data;

    if (EVENT_NODES[event]) {
        event_node->next = EVENT_NODES[event];
    }

    EVENT_NODES[event] = event_node;
    switch_mutex_unlock(BLOCK);
    switch_thread_rwlock_unlock(RWLOCK);
    /* </LOCKED> ----------------------------------------------- */

    if (node) {
        *node = event_node;
    }

    return SWITCH_STATUS_SUCCESS;
}
```

**Excerpt — `switch_event_deliver` callback invocation (lines
400-422):**

```c
SWITCH_DECLARE(void) switch_event_deliver(switch_event_t **event)
{
    switch_event_types_t e;
    switch_event_node_t *node;

    if (SYSTEM_RUNNING) {
        switch_thread_rwlock_rdlock(RWLOCK);
        for (e = (*event)->event_id;; e = SWITCH_EVENT_ALL) {
            for (node = EVENT_NODES[e]; node; node = node->next) {
                if (switch_events_match(*event, node)) {
                    (*event)->bind_user_data = node->user_data;
                    node->callback(*event);
                }
            }

            if (e == SWITCH_EVENT_ALL) {
                break;
            }
        }
        switch_thread_rwlock_unlock(RWLOCK);
    }

    switch_event_destroy(event);
}
```

**Excerpt — `switch_event_unbind_callback` (lines 2134-2173):**

```c
SWITCH_DECLARE(switch_status_t) switch_event_unbind_callback(switch_event_callback_t callback)
{
    switch_event_node_t *n, *np, *lnp = NULL;
    switch_status_t status = SWITCH_STATUS_FALSE;
    int id;

    switch_thread_rwlock_wrlock(RWLOCK);
    switch_mutex_lock(BLOCK);
    /* <LOCKED> ----------------------------------------------- */
    for (id = 0; id <= SWITCH_EVENT_ALL; id++) {
        lnp = NULL;

        for (np = EVENT_NODES[id]; np;) {
            n = np;
            np = np->next;
            if (n->callback == callback) {
                if (lnp) {
                    lnp->next = n->next;
                } else {
                    EVENT_NODES[n->event_id] = n->next;
                }

                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Event Binding deleted for %s:%s\n", n->id, switch_event_name(n->event_id));
                FREE(n->subclass_name);
                FREE(n->id);
                FREE(n);
                status = SWITCH_STATUS_SUCCESS;
            } else {
                lnp = n;
            }
        }
    }
    switch_mutex_unlock(BLOCK);
    switch_thread_rwlock_unlock(RWLOCK);
    /* </LOCKED> ----------------------------------------------- */

    return status;
}
```

**Implications.**

- The W2 `osw::events::Binder` calls `switch_event_bind("mod_open_switch",
  SWITCH_EVENT_ALL, NULL, osw_event_handler, this)` once at module
  load. The handler signature MUST be exactly
  `void osw_event_handler(switch_event_t *)` (or, with `extern "C"`,
  a C-linkage function pointer compatible with that typedef).
- The pointer passed in is **owned by FS**. Implementations must read
  headers / body synchronously into module-owned structures (e.g.
  serialise to a `shared_ptr<const std::string>`) BEFORE returning.
  Holding the `switch_event_t*` past return is a use-after-free —
  FS calls `switch_event_destroy(event)` (line 422) right after the
  callback chain.
- The callback runs under FS's rdlock. Doing slow work (acquiring
  contended locks, blocking I/O) inside the callback will stall ALL
  dispatch threads — measure with the synthetic `OSW_DEBUG_TIMING=1`
  histogram and keep the steady-state below 50µs (see W2 contract).
- Multiple dispatch threads may invoke the same callback concurrently
  (FF-004). The callback body MUST be reentrant; per-tier producer
  state needs MPSC-safe synchronisation (mutex + deque + condvar in
  V1).
- Unbind under wrlock waits for in-flight readers — therefore the
  drain order "Binder::Stop() (unbind) → drain rings" is safe: after
  unbind returns, no new event will be enqueued. There is no need
  for a "stop accepting" flag in the callback itself.
- Exceptions: the typedef is C linkage. An exception escaping the
  callback will propagate into FS's C dispatcher, which is undefined
  behaviour. The W2 callback MUST wrap its entire body in
  `try { ... } catch (...) { ... }` and never let an exception escape.
  Standard C++ EH unwinding through a C activation frame violates
  the C ABI used by `switch_thread_t` / APR threads.

**Used by W2 code:**

- `src/events/binder.cc` — the production `osw_event_handler`
  C-linkage shim, wrapped around the C++ `Binder::HandleEvent` body.
- `tests/unit/events/binder_test.cc` — exception-boundary test:
  a stub callback that throws is caught by the wrapper and logged,
  not propagated into a (mocked) caller.

---

## FF-019 — `switch_event_get_header` returns an FS-owned `char*` (lifetime ≤ event)

**Claim.** `switch_event_get_header(event, name)` is a macro that
expands to `switch_event_get_header_idx(event, name, -1)`. The
function walks `event->headers` and returns `hp->value` — a pointer
into the `switch_event_header_t` node's value field, allocated when
the header was added (via `DUP(value)` inside `switch_event_add_header*`).
The string is **owned by the event**, freed when `switch_event_destroy`
runs (`free_header(&this)` at line 1300 of `switch_event.c`). The
caller MUST NOT `free()` the returned pointer and MUST NOT retain it
past the lifetime of the event.

`switch_event_get_body(event)` is the symmetric body accessor:
returns `event->body`, with the same ownership.

A NULL return means "header not present" (or "header_name is NULL").

**Source.**

- Macro: `src/include/switch_event.h:172`
  (`#define switch_event_get_header(_e, _h) switch_event_get_header_idx(_e, _h, -1)`).
- `switch_event_get_header_idx`: `src/switch_event.c:846-864`.
- `switch_event_get_header_ptr`: `src/switch_event.c:825-844`.
- Header-value lifetime: `src/switch_event.c:1289-1312` (destroy
  loop — see FF-017 excerpt).

**Permalinks.**

- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/include/switch_event.h#L170-L173>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_event.c#L825-L864>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_event.c#L1289-L1312>

**Excerpt — macro (lines 170-173 of switch_event.h):**

```c
SWITCH_DECLARE(switch_event_header_t *) switch_event_get_header_ptr(switch_event_t *event, const char *header_name);
_Ret_opt_z_ SWITCH_DECLARE(char *) switch_event_get_header_idx(switch_event_t *event, const char *header_name, int idx);
#define switch_event_get_header(_e, _h) switch_event_get_header_idx(_e, _h, -1)
```

**Excerpt — `switch_event_get_header_idx` (lines 846-864):**

```c
SWITCH_DECLARE(char *) switch_event_get_header_idx(switch_event_t *event, const char *header_name, int idx)
{
    switch_event_header_t *hp;

    if ((hp = switch_event_get_header_ptr(event, header_name))) {
        if (idx > -1) {
            if (idx < hp->idx) {
                return hp->array[idx];
            } else {
                return NULL;
            }
        }

        return hp->value;
    } else if (!strcmp(header_name, "_body")) {
        return event->body;
    }

    return NULL;
}
```

**Excerpt — `switch_event_get_header_ptr` (lines 825-844):**

```c
SWITCH_DECLARE(switch_event_header_t *) switch_event_get_header_ptr(switch_event_t *event, const char *header_name)
{
    switch_event_header_t *hp;
    switch_ssize_t hlen = -1;
    unsigned long hash = 0;

    switch_assert(event);

    if (!header_name)
        return NULL;

    hash = switch_ci_hashfunc_default(header_name, &hlen);

    for (hp = event->headers; hp; hp = hp->next) {
        if ((!hp->hash || hash == hp->hash) && !strcasecmp(hp->name, header_name)) {
            return hp;
        }
    }
    return NULL;
}
```

**Implications.**

- Inside the W2 `osw_event_handler` callback, code that reads a
  header value with `switch_event_get_header(event, "Unique-ID")`
  obtains a pointer valid for the duration of the callback only.
  Copy with `std::string` (or `std::string_view` followed by an
  arena allocation) before the callback returns.
- A NULL return must NOT be passed to `std::string` constructors
  that take a `const char*` without length (UB on NULL). The
  envelope-builder helper `HeaderOr("")` defensively maps NULL to
  empty string.
- `switch_event_get_body(event)` has the same lifetime contract;
  the W2 envelope builder copies the body into `EventEnvelope.body`
  (a `bytes` field that owns a `std::string`) synchronously.
- The function uses case-insensitive comparison (`strcasecmp`), so
  "Unique-ID", "unique-id", and "UNIQUE-ID" all match the same
  header. The W2 builder uses canonical casing in its constants
  for readability but is robust to case variation in source events.
- For `iter`-style enumeration of all headers (needed by the
  include-list filter), the W2 builder walks `event->headers` directly
  via the public `switch_event_header_t` chain (`hp->name`,
  `hp->value`, `hp->next`). This is the only public method the
  v1.10.12 API exposes for "list all headers"; `switch_event_serialize`
  exists but allocates a flat string.

**Used by W2 code:**

- `src/events/envelope.cc` — `BuildEnvelope` uses
  `switch_event_get_header` for the well-known fields (Unique-ID,
  Event-Date-Timestamp, etc.) and walks `event->headers` directly
  for the include-list-driven `headers` map.

---

## FF-020 — `switch_event_create_subclass` requires `SWITCH_EVENT_CUSTOM` + non-NULL subclass, adds `Event-Subclass` header

**Claim.** `switch_event_create_subclass(event, event_id, subclass_name)`
is a macro that expands to
`switch_event_create_subclass_detailed(__FILE__, __SWITCH_FUNC__,
__LINE__, event, event_id, subclass_name)`. The
detailed function:

1. Sets `*event = NULL` unconditionally on entry.
2. Returns `SWITCH_STATUS_GENERR` (without allocating) if
   `event_id` is neither `SWITCH_EVENT_CLONE` nor `SWITCH_EVENT_CUSTOM`
   AND `subclass_name` is non-NULL. (Translation: only CUSTOM /
   CLONE events may carry a subclass name; the function refuses
   to set a subclass on, e.g., a `SWITCH_EVENT_CHANNEL_HANGUP`
   event.)
3. Allocates a new event (or pops from the recycle queue),
   memsets to zero, sets `event_id`.
4. If `subclass_name != NULL`: stores `subclass_name` (DUP'd) into
   `(*event)->subclass_name` AND calls
   `switch_event_add_header_string(*event, SWITCH_STACK_BOTTOM,
   "Event-Subclass", subclass_name)` so the wire form carries
   the subclass under the `Event-Subclass` header.
5. Returns `SWITCH_STATUS_SUCCESS`.

The wire-side subclass identifier is therefore the
`Event-Subclass` header value. Subscribers / handlers that filter
on subclass match against this header.

Subclass names that you intend to FIRE (publish) should be
reserved up-front via `switch_event_reserve_subclass(name)` at
module load — but reservation is optional for FIRING (only required
for FS internal accounting). Reservation IS required to bind to a
specific subclass via `switch_event_bind(..., SWITCH_EVENT_CUSTOM,
subclass_name, ...)`. Our W2 module binds to `SWITCH_EVENT_ALL` and
filters in-process, so we do not need to reserve our `osw.audit.*`
subclasses to receive them — but reserving is still polite and
audit-friendly (the bind path in `switch_event_bind_removable` at
lines 2071-2079 auto-reserves on demand, so a missed reservation is
not catastrophic).

**Source.**

- Macro: `src/include/switch_event.h:150-153`.
- `switch_event_create_subclass_detailed`: `src/switch_event.c:747-787`.
- `switch_event_reserve_subclass_detailed`: `src/switch_event.c:485-522`.

**Permalinks.**

- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/include/switch_event.h#L150-L153>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_event.c#L747-L787>
- <https://github.com/signalwire/freeswitch/blob/v1.10.12/src/switch_event.c#L485-L522>

**Excerpt — macro (lines 150-153 of switch_event.h):**

```c
SWITCH_DECLARE(switch_status_t) switch_event_create_subclass_detailed(const char *file, const char *func, int line,
                                                                      switch_event_t **event, switch_event_types_t event_id, const char *subclass_name);
#define switch_event_create_subclass(_e, _eid, _sn) switch_event_create_subclass_detailed(__FILE__, (const char * )__SWITCH_FUNC__, __LINE__, _e, _eid, _sn)
```

**Excerpt — `switch_event_create_subclass_detailed` (lines 747-787):**

```c
SWITCH_DECLARE(switch_status_t) switch_event_create_subclass_detailed(const char *file, const char *func, int line,
                                                                      switch_event_t **event, switch_event_types_t event_id, const char *subclass_name)
{
#ifdef SWITCH_EVENT_RECYCLE
    void *pop;
#endif

    *event = NULL;

    if ((event_id != SWITCH_EVENT_CLONE && event_id != SWITCH_EVENT_CUSTOM) && subclass_name) {
        return SWITCH_STATUS_GENERR;
    }
#ifdef SWITCH_EVENT_RECYCLE
    if (EVENT_RECYCLE_QUEUE && switch_queue_trypop(EVENT_RECYCLE_QUEUE, &pop) == SWITCH_STATUS_SUCCESS && pop) {
        *event = (switch_event_t *) pop;
    } else {
#endif
        *event = ALLOC(sizeof(switch_event_t));
        switch_assert(*event);
#ifdef SWITCH_EVENT_RECYCLE
    }
#endif

    memset(*event, 0, sizeof(switch_event_t));

    if (event_id == SWITCH_EVENT_REQUEST_PARAMS || event_id == SWITCH_EVENT_CHANNEL_DATA || event_id == SWITCH_EVENT_MESSAGE) {
        (*event)->flags |= EF_UNIQ_HEADERS;
    }

    if (event_id != SWITCH_EVENT_CLONE) {
        (*event)->event_id = event_id;
        switch_event_prep_for_delivery_detailed(file, func, line, *event);
    }

    if (subclass_name) {
        (*event)->subclass_name = DUP(subclass_name);
        switch_event_add_header_string(*event, SWITCH_STACK_BOTTOM, "Event-Subclass", subclass_name);
    }

    return SWITCH_STATUS_SUCCESS;
}
```

**Implications.**

- The W2 audit helper `osw::audit::Emit(name, headers)` calls
  `switch_event_create_subclass(&ev, SWITCH_EVENT_CUSTOM, full_subclass)`
  where `full_subclass = "osw.audit." + name`. It must pass
  `SWITCH_EVENT_CUSTOM` (not `SWITCH_EVENT_ALL` or another id); any
  other event_id with a non-NULL subclass returns
  `SWITCH_STATUS_GENERR` and the caller's `ev` stays NULL.
- The W2 audit subclass family is `osw.audit.*`:
  `osw.audit.module_loaded`, `osw.audit.module_shutdown_with_pending_events`,
  `osw.audit.subscriber_connected`,
  `osw.audit.subscriber_disconnected`,
  `osw.audit.subscriber_kicked` (RESOURCE_EXHAUSTED), and any
  future-W3+ control-API audit subclasses (`osw.audit.originate_started`,
  etc.).
- The classifier (`src/events/tier.cc`) recognises subclasses
  matching the glob `osw.audit.*` and routes them to Tier 1
  unconditionally (per W2 default rules).
- Subscribers that filter by `subclass_name` on the envelope side
  (the `EventEnvelope.subclass_name` field, populated from the
  `Event-Subclass` header in the envelope builder) will receive
  the dotted-namespace string we passed in. There is no FS-side
  case normalisation; ours stays lowercase by convention.
- We deliberately do NOT call `switch_event_reserve_subclass`
  at module load for the `osw.audit.*` family. We bind to
  `SWITCH_EVENT_ALL` so reservation is unnecessary for our own
  receive path, and the auto-reservation in
  `switch_event_bind_removable:2071-2079` would only fire if some
  third party tried to bind to our exact subclass — which is
  fine. If a future caller wants to bind a SPECIFIC subclass
  filter at the FS level, we will need to reserve it first; the
  cost is negligible and the entry would be tracked in
  `src/events/binder.cc::Init()`.

**Used by W2 code:**

- `src/observability/audit.cc` — `osw::audit::Emit` builds the
  CUSTOM event with `Event-Subclass = "osw.audit." + name`, adds
  caller-supplied headers, and fires via `osw::EventGuard::fire()`.
  The event then re-enters our own pipeline via the binder
  callback (Tier 1 by classifier rule) and ships to subscribers.
- `tests/unit/observability/audit_test.cc` — exercises the helper
  against the FS-mock seam; verifies `Event-Subclass` is set to the
  full dotted name and the subclass-name slot on the envelope is
  populated.

---

## How to add a new FF entry

If you find a previously-undocumented FreeSWITCH behaviour that
the spec depends on, add it here BEFORE editing the spec:

1. Open the FS source at v1.10.12 (clone fresh if needed).
2. Verify the behaviour with a 5-20 line excerpt.
3. Allocate the next `FF-NNN` id.
4. Write the entry following the format above.
5. Land it in a commit that adds ONLY the FF entry (no spec
   edits), so the reviewer can verify the citation independently.
6. THEN edit specs that reference the entry by id.

The discipline this enforces: every spec claim about FS that lands
has at least one human (the reviewer) reading the FS source. The
round 1 / round 2 failures all happened because this step was
skipped.
