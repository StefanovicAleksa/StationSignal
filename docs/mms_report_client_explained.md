# `mms_report_client` — Full Walkthrough

> Source: `src/features/mms_report_client/`
>
> Both known issues this doc originally flagged have since been fixed:
>
> 1. **Descriptive data types** (Dbpos labeling / CODEDENUM ambiguity) — the
>    whole feature was removed at explicit user request. Every value, Dbpos or
>    otherwise, is now reported identically (a raw integer for bitstring/
>    CODEDENUM values). The section that used to flag this as an open issue
>    has been deleted.
> 2. **Caching on disconnect** (null-ish/stale values after reconnect) — root
>    cause confirmed and fixed: the value-diff cache used to be wiped back to
>    `NULL` on every reconnect, turning the reconnect's own fresh snapshot
>    into a false "bootstrap" event with no real previous value to report. The
>    cache is now populated exactly once and preserved for the client's whole
>    lifetime — see §7 below for the full redesign, kept as a reference for
>    how the mechanism works now (not as an investigation guide).

---

## 1. Overview

`mms_report_client` connects to **one IED** over MMS, enables every Report
Control Block (RCB — BRCB/URCB) that `ied_model` says exists for it (never
re-discovers RCBs over the wire — SCL, or `ied_model_online_loader`'s
live-discovered equivalent, is the only source of truth), and turns every
report the server pushes into a normalized `MmsReportRecord` delivered to a
callback the caller registers. No JSON here — that's `ipc_dispatcher`'s job.

At the highest level, three things are going on simultaneously:

1. **Connection lifecycle** — a dedicated background thread
   (`supervisorLoop`) that connects, enables RCBs, waits, detects loss,
   backs off, and reconnects — forever, until stopped.
2. **Report decoding** — a separate thread (libiec61850's own internal
   report-reader thread) that receives raw MMS reports and turns them into
   `MmsReportRecord`s.
3. **Event filtering ("hybrid filter")** — pure domain logic that decides,
   per data point, whether a given report entry represents an actual change
   worth forwarding, or noise (a duplicate, a bootstrap/GI snapshot, a
   post-reconnect replay) that should be silently absorbed.

The feature is intentionally split file-by-file along this codebase's usual
`service/domain/data/utils` lines:

| Layer | File | Job |
|---|---|---|
| `service/mms_report_client_api.h/.c` | Public API + one-time setup (`buildMemberRefCache`) |
| `domain/mms_report_client_types.h` | All structs — this is the actual domain model |
| `domain/mms_report_client_usecases.c` | Pure logic: the hybrid filter, Gap-4 structure decomposition, quality-grouping, backoff math, wire-reference conversion |
| `data/mms_report_client_connection.c` | The supervisor thread, RCB enable sequence, dynamic-dataset creation, reconnect/backoff state machine |
| `data/mms_report_client_report_adapter.c` | Bridges libiec61850's `ReportCallbackFunction` into `MmsReportClientUseCases_buildReportRecord` |
| `data/mms_report_client_auth.c` | ACSE password auth setup (duplicated from `scl_bootstrap`'s equivalent, deliberately — see CLAUDE.md's cross-feature convention) |

---

## 2. Public API surface (`service/mms_report_client_api.h`)

```
MmsReportClient_create(iedModel, host, port, config, &err)  -> handle
MmsReportClient_setReportCallback(handle, cb, userParam)
MmsReportClient_setConnectionStateCallback(handle, cb, userParam)   // optional
MmsReportClient_setRcbStatusCallback(handle, cb, userParam)         // optional
MmsReportClient_start(handle)   -> non-blocking, starts supervisor thread
MmsReportClient_stop(handle)    -> blocking, joins supervisor thread
MmsReportClient_destroy(handle)
MmsReportClient_destroyReportRecord(record)   // caller must call this on every delivered record
```

Key contract points:
- `iedModel` is **borrowed** — the caller (usually `orchestration`) owns it
  and must outlive the client.
