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
- `src/include/switch_platform.h:190-200` (visibility attributes on
  the GCC/Linux path).

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

**Excerpt — `switch_platform.h:184-200` (the GCC/Linux branch):**

```c
#elif defined(__GNUC__) && __GNUC__ >= 4

#define SWITCH_MOD_DECLARE(type)        __attribute__((visibility("default"))) type
#define SWITCH_MOD_DECLARE_NONSTD(type) __attribute__((visibility("default"))) type
#define SWITCH_MOD_DECLARE_DATA         __attribute__((visibility("default")))

#else

#define SWITCH_MOD_DECLARE(type)    type
#define SWITCH_MOD_DECLARE_NONSTD(type)    type
#define SWITCH_MOD_DECLARE_DATA
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