- `host`/`port` are caller-supplied — SCL's `<ConnectedAP>` IP is not parsed
  anywhere in this codebase.
- `MmsReportClient_start` reads `IedModel_getReportSubscriptionTargets` once
  (`client->targets`, a `LinkedList` of `ReportControlBlockTarget*`) and
  builds `client->memberRefCache` once (`buildMemberRefCache`, see §3) —
  **both are built exactly once and reused across every reconnect.** Nothing
  about the RCB list, the reference/type side-tables, or the **value-diff
  cache itself** changes on reconnect — the cache is populated once (on each
  slot's first-ever observation) and preserved for the client's whole
  lifetime, never reset. See §7 for the full design and the real-hardware
  bug this fixed.
- The report callback delivers ownership of the `MmsReportRecord*` to the
  caller — must be freed with `MmsReportClient_destroyReportRecord`.

---

## 3. Setup: `buildMemberRefCache` (`service/mms_report_client_api.c`)

Called once from `MmsReportClient_start`, before the supervisor thread ever
touches the network. For every RCB target, this builds one
`MmsReportClientMemberRefCacheEntry` (defined in
`domain/mms_report_client_types.h`) that stays alive for the client's whole
lifetime. This is the single most important struct in the feature — almost
everything downstream indexes into it.

For each RCB target, purely from the **already-parsed SCL model** (never
over the wire — this respects the "no over-the-wire tree discovery" hard
rule):

1. **`memberReferences`** — the dataset's ordered member reference strings
   (`IedModel_getDataSetMemberReferences`), or, if the RCB has no configured
   dataset at all (`datSet="Dyn"` in SCL terms), every FC=ST/MX leaf under
   the RCB's own LN (`IedModel_getReportableAttributeReferencesForLogicalNode`).
2. **`memberLeafReferences`/`memberLeafCounts`** ("Gap 4" — structure
   decomposition) — for each raw dataset member that is itself a
   *structured* attribute (e.g. a DPC `Pos` with sub-attributes `stVal`,
   `q`, `t`), the ordered list of leaf reference strings underneath it
   (`IedModel_getDataSetMemberLeafReferences`). A flat (non-structured)
   member gets `NULL`/`0` here — nothing to decompose.
3. **`memberLeafWireTypes`** — parallel to `memberLeafReferences`: each
   decomposed leaf's *expected* (SCL-declared) `DataAttributeType`
   (`IedModel_getDataSetMemberLeafWireTypes`) — guards against a
   same-count-but-different-order mismatch between this daemon's
   locally-resolved leaf order and a real device's actual wire order (see §6's
   `decomposedLeafTypesMatch`).
4. **`leafSlotOffsets`/`totalLeafSlots`** — flattens the whole RCB into one
   contiguous array of "leaf slots": a non-decomposed member occupies 1
   slot, a decomposed member occupies `memberLeafCounts[i]` consecutive
   slots.
5. **`lastForwardedValues`** — `calloc`'d to `totalLeafSlots`, all `NULL` —
   this is **the value-diff cache** the hybrid filter reads/writes. Each slot
   is populated exactly once, on its first-ever observation, and then
   **preserved for the client's whole lifetime — never reset again**, not
   even on reconnect (see §6/§7).
6. **`everPopulated`** — a single `bool` flag on the whole cache entry, set
   once, at the end of the first report this RCB ever processes. Purely a
   debug-logging gate (see §7) — has no effect on forward/drop decisions.

None of the above ever touches the network — it's all local SCL-derived
bookkeeping, computed once.

---

## 4. Connection lifecycle: the supervisor thread

`data/mms_report_client_connection.c`'s `supervisorLoop` is the heart of the
feature — one dedicated `Thread` (via `hal_thread.h`), running until
`stopRequested`:

```
while (!stopRequested):
    IedConnection_connect(host, port)
    if connected:
        connectedAtMs = now()
        enableAllTargets()                      // enable every RCB
        loop:                                     // "connected phase"
            wait on wakeSignal
            if stopRequested: break outer
            if connectionLostSignal: clear it, break inner  // fall through to backoff
            else: spurious wake, keep waiting     // see below
        if (now() - connectedAtMs) >= 5000ms:
            currentBackoffMs = 0                  // only reset backoff after a STABLE connection
    if stopRequested: break
    delay = computeNextBackoffDelay(currentBackoffMs, initial=1000, max=30000)
    interruptibleSleep(delay)
```

### Why the "spurious wake" loop exists (a fixed bug, worth understanding)

`onStateChanged` (libiec61850's state-changed callback, fired on an
internal library mutex — must never block or call blocking `IedConnection_*`
APIs) posts `wakeSignal` on **every** state transition, not just
`IED_STATE_CLOSED`. A single successful `IedConnection_connect()` drives
`CONNECTING` then `CONNECTED`, each posting once — so by the time
`supervisorLoop` first reaches its wait, wakes are already pending. An
earlier version treated *any* wake as "go reconnect," re-running
`enableAllTargets()` a second time for one real connect — a real,
confirmed root cause of duplicate-report floods after reconnect (see
CLAUDE.md's bugfix history). The fix: only a genuine
`connectionLostSignal == true` (set exclusively when `onStateChanged` sees
`IED_STATE_CLOSED`) breaks out of the connected phase.

### Backoff-reset debounce

`currentBackoffMs` only resets to 0 if the connection that was just lost had
been up for at least `MMS_REPORT_CLIENT_STABLE_CONNECTION_MS` (5000ms).
Without this, a flaky real link that connects then immediately bounces would
get stuck retrying at the ~1s initial tier forever instead of escalating —
and each retry re-triggers the full reset+GI enable cycle, giving any
comparison bug (§6) more chances to fire in a tight burst.

---

## 5. Enabling one RCB: `enableOneTarget`

Called once per target, from `enableAllTargets`, every single connect
**and** every reconnect:

1. `IedConnection_getRCBValues` — fetch the RCB object.
2. `IedConnection_installReportHandler` — installed **before** enabling, so
   no report can arrive with no handler attached.
3. Build the enable mask: `RCB_ELEMENT_RPT_ENA | RCB_ELEMENT_GI`, plus
   `RCB_ELEMENT_DATSET` if a dataset reference is available (SCL-configured,
   or dynamically created via `getOrCreateDynamicDataset` for a
   `datSet="Dyn"` RCB — a real, `@`-prefixed, association-scoped dataset
   covering every FC=ST/MX leaf of the RCB's LN, de-duplicated per LN per
   connect cycle).
4. `ClientReportControlBlock_setRptEna(rcb, true)` +
   `ClientReportControlBlock_setGI(rcb, true)` — **GI is requested on every
   enable, unconditionally.** `TrgOps`/`BufTm`/`IntgPd`/`ConfRev` are never
   touched — left exactly as the IED's own SCL config has them.
5. `IedConnection_setRCBValues(..., mask, true)` — the actual enable write.

**No cache reset happens anywhere in this function anymore** — an earlier
design reset the whole RCB's value-diff cache here, right before the enable
write, on every single (re-)enable. That's been removed entirely (see §7):
the cache is populated once and preserved forever, so there's nothing left
to reset on a reconnect.

Why GI, then, given a `reason` field is never trusted for filtering (see
§6)? Because *something* has to force a deterministic snapshot to diff
against. On a genuine first-ever connect, that snapshot lands against the
still-all-NULL cache and is bootstrap-suppressed (never forwarded) exactly
like any other first observation. On every reconnect after that, this same
GI snapshot instead diffs against the **real, preserved last-known value**
from before the disconnect — a genuine change made while disconnected
forwards with a real previous value; an unchanged resend is suppressed by
the ordinary diff check. Without GI, on a device with no periodic integrity
reporting and no coincidental traffic at enable time, a real change made
while disconnected could go undetected simply because nothing prompted the
device to report it yet.

---

## 6. The hybrid event filter (`domain/mms_report_client_usecases.c`)

This is the piece responsible for turning "every report the server sends"
into "only reports that represent a genuine change."

### `shouldForwardAndUpdateCache` — the single decision point

For one leaf slot:
```
if slot < 0: return true            // no cache coverage, always forward, no previousValue
cached = lastForwardedValues[slot]
*outPreviousValue = clone(cached) if cached else NULL
if cached == NULL:
    updateValueDiffCache(slot, newValue)   // seed
    return false                            // bootstrap — NEVER forwarded
if isDuplicateValue(cached, newValue): return false
updateValueDiffCache(slot, newValue)
return true
```

Two structural properties matter:

- **`reason` (`ReasonForInclusion`, e.g. `DATA_CHANGE`/`GI`/`QUALITY_CHANGE`)
  is *never* trusted as a bypass.** An earlier version trusted a
  real-change bit as "skip the diff, always forward" — real hardware proved
  this unsafe: a device tagged hundreds of byte-identical reports
  `DATA_CHANGE`. `reason` is only ever carried through as informational
  metadata on `MmsReportEntry.reason`.
- **`cached == NULL` is unconditionally treated as "bootstrap, seed but
  never forward"** — this is what makes a genuine first-ever connect's GI
  snapshot, a foreign client's concurrent GI, or a buffered RCB's
  first-ever redelivery all order-independent: whichever lands first seeds
  the cache and is dropped; whichever lands second is diffed as a duplicate
  and also dropped. Since the cache is never reset again after that first
  population (see below), `cached == NULL` should be structurally impossible
  on any later report — if it happens anyway, `shouldForwardAndUpdateCache`
  logs it loudly to stderr as a bug (gated on the `everPopulated` flag, see
  §3/§7).

### `valuesAreSemanticallyEqual` — the *real* comparison

`MmsValue_equals` (libiec61850) is a **raw byte-exact `memcmp`**. That's
wrong for two wire types this filter relies on being compared *semantically*:

- **`MMS_UTC_TIME`** — the last of its 8 bytes is a `TimeQuality` flag
  (leap-second-known / clock-failure / not-synchronized / accuracy), not
  part of "when did this happen." It can legitimately wobble around a
  reconnect while the millisecond timestamp is unchanged. Fixed by
  comparing via `MmsValue_getUtcTimeInMs` instead of raw bytes.
- **`MMS_BIT_STRING`** — the wire encoding for CODEDENUM/Dbpos/Tcmd status
  points. `MmsValue_equals` `memcmp`s the *whole buffer including unused
  padding bits*, and real firmware is commonly inconsistent about
  zero-padding those across different code paths (e.g. a GI-triggered read
  vs. a live-change report). Two values that decode to the identical
  integer can still fail a raw `memcmp`. Fixed by comparing via
  `MmsValue_getBitStringSize` (guard) + `MmsValue_getBitStringAsInteger`.

Every other type falls through to `MmsValue_equals` unchanged.
`goose_subscriber_usecases.c` has an independently-duplicated copy of this
exact function (per this codebase's no-shared-domain-layer convention).

### The cache is never reset — populated once, preserved forever

An earlier design reset every `lastForwardedValues[slot]` back to `NULL` on
every single (re-)enable, via a function called
`MmsReportClientUseCases_resetValueDiffCache`. **That function and its one
call site (in `enableOneTarget`) are both gone.** See §7 for the full story
of why: in short, wiping the cache on every reconnect turned each
reconnect's own fresh GI snapshot into a false "bootstrap" event, discarding
a perfectly good last-known value and reporting `previousValue: null` right
when a real value existed a moment before.

Under the current design, a slot is written exactly twice in its life: once
when `updateValueDiffCache` first seeds it (bootstrap, `cached == NULL`),
and again every time a genuinely different value arrives afterward. It is
never set back to `NULL`.

`handle->memberRefCacheLock` (a binary `Semaphore`) still guards
`lastForwardedValues` — held by the report-reader thread (via `onReport` →
`MmsReportClientUseCases_buildReportRecord`) on every access. With the
reset gone, this thread is now the cache's only writer, so the lock is
technically guarding a single-writer scenario — kept anyway as cheap,
uncontended insurance against a future writer reappearing on another
thread. (Historically, this lock's absence — combined with the reset
running on the supervisor thread concurrently with report processing — was
a confirmed real root cause of a value-corruption flood on real hardware;
see CLAUDE.md's bugfix history for the full story, now superseded by the
reset's removal.)

### Building the entry list — `buildEntries`, three phases

1. **`collectCandidates`** — walks every raw dataset position; if it's a
   decomposed structure, flattens it (`MmsReportClientUtils_flattenStructure`)
   and zips each flattened leaf against `memberLeafReferences[i]` **only
   if** the flattened count matches `memberLeafCounts[i]` **and** every
   leaf's actual wire-decoded `MmsType` matches its expected
   `DataAttributeType` (`decomposedLeafTypesMatch`, guards against a
   same-count-but-different-order mismatch between this daemon's
   locally-resolved SCL leaf order and a real device's actual wire order —
   confirmed on real hardware, a DPC's `stVal`/`t` came out swapped). A
   mismatch on either falls back to the raw, non-decomposed entry.
2. **Per-candidate filter + group-extension** — runs
   `shouldForwardAndUpdateCache` on every candidate, then a second pass:
   every `q`-named candidate's own `$`-prefix is a "group anchor"; every
   candidate resolves to the *longest* anchor it's genuinely nested under
   (`resolveGroupAnchor` — an ancestor walk, not a single-`$`-strip, needed
   for deeply nested CMVs). If **any** candidate in a group qualifies, the
   whole group forwards — this is what keeps quality and its value sibling
   traveling together even though quality rarely changes on its own.
3. **Emit** — every forwarded candidate is cloned into the final
   `MmsReportEntry` array; everything else's `previousValue` clone (which
   was always taken in phase 2, regardless of forward/drop outcome — a
   dragged-in group member needs its own previous value too) is freed.

### Cross-RCB dedup

Separately, `MmsReportClientUseCases_shouldForwardAcrossRcb` catches the
case where two *different* redundant RCB instances (e.g. `urcbA01`/`urcbB01`
on the same LN/dataset, common for multi-client redundancy) report the
exact same event nearly simultaneously — each survives its own per-RCB
filter independently, so this second, cross-RCB cache (keyed by the last
forwarded record's content, any RCB) suppresses the second one. Persists
across reconnects — never reset in `enableOneTarget`, since it's an
orthogonal concern to per-RCB resync.

---

## 7. Fixed: caching on disconnect / null values

**Original symptom**: values looked wrong, stale, or null-ish right around a
disconnect/reconnect boundary — specifically, `previousValue` came back
`NULL` (or the reported "change" looked fabricated with no real prior
context) right after a reconnect, even though the device had a perfectly
good last-known value a moment before the connection blipped.

**Root cause**: `MmsReportClientMemberRefCacheEntry.lastForwardedValues`
(§3, §6) used to be wiped back to `NULL` in full on every single
(re-)enable — first connect **and** every reconnect alike — via
`MmsReportClientUseCases_resetValueDiffCache`, called from `enableOneTarget`
right before the enable write. That made every reconnect's own GI snapshot
look identical to a genuine first-ever connect from `shouldForwardAndUpdateCache`'s
point of view: `cached == NULL` → bootstrap → silently seeded, never
forwarded, and — critically — with no real value to report as
`previousValue` even once a genuine change *did* eventually get forwarded,
since the "previous" value in the cache was itself just a freshly-reseeded
snapshot, not the device's true pre-disconnect state.

**The fix** (this codebase's current, permanent design): the reset function
and its one call site are both **deleted**. The cache is now:

1. **Populated exactly once** — on each leaf slot's genuine first-ever
   observation, whatever naturally produces it (this client's own requested
   GI on first connect, a foreign client's concurrent GI, or a buffered
   RCB's first redelivery).
2. **Preserved for the client's entire lifetime** — never reset, on
   reconnect or otherwise. Only `MmsReportClient_stop`/`_destroy` (an
   explicit `STOP_REPORTING`) frees it, via
   `MmsReportClientUseCases_destroyMemberRefCacheEntry`.
3. **Diffed against on every reconnect's own fresh GI snapshot**, same as
   any other report — a genuine change made while disconnected now forwards
   with the **real, true pre-disconnect value** as `previousValue`; an
   unchanged resend is suppressed by the ordinary diff check, no
   bootstrap/reset logic involved at all.

A new `everPopulated` flag (§3) on each `MmsReportClientMemberRefCacheEntry`
tracks whether that RCB has ever processed a report — set once, at the end
of `buildEntries`, based on its value at the *start* of that call (so the
true first-ever report, which finds every slot `NULL`, never trips it).
Once `everPopulated` is `true`, a `cached == NULL` slot found in any later
report is structurally unexpected — `shouldForwardAndUpdateCache` logs it to
stderr on **every** occurrence:

```
[mms_report_client] ERROR: cache slot %d (reference '%s') for RCB '%s' is
unexpectedly NULL after this RCB was already populated once - this should
never happen, investigate
```

If you ever see this line, something is resetting or bypassing the cache in
a way this design doesn't expect — start by checking whether
`enableOneTarget` (or any other code path) has reintroduced a reset call, or
whether `lastForwardedValues` is being touched outside
`shouldForwardAndUpdateCache`/`updateValueDiffCache`.

`goose_subscriber` has the identical fix, identical reasoning, and an
identical `everPopulated`-gated log line (`[goose_subscriber]` prefix) — its
own reset used to fire on every STALE/INVALID_STATE→VALID liveness recovery
instead of an MMS reconnect, but the bug and the fix are structurally the
same.

---

## 8. Everything else worth knowing before you dig in

- **No polling** — reports are 100% push-driven by the IED's own BRCB/URCB
  triggering. The only "loop" in this feature is the supervisor's
  reconnect state machine and its blocking wait on `wakeSignal`.
- **Two threads exist**: the supervisor thread (connect/enable) and
  libiec61850's own internal report-reader thread (decode incoming
  reports). `memberRefCacheLock` still guards `lastForwardedValues`, though
  the report-reader thread is now its only writer (the supervisor thread no
  longer resets it) — see §7. Everything else in this feature is either
  read-only after `MmsReportClient_start`, or thread-confined.
- **The client never writes `TrgOps`/`BufTm`/`IntgPd`/`ConfRev`** — those
  are left exactly as the device's own SCL config has them. Only
  `RptEna`/`GI`/`DatSet` are ever set.
- **Dynamic datasets** (`datSet="Dyn"`) are association-scoped
  (`@`-prefixed name) — destroyed automatically when the connection closes,
  no explicit cleanup, no leak risk across reconnects.

---

## Suggested Markdown Viewers

If you'd rather read this as rendered Markdown instead of raw text:

- **VS Code** (if you have it) — built-in preview, `Ctrl+Shift+V` / `Cmd+Shift+V`.
- **[Dillinger](https://dillinger.io/)** — paste-and-render, no install, works offline-ish (client-side).
- **[StackEdit](https://stackedit.io/)** — similar, plus can export to HTML/PDF.
- **`glow` (CLI)** — `glow docs/mms_report_client_explained.md` renders nicely straight in your terminal with syntax highlighting, if you'd rather stay in the shell.
- **GitHub/GitLab** — if this repo is pushed anywhere, its web UI renders `.md` natively.

Or just ask me to render it as an Artifact and I'll give you a styled HTML
page instead.
