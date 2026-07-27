# `mms_report_client`

Source: `src/features/mms_report_client/`

## 1. Overview

`mms_report_client` connects to **one IED** over MMS, enables every Report Control Block (RCB —
BRCB/URCB) that `ied_model` says exists for it, and turns every report the server pushes into a
normalized `MmsReportRecord` delivered to a caller-registered callback. It never discovers RCBs
over the wire and never re-parses SCL — `ied_model` (SCL-derived, or `ied_model_online_loader`'s
live-discovered equivalent) is the only source of truth for what to subscribe to. JSON
stringification is out of scope — that is `ipc_dispatcher`'s job.

This is one of "The Two Workers" described in the daemon's top-level architecture: the MMS side,
driven entirely by `IedConnection`/`ClientReportControlBlock`/`ClientReport`
(`third_party/include/iec61850_client.h`). It never polls for data — every report reaches this
feature by the server pushing it, triggered by the RCB's own `TrgOps`/`BufTm`/`IntgPd`
configuration on the device. The only "loop" anywhere in this feature is the reconnect
supervisor's state machine, which loops over connection lifecycle events, not data.

Three things run concurrently under the hood: a dedicated supervisor thread that owns
connect/enable/reconnect; libiec61850's own internal report-reader thread that decodes incoming
reports; and a pure-logic "hybrid filter" that decides, per data point, whether a report entry
represents a genuine change worth forwarding or noise that should be silently absorbed.

Public boundary: `src/features/mms_report_client/service/mms_report_client_api.h`. Works with an
`IedModelHandle` in any `AccessMode`, including `IED_MODEL_ACCESS_REPORT_ONLY` — it only ever
calls `IedModel_getReportSubscriptionTargets`, which is always available regardless of mode.

## 2. Public API surface

```
MmsReportClientConfig_defaults(config)
MmsReportClient_create(iedModel, host, port, config, &err)  -> handle
MmsReportClient_setReportCallback(handle, cb, userParam)
MmsReportClient_setConnectionStateCallback(handle, cb, userParam)   // optional
MmsReportClient_setRcbStatusCallback(handle, cb, userParam)         // optional
MmsReportClient_start(handle)   -> non-blocking, starts the supervisor thread
MmsReportClient_stop(handle)    -> blocking, joins the supervisor thread
MmsReportClient_destroy(handle)
MmsReportClient_destroyReportRecord(record)   // caller must call this on every delivered record
```

Contract notes:
- `iedModel` is **borrowed** — the caller (usually `orchestration`) owns it and must keep it alive
  for the client's whole lifetime.
- `host`/`port` are caller-supplied — this codebase has no SCL `<ConnectedAP>` IP-address parsing.
- `config` may be `NULL` (defaults to `MmsReportClientConfig_defaults`). `acseAuthPassword` inside
  it is borrowed only for the duration of `_create` — the handle takes its own owned copy
  (`ownedAuthPassword`).
- `MmsReportClient_start` reads `IedModel_getReportSubscriptionTargets` exactly once
  (`client->targets`) and builds `client->memberRefCache` exactly once (`buildMemberRefCache`).
  Both are built once and reused across every reconnect — nothing about the RCB list, the
  reference/type/semantics side-tables, or the value-diff cache itself is rebuilt on reconnect.
  Returns an error only for synchronous setup failures (empty target list, thread-create
  failure) — connection failures are handled by the reconnect loop, not returned here.
- The report callback delivers ownership of the `MmsReportRecord*` to the caller — it must
  eventually call `MmsReportClient_destroyReportRecord` on it. Not safe to call
  `MmsReportClient_stop`/`_destroy` from within the report callback (deadlock).
- `MmsReportClient_stop`/`_destroy` are safe to call more than once, or on an
  already-stopped/never-started client (no-op).

## 3. Per-file breakdown

### `service/mms_report_client_api.h` / `.c`

Public API plus one-time setup. `MmsReportClient_create` allocates the handle, copies
`host`/config, takes an owned copy of `acseAuthPassword`, and delegates connection-object creation
to `MmsReportClientConnection_create`. `MmsReportClient_start` resolves the RCB target list and
calls `buildMemberRefCache` before starting the supervisor thread via
`MmsReportClientConnection_start`.

`buildMemberRefCache` is called once, before the supervisor thread ever touches the network. For
every RCB target, purely from the already-parsed SCL model (never over the wire), it builds one
`MmsReportClientMemberRefCacheEntry` that stays alive for the client's whole lifetime:

1. **`memberReferences`** — the dataset's ordered member reference strings
   (`IedModel_getDataSetMemberReferences`), or, if the RCB has no configured dataset at all
   (`datSet="Dyn"` in SCL terms, `target->datasetReference == NULL`), every FC=ST/MX leaf under
   the RCB's own LN (`IedModel_getReportableAttributeReferencesForLogicalNode`) — the same list
   `enableOneTarget` uses to actually create the dynamic dataset on connect, computed once here so
   both sides stay in sync.
2. **`memberLeafReferences`/`memberLeafCounts`** (Gap-4 structure decomposition) — for each raw
   dataset member that is itself a structured attribute (e.g. a DPC `Pos` with sub-attributes
   `stVal`/`q`/`t`), the ordered list of leaf reference strings underneath it
   (`IedModel_getDataSetMemberLeafReferences`). A flat member gets `NULL`/`0` — nothing to
   decompose.
3. **`memberLeafWireTypes`** — parallel to `memberLeafReferences`: each decomposed leaf's expected
   (SCL-declared) `DataAttributeType` (`IedModel_getDataSetMemberLeafWireTypes`). Still populated
   at build time but **not currently consulted** as a decomposition gate — see §3's
   `collectCandidates` below for why.
4. **`leafSemantics`** — parallel to `lastForwardedValues` (same slot indexing): each leaf's
   `IedModelDaSemantic` (`IED_MODEL_DA_SEMANTIC_DBPOS` or `_NONE`), resolved once via
   `IedModel_getDataSetMemberSemantics`/`_getDataSetMemberLeafSemantics`. Lets `ipc_dispatcher`
   attach a descriptive Dbpos label without guessing from the wire bitstring alone. `NULL` for the
   dynamic-RCB (`datSet="Dyn"`) case — a known v1 gap. Never reset on reconnect (a DA's real SCL
   type doesn't change across reconnects).
5. **`leafSlotOffsets`/`totalLeafSlots`** — flattens the whole RCB into one contiguous array of
   "leaf slots": a non-decomposed member occupies 1 slot, a decomposed member occupies
   `memberLeafCounts[i]` consecutive slots.
6. **`lastForwardedValues`** — `calloc`'d to `totalLeafSlots`, all `NULL`. This is the value-diff
   cache the hybrid filter reads/writes (§3, `mms_report_client_usecases.c`). Each slot is
   populated exactly once, on its first-ever observation, and preserved for the client's whole
   lifetime.
7. **`everPopulated`** — a single `bool`, set once at the end of the first report this RCB ever
   processes. Gates a one-time `stderr` warning if a cache slot is unexpectedly `NULL` after this
   RCB was already populated once — has no effect on forward/drop decisions.
8. **`lastEntryId`** — `NULL` until the first report carrying an EntryID arrives. Backs EntryID
   resumption (§3, connection layer) for buffered RCBs.

`linkedListToStringArray`/`linkedListToWireTypeArray`/`linkedListToSemanticArray` are small local
helpers that drain a `LinkedList` of heap-boxed values into a flat owned array, freeing the list
shell either way.

### `domain/mms_report_client_types.h`

All structs — the actual domain model. Domain vocabulary here **is** libiec61850's MMS-client
reporting vocabulary (`MmsValue`, `ReasonForInclusion`, `IedClientError`, `IedConnection`) — same
convention as `ied_model`'s domain layer using `IedModel`/`FunctionalConstraint` directly: this
data genuinely is the feature's domain, not swappable infrastructure.

- **`MmsReportEntry`** — one dataset member's value in a received report: owned `reference`/
  `value` (deep clones taken before the originating `ClientReport` is invalidated), `reason`
  (carried through as informational metadata only, never a filtering signal), owned
  `previousValue` clone (whatever was cached for this wire position immediately before this
  report overwrote it — `NULL` means either this is the position's genuine first-ever report, or
  it has no cache slot at all), and `semantic` (`IED_MODEL_DA_SEMANTIC_DBPOS` if this leaf's real
  SCL `bType` is `Dbpos`, else `_NONE` — lets `ipc_dispatcher` attach a descriptive label without
  guessing from the wire type).
- **`MmsReportClientMemberRefCacheEntry`** — the single most important struct in the feature
  (built once by `buildMemberRefCache`, described field-by-field above): `rcbReference`,
  `memberReferences`/`memberCount`, `memberLeafReferences`/`memberLeafCounts`,
  `memberLeafWireTypes`, `leafSlotOffsets`/`totalLeafSlots`, `lastForwardedValues`,
  `leafSemantics`, `everPopulated`, `lastEntryId`.
- **`MmsReportClientDedupEntry`** — one owned `(reference, value)` pair kept by the cross-RCB dedup
  cache. Deliberately not `MmsReportEntry` itself (which also carries a `reason` this comparison
  ignores).
- **`MmsReportClientCrossRcbDedupCache`** — a deep copy of the last record this client actually
  forwarded, from *any* RCB. `rcbReference == NULL` means nothing forwarded yet.
- **`MmsReportRecord`** — one fully-decoded, fully-owned report: `rcbReference`, `buffered`,
  `rptId`, `hasEntryId`/`entryId`, `hasTimestamp`/`timestampMs`, `hasSeqNum`/`seqNum`, and the
  `entries` array. Delivered to the caller's callback; the caller owns it afterward.
- Callback typedefs: `MmsReportClientCallback`, `MmsReportClientConnStateCallback`,
  `MmsReportClientRcbStatusCallback`.
- **`MmsReportClientConfig`** — `connectTimeoutMs`/`requestTimeoutMs` (0 = library default),
  `reconnectInitialDelayMs` (default 1000), `reconnectMaxDelayMs` (default 30000),
  `acseAuthPassword` (borrowed at the config-struct level, `NULL` = no ACSE auth).
- **`struct sMmsReportClientHandle`** — the internal representation, defined here (not behind a
  separate internal header) so every file in the feature has field access; opacity is enforced by
  which header is exposed (`service/mms_report_client_api.h` is the only public one). Holds the
  borrowed `iedModel`, owned `host`/config/auth password, owned `IedConnection`, owned `targets`
  and `memberRefCache` lists, the cross-RCB dedup cache, the three optional callbacks, reconnect
  supervisor state (`stopRequested`/`connectionLostSignal`/`supervisorExited`/`wakeSignal`
  semaphore/`supervisorThread`/`currentBackoffMs`), and `memberRefCacheLock` (a binary `Semaphore`
  guarding every `lastForwardedValues`/`lastEntryId` access).

### `domain/mms_report_client_usecases.h` / `.c`

Pure logic — no `ClientReport`/`IedConnection` awareness at all; that's entirely the data layer's
job. Takes plain arguments (strings, `MmsValue*` arrays, `ReasonForInclusion` arrays) rather than
the opaque `ClientReport` type, specifically so it stays unit-testable (`ClientReport` has no
public constructor, `MmsValue` does).

**`valuesAreSemanticallyEqual`** — the real value comparison the hybrid filter uses.
`MmsValue_equals` (libiec61850) is a raw, byte-exact `memcmp`, which is correct for most types but
wrong for two that show up constantly in real report datasets:
- **`MMS_UTC_TIME`** — the last of its 8 bytes is a `TimeQuality` flag (leap-second-known /
  clock-failure / not-synchronized / accuracy), not part of "when did this happen." It can
  legitimately wobble (e.g. a device's clock-sync state settling right after a reconnect) even
  though the millisecond timestamp is unchanged. Compared instead via `MmsValue_getUtcTimeInMs` —
  the same accessor `ipc_dispatcher`'s own value codec already uses to render this type, so "same
  JSON output" correctly implies "same by this filter."
- **`MMS_BIT_STRING`** (the wire encoding for CODEDENUM/Dbpos/Tcmd-style status points) —
  `MmsValue_equals` `memcmp`s the whole underlying buffer, including unused padding bits in the
  last byte. Real device firmware is commonly inconsistent about zero-padding those across
  different report-generation code paths (e.g. a GI-triggered read vs. a live-change report), so
  two values that decode to the identical integer can still fail a raw `memcmp`. Compared instead
  via `MmsValue_getBitStringSize` (guard) + `MmsValue_getBitStringAsInteger` — again the same
  accessors the value codec uses to render it.

Every other type falls through to `MmsValue_equals` unchanged (BOOLEAN/INTEGER/UNSIGNED/FLOAT/
STRING already compare semantically correctly). `goose_subscriber_usecases.c` has an
independently-duplicated copy of this exact function (this codebase's no-shared-domain-layer
convention).

**`MmsReportClientUseCases_isDuplicateValue(cached, newValue)`** — `false` if either side is
`NULL` (never dereferences), otherwise delegates to `valuesAreSemanticallyEqual`.

**`updateValueDiffCache`** — mutates `lastForwardedValues[slot]` in place: deletes the old clone
(if any) and clones `newValue` into its place. No-op/bounds-checked if `memberRefCache`/slot is
invalid.

**`shouldForwardAndUpdateCache(memberRefCache, slot, value, reference, outPreviousValue)`** — the
event filter's single decision point, applied once per (possibly decomposed-leaf) candidate:

```
if outPreviousValue: *outPreviousValue = NULL
if slot < 0: return true                    // no cache coverage — always forward, no previousValue
cached = lastForwardedValues[slot]
if outPreviousValue && cached: *outPreviousValue = clone(cached)
if !value: return false                     // this wire position carries no value in this report — never seed/overwrite
if !cached:
    if everPopulated: log "unexpectedly NULL" to stderr   // structurally unexpected past this RCB's first report
    updateValueDiffCache(slot, value)        // bootstrap seed
    return false                             // NEVER forwarded
if isDuplicateValue(cached, value): return false
updateValueDiffCache(slot, value)
return true
```

Two structural properties matter:
- **`reason` (`ReasonForInclusion`, e.g. `DATA_CHANGE`/`GI`/`QUALITY_CHANGE`) is never trusted as
  a bypass of the diff check.** GI and a real-change bit are independent, combinable
  `ReasonForInclusion` bits — a real device can and does set both at once, so trusting `reason`
  alone can bypass bootstrap-suppression. `reason` is carried through on `MmsReportEntry.reason`
  purely as informational metadata.
- **`cached == NULL` is unconditionally treated as "bootstrap: seed but never forward."** This
  makes a genuine first-ever connect's GI snapshot, a foreign client's concurrent GI, or a
  buffered RCB's first-ever redelivery all order-independent — whichever lands first seeds the
  cache and is dropped, whichever lands second is diffed as a duplicate and also dropped. Since
  the cache is never reset after that first population, `cached == NULL` should be structurally
  impossible on any later report for a slot that has a real value — if it happens anyway, this
  function logs it loudly to stderr, gated on `everPopulated` so the true first-ever report never
  trips it.
- A wire position with **no value at all** in a given report (`MmsValue_getElement` can
  legitimately return `NULL` for some index, e.g. on a buffered redelivery) is never treated as a
  bootstrap seed or a real change, and never overwrites a real cached value with `NULL` — the
  cache is left untouched and this position is simply not forwarded for this report.

**The cache is populated once and preserved forever — never reset, on reconnect or otherwise.**
`enableOneTarget` (connection layer) does not reset `lastForwardedValues` on any (re-)enable. A
slot is written exactly twice in its life: once when `updateValueDiffCache` first seeds it
(bootstrap, `cached == NULL`), and again every time a genuinely different value arrives
afterward — it is never set back to `NULL`. This is what makes a reconnect's own fresh GI/
redelivered snapshot diff against the real, preserved last-known value from before the
disconnect: a genuine change made while disconnected forwards with a real `previousValue`; an
unchanged resend is suppressed by the ordinary diff check.

`handle->memberRefCacheLock` guards `lastForwardedValues` (and `lastEntryId`) — held by the
report-reader thread (`onReport` → `MmsReportClientUseCases_buildReportRecord`) on every access,
and by the supervisor thread on every `lastEntryId` read in `enableOneTarget`. Both fields are
genuinely cross-thread now (not just uncontended insurance), since `lastEntryId` is written by the
report-reader thread and read by the supervisor thread on every (re)enable of a buffered RCB.

**`reorderFlattenedToMatchReferences`** — reorders a flattened structure's elements (from
`MmsReportClientUtils_flattenStructure`) so `outReordered[refIdx]` is the value that actually
belongs at `leafReferences[refIdx]`, even when the wire's real element order doesn't match this
daemon's locally-resolved reference order. Two IEC 61850 common data attributes have a *fixed,
unambiguous* wire encoding regardless of CDC — Quality (`"q"`) is always a 13-bit `MMS_BIT_STRING`
(`Quality_toMmsValue`), Timestamp (`"t"`) is always `MMS_UTC_TIME` — so those two are matched by
type (and, for `"q"`, exact bit size, since a CODEDENUM-typed `stVal` such as a DPC's Dbpos is
also `MMS_BIT_STRING` but never 13 bits wide) rather than trusting either order. This is not
"guessing IEC 61850 semantics" (the codebase's own Hard Rule) — Quality's and Timestamp's wire
encodings are standardized and identical across every CDC, unlike e.g. a CODEDENUM's
Dbpos-vs-Tcmd ambiguity, which is never guessed anywhere in this codebase. Whatever's left (the
CDC-specific value field(s), e.g. `stVal`) is assigned positionally among the remaining,
unconsumed wire slots, in order. Returns `false` (caller falls back to the raw, non-decomposed
entry, same as an outright count mismatch) if a `"q"`/`"t"` reference exists but no matching-typed,
not-yet-claimed wire element is found for it, or if positional fill-in runs out of slots.

**`collectCandidates`** — walks every raw dataset position; if it's a decomposed structure,
flattens it (`MmsReportClientUtils_flattenStructure`) and, only if the flattened count matches
`memberLeafCounts[i]`, reorders it via `reorderFlattenedToMatchReferences` before zipping each
leaf against `memberLeafReferences[i]`. **The per-leaf expected-vs-actual type cross-check
(`decomposedLeafTypesMatch`, via `IedModel_dataAttributeTypeMatchesMmsType`) that used to gate
this is no longer consulted here** — `memberLeafWireTypes` is still populated at build time but
removing the gate exposed no behavior change beyond no longer rejecting genuine decompositions
that happened to fail the type check. A count mismatch, flatten failure, or unresolvable `q`/`t`
reorder falls back to the raw, non-decomposed entry for that position.

**`splitReference`/`GroupAnchor`/`resolveGroupAnchor`** — quality-pairing support. Every `"q"`-named
candidate's own `"$"`-prefix anchors a group scope; every candidate (including `"q"` itself)
resolves to the *longest* anchor it is genuinely nested under (an ancestor walk, not a single
`"$"`-strip — required for deeply nested CONSTRUCTED-DA chains, e.g. a CMV's `"...phsA$cVal$mag$f"`
resolving to the `"...phsA"` anchor several segments up, not its own last segment).

**`buildEntries`** — builds the final, flat entry list for one report in three phases:
1. **`collectCandidates`** — every (possibly Gap-4-decomposed) leaf across every raw dataset
   position, undecided.
2. **Per-candidate filter + group-extension** — runs `shouldForwardAndUpdateCache` on every
   candidate (this is where `previousValue` is populated for every candidate, regardless of its
   own forward/drop outcome — a candidate dragged in later still needs its own previous value).
   Then a group-aware pass: a candidate that didn't individually qualify still forwards if *any*
   other candidate resolving to the same anchor does — this is what keeps a value and its quality
   sibling (or any other sibling DA under the same DO/SDO) traveling together even though quality
   rarely changes on its own; without it, quality gets silently dropped by its own diff-check on
   every report after the first, orphaning `ipc_dispatcher`'s own quality-pairing (which only
   pairs entries present in the same record). A candidate with **no value of its own** in this
   report is excluded from being dragged in (skipped by the group-extension pass entirely) —
   dragging it in would let phase 3's `updateValueDiffCache` overwrite a real cached value with
   `NULL`, the exact hazard `shouldForwardAndUpdateCache`'s own `!value` branch already guards
   against for the solo path.
3. **Emit** — every forwarded candidate's cache slot is (re-)updated (a no-op if phase 2 already
   updated it) and cloned into the final `MmsReportEntry` array via `appendEntry`; every
   non-forwarded candidate's `previousValue` clone (always taken in phase 2 regardless of outcome)
   is freed here instead.

`memberRefCache->everPopulated` is flipped to `true` only after the whole report is processed
(every candidate was checked against its value *before* this report) — this keeps the true
first-ever report's own from-empty seeding silent while making every report from then on treat an
unexpected `NULL` slot as a bug worth logging.

**`MmsReportClientUseCases_buildReportRecord`** — the public entry point: builds a fully-owned
`MmsReportRecord` from already-extracted report fields (scalar fields cloned/duplicated directly,
`entries` built via `buildEntries`). `memberRefCache` may be `NULL` (an RCB with no resolvable
dataset), which disables both Gap-4 decomposition and the value-diff filter for that call — every
entry becomes a plain 1:1 passthrough, always forwarded, no `previousValue`.

**Cross-RCB dedup** — `MmsReportClientUseCases_shouldForwardAcrossRcb(cache, rcbReference,
entries, entryCount)` catches the case where two *different* redundant RCB instances (e.g.
`urcbA01`/`urcbB01` on the same LN/dataset, common for multi-client redundancy) report the exact
same event nearly simultaneously — each survives its own per-RCB filter independently, so this
second cache (keyed by the last forwarded record's content, from any RCB) suppresses the second
one. A record is a duplicate only if the cache has prior content, `rcbReference` differs from the
one that produced it, and every `(reference, value)` pair matches positionally
(`crossRcbEntriesEqual`, itself using `valuesAreSemanticallyEqual`). Any other case (nothing
cached yet, same `rcbReference` as before, or genuinely different content) replaces the cache's
content and forwards. `NULL`-safe on `cache` (always forwards). Persists across reconnects —
deliberately never reset in `enableOneTarget`, since this concern is orthogonal to per-RCB resync.

**`MmsReportClientUseCases_computeNextBackoffDelay(currentDelayMs, initialMs, maxMs)`** — pure
doubling-with-cap: `currentDelayMs == 0` returns `initialMs`; otherwise doubles (via `uint64_t` to
avoid overflow) and caps at `maxMs`.

**`MmsReportClientUseCases_buildWireMemberReferences`**/`convertToWireMemberReference` — converts
this codebase's `"$"`-joined reference form (`LD/LN$FC$DO[$SDO...]$DA`) into
`IedConnection_createDataSet`'s required dot/bracket form (`LD/LN.DO[.SDO...].DA[FC]`) — used only
for dynamically-created datasets. A malformed input (fewer than 3 `"$"`-segments) is silently
skipped, never a partial/best-effort string.

**Destructors**: `MmsReportClientUseCases_freeReportRecord`, `_destroyMemberRefCacheEntry` (frees
every array in a cache entry, including `lastForwardedValues`' cloned `MmsValue*`s and
`lastEntryId`), `_destroyCrossRcbDedupCache` — all `NULL`-safe.

### `data/mms_report_client_connection.c` / `.h`

All `IedConnection`/libiec61850 third-party integration: connection creation, the reconnect
supervisor thread + semaphore, and the per-RCB enable sequence. Deliberately not unit-tested in
depth — proven end-to-end instead (a live `IedConnection` can't be meaningfully faked in a
hermetic unit test).

**`MmsReportClientConnection_create`** — creates the `IedConnection`
(`IedConnection_createEx(NULL, true)` — library-internal reception thread, no manual `tick()`
polling), applies timeouts if configured, applies ACSE password auth once via
`MmsReportClientAuth_configurePasswordAuth` (covers every subsequent reconnect since the same
`IedConnection` object is reused), and installs `onStateChanged`. Does not connect yet.

**`onStateChanged`** — fires while an internal libiec61850 state mutex is held; must never block
or call `IedConnection_getState`/any blocking `IedConnection_*` function. Sets
`handle->connectionLostSignal = true` only on `IED_STATE_CLOSED` while not already stopping,
invokes the optional connection-state callback (mapped to `MmsReportClientConnState`), and
unconditionally posts `wakeSignal`. All the real work happens on the supervisor thread.

**`supervisorLoop`** — one dedicated `Thread` (via `hal_thread.h`), running until `stopRequested`:

```
while (!stopRequested):
    IedConnection_connect(host, port)
    if connected:
        connectedAtMs = now()
        enableAllTargets()                        // enable every RCB
        loop:                                       // "connected phase" — consume every wake
            wait on wakeSignal
            if stopRequested: break outer
            if connectionLostSignal: clear it, break inner   // fall through to backoff
            else: spurious wake — keep waiting
        if (now() - connectedAtMs) >= 5000ms:
            currentBackoffMs = 0                    // only reset backoff after a STABLE connection
    if stopRequested: break
    delay = computeNextBackoffDelay(currentBackoffMs, initial=1000, max=30000)
    interruptibleSleep(delay)
```

The connected-phase inner loop only treats a genuine `connectionLostSignal == true` as "go
reconnect" — never a bare wake. `onStateChanged` posts `wakeSignal` on *every* state transition,
not just loss: a single successful `IedConnection_connect()` drives `CONNECTING` then `CONNECTED`,
each posting once, so by the time `supervisorLoop` first reaches its wait, wakes are already
pending. Treating any wake as "reconnect" would re-run `enableAllTargets()` a second time for one
real connect, with no connection actually lost — a duplicate, fully redundant RptEna/enable cycle
per RCB.

`currentBackoffMs` only resets to 0 if the connection just lost had been up for at least
`MMS_REPORT_CLIENT_STABLE_CONNECTION_MS` (5000ms) — a debounce so a flaky link that connects then
immediately bounces escalates its backoff instead of retrying at the ~1s initial tier forever
(each retry re-triggers the full reset+GI enable cycle, giving the value comparison logic more
chances to fire in a tight burst).

**`interruptibleSleep`** — sleeps in 100ms chunks so `MmsReportClientConnection_stop`'s bounded
wait for the supervisor to exit doesn't have to wait out a full backoff delay (`hal_thread.h` has
no interruptible sleep primitive).

**`enableAllTargets`** — visits every cached target, calling `enableOneTarget` for each, checking
`stopRequested` on every iteration so a concurrent stop doesn't cascade a long, wasted string of
`IED_ERROR_CONNECTION_LOST` failures through every remaining RCB on a device with many RCBs. Owns
a fresh `dynamicDatasetCache` (LN reference → generated dataset name) per connect cycle, discarded
at the end — never carried across reconnects, since the `@`-scoped datasets it names don't survive
one either.

**`enableOneTarget`** — called once per target, every connect and every reconnect:
1. `IedConnection_getRCBValues` — fetch the RCB object. On failure, logs and fires the optional
   RCB-status callback with `enabled=false`.
2. `IedConnection_installReportHandler` — installed **before** enabling, so no report can arrive
   with no handler attached.
3. Build the enable mask, starting with `RCB_ELEMENT_RPT_ENA | RCB_ELEMENT_GI`.
4. **Buffered-RCB EntryID opt-in**: if `target->buffered`, reads the RCB's current `OptFlds`
   (`ClientReportControlBlock_getOptFlds`) and, only if `RPT_OPT_ENTRY_ID` isn't already set, ORs
   it in and adds `RCB_ELEMENT_OPT_FLDS` to the mask — never clobbers any other OptFlds bit, and
   only writes it once (not on every reconnect) since OptFlds isn't expected to reset itself
   across associations the way `RptEna` does. This exists because a real device was found sending
   zero EntryID across every single report — without this bit, EntryID resumption below is
   structurally impossible against such a device, regardless of how correct the resumption logic
   itself is, since there is never anything to cache and resume from.
5. **DatSet**: always explicitly (re-)set, never relying on a server-side default dataset. If
   `target->datasetReference` is `NULL` (SCL declared no `datSet`, `datSet="Dyn"`), falls back to
   `getOrCreateDynamicDataset` (below). If a dataset reference resolves either way, adds it to the
   mask via `RCB_ELEMENT_DATSET`.
6. `ClientReportControlBlock_setRptEna(rcb, true)` + `ClientReportControlBlock_setGI(rcb, true)` —
   **GI is requested on every enable, unconditionally**, purely to force an immediate,
   deterministic snapshot to diff the cache against — never trusted or forwarded on its own
   merits. On a genuine first-ever connect this snapshot lands against the still-all-`NULL` cache
   and is bootstrap-suppressed like any other first observation. On every reconnect after that,
   the same snapshot instead diffs against the real, preserved last-known value from before the
   disconnect — a genuine change made while disconnected forwards with a real previous value; an
   unchanged resend is suppressed by the ordinary diff check. Without GI, a device with no
   periodic integrity reporting and no coincidental traffic at enable time could leave a real
   change undetected simply because nothing prompted the device to report it yet.
   `TrgOps`/`BufTm`/`IntgPd`/`ConfRev` are never touched — left exactly as the device's own SCL
   config has them.
7. **EntryID resumption**: for a buffered RCB, looks up its cache entry and, under
   `memberRefCacheLock`, reads `lastEntryId`. If set, calls `ClientReportControlBlock_setEntryId`
   and adds `RCB_ELEMENT_ENTRY_ID` to the mask, so a reconnect resumes delivery from where this
   client left off instead of the server redelivering its entire unacknowledged backlog on every
   `RptEna` transition. `ClientReportControlBlock_setEntryId` itself is a local, synchronous
   struct mutation (clones the value internally), safe to call while holding the lock, unlike the
   network call below. On the very first-ever enable (`lastEntryId` still `NULL`) this is a no-op
   — full backlog resume, same as before this mechanism existed.
8. `IedConnection_setRCBValues(..., mask, true)` — the actual enable write. If it fails **and**
   `RCB_ELEMENT_ENTRY_ID` was set, retries once with that bit cleared (full resume) rather than
   leaving the RCB unreported — the server may have rejected an EntryID it no longer recognizes
   (its own buffer wrapped past it, or it restarted); IEC 61850 leaves the exact failure mode here
   implementation-defined, so this doesn't guess at it. Any remaining failure uninstalls the
   report handler, fires the RCB-status callback with `enabled=false`, and returns.
9. On success, fires the RCB-status callback with `enabled=true`.

The value-diff cache is deliberately never reset anywhere in this function — on first connect it's
already all-`NULL` from `buildMemberRefCache`; on every subsequent enable, preserving the real
last-known values is the entire point (see §3's `mms_report_client_usecases.c` breakdown).

**`getOrCreateDynamicDataset`** — for an RCB whose SCL declared no `datSet`: creates an
association-scoped dataset (`IedConnection_createDataSet` with an `"@"`-prefixed name, so it is
destroyed automatically when the connection closes — no explicit cleanup, no leak risk across
reconnects) covering every FC=ST/MX leaf attribute under the RCB's own LN, using the member list
already resolved once into `handle->memberRefCache` by `buildMemberRefCache`.
`dynamicDatasetCache` de-dupes by LN reference within one connect cycle — many real IEDs expose
several reserved RCB instances per LN (e.g. `urcbA..urcbJ`) that would otherwise each trigger
their own `createDataSet` call for what is conceptually the same dataset, wastefully consuming the
device's often-small dataset-count budget. Returns `NULL` (caller falls back to the pre-existing
behavior: skip DATSET, let `setRCBValues` fail, log, move on) if no reportable attributes exist
for the LN, no wire-convertible references exist, or dataset creation itself fails.

**`MmsReportClientConnection_start`** — creates `wakeSignal` and `memberRefCacheLock` (both
`Semaphore`s), resets supervisor state, and starts `supervisorLoop` on a new `Thread`.

**`MmsReportClientConnection_stop`** — sets `stopRequested`, closes the connection, posts
`wakeSignal` (to unstick a blocked wait), then busy-waits (20ms polls) for `supervisorExited`
before uninstalling every target's report handler. Safe to call more than once (no-ops if already
`stopRequested`).

**`MmsReportClientConnection_destroy`** — destroys the supervisor `Thread` handle, then the
`IedConnection`, then `wakeSignal`/`memberRefCacheLock`, in that order. `IedConnection_destroy`
can synchronously re-fire `onStateChanged` (it internally closes the connection again even if
already closed), which unconditionally posts `wakeSignal` — so the semaphore must still be alive
when `IedConnection_destroy` runs; destroying it first would leave that callback posting to freed
memory.

### `data/mms_report_client_report_adapter.c` / `.h`

The **only** file in this feature that touches the opaque `ClientReport` type. Installed as the
`ReportCallbackFunction` on `IedConnection`. Fires on libiec61850's internal reception thread
(`IedConnection_createEx(NULL, true)`) — must not block, and must not call
install/uninstallReportHandler or any synchronous `IedConnection_*` function from here (deadlock
risk, per `ReportCallbackFunction`'s documented constraints).

**`MmsReportClientReportAdapter_onReport(parameter, report)`**:
1. Extracts `rcbReference`, `rptId`, looks up `buffered` (`lookupBuffered`, a linear scan of
   `handle->targets` — `ClientReport` doesn't expose buffered-ness directly) and the matching
   `MmsReportClientMemberRefCacheEntry` (`lookupMemberRefCache`, a linear scan of
   `handle->memberRefCache`, both keyed by `rcbReference`).
2. Extracts `dataSetValues`, per-entry `reasons`/`dataReferences` (only if
   `ClientReport_hasDataReference`), `entryId`, `hasTimestamp`, `hasSeqNum` — every extraction
   happens before returning, since `ClientReport_getDataSetValues`/`getDataReference`/`getEntryId`
   are documented as valid only for the lifetime of the `ClientReport`/inside this callback.
3. Under `handle->memberRefCacheLock`, calls `MmsReportClientUseCases_buildReportRecord`, then
   unconditionally updates `fallback->lastEntryId` (clones `entryId` if present) — whether or not
   this report's entries survive the value-diff filter, the EntryID was durably received, so a
   later reconnect must resume from here, not re-request everything the server still has
   buffered.
4. Frees the temporary `reasons`/`dataReferences` arrays. If record-building failed (OOM), drops
   the report.
5. `record->entryCount > 0` means it survived the per-RCB hybrid filter. A second, independent
   gate — `MmsReportClientUseCases_shouldForwardAcrossRcb` — then checks it isn't an exact
   duplicate of what a *different* RCB on this same client just forwarded. Only if both checks
   pass does `handle->reportCallback` fire (transferring ownership of `record`); otherwise the
   report adapter frees it itself, since the callback (which would otherwise own destroying it)
   never ran.

### `data/mms_report_client_auth.c` / `.h`

Isolates ACSE password authentication wiring behind
`MmsReportClientAuth_configurePasswordAuth(conn, password)`: retrieves the `IedConnection`'s
`MmsConnection`, then its `IsoConnectionParameters`, creates an `AcseAuthenticationParameter` with
`ACSE_AUTH_PASSWORD`, and attaches it. `NULL`-safe (no-op if `conn` or `password` is `NULL`). Must
be called on a fresh, not-yet-connected `IedConnection`, before its first `IedConnection_connect()`
— auth is negotiated at MMS association time.

Unlike `scl_bootstrap` (which creates a fresh `IedConnection` per candidate/attempt and needs a
retry dance), `mms_report_client` reuses the **same** `IedConnection` object across every reconnect
attempt, so calling this once at `MmsReportClientConnection_create` time covers every subsequent
reconnect too — applied unconditionally from the very first attempt, no auth-then-retry logic
needed, since this feature always targets one already-known IED, never a blind multi-candidate
scan. Same unresolved-ownership caveat as `scl_bootstrap_auth.h`: whether
`IsoConnectionParameters_setAcseAuthenticationParameter` takes ownership of the
`AcseAuthenticationParameter` is undocumented in `iso_connection_parameters.h`.

### `utils/mms_report_client_utils.c` / `.h`

Small, reusable third-party (`MmsValue`) helpers shared by the data layer (report adapter) and the
domain layer (usecases). No `ClientReport`/`IedConnection` awareness — just `MmsValue`/plain-array
cloning.

- **`MmsReportClientUtils_cloneMmsValueArray(dataSetValues, count)`** — clones the first `count`
  elements of an `MMS_ARRAY`/`MMS_STRUCTURE` into a freshly allocated owned array.
- **`MmsReportClientUtils_cloneReasonArray(src, count)`** — plain `memcpy`-based clone of a
  `ReasonForInclusion` array.
- **`MmsReportClientUtils_safeStringDup(s)`** — `NULL`-safe `strdup`.
- **`MmsReportClientUtils_flattenStructure(value, outLeafCount)`** — recursively flattens a
  (possibly nested) `MMS_STRUCTURE` into an ordered array of its terminal (non-structure) leaf
  elements, depth-first in element order — matching the same convention
  `IedModel_getDataSetMemberLeafReferences` uses over the DOType/DAType model tree
  (correspondence between the two is purely positional, documented on that function, not verified
  by type here — see `reorderFlattenedToMatchReferences` in the usecases layer for how a
  same-count-different-order mismatch is corrected). If `value` isn't itself `MMS_STRUCTURE`,
  returns a single-element array containing `value` unchanged. Returned pointers are **borrowed**
  from `value` — caller must `MmsValue_clone` before `value`'s owning `ClientReport` becomes
  invalid.

## 4. Threading & concurrency model

Two threads matter for this feature:
- **The supervisor thread** (`supervisorLoop`) — owns connect/enable/reconnect/backoff. The only
  writer of `lastEntryId` from the reader side... actually the only *reader* of `lastEntryId` on
  the enable path; the report adapter (below) is its writer.
- **libiec61850's own internal report-reader thread** — created by
  `IedConnection_createEx(NULL, true)`, decodes incoming reports and invokes
  `MmsReportClientReportAdapter_onReport`.

`handle->memberRefCacheLock` (a binary `Semaphore`) guards every `lastForwardedValues` slot access
and every `lastEntryId` access: held by the report-reader thread on every
`onReport`→`buildReportRecord` call (mutating `lastForwardedValues` in place) and on the
`lastEntryId` update immediately after; held by the supervisor thread on every buffered-RCB
`lastEntryId` read in `enableOneTarget`. `onStateChanged` itself fires on an internal libiec61850
state mutex and must never block or call a blocking `IedConnection_*` API — it only sets plain
flags and posts `wakeSignal`, deferring all real work to the supervisor thread.

`handle->targets` and `handle->memberRefCache` are read-only after `MmsReportClient_start` — built
once, never mutated as a whole list afterward (only the per-entry cache fields inside
`memberRefCache` entries mutate, under the lock above). `dynamicDatasetCache` inside
`enableAllTargets` is thread-confined to the supervisor thread (stack-local per connect cycle).
`handle->crossRcbDedupCache` is only ever touched from the report-reader thread (inside
`MmsReportClientReportAdapter_onReport`), so it needs no separate lock.

## 5. Known limitations / deliberate scope boundaries

- **`TrgOps`/`BufTm`/`IntgPd`/`ConfRev` are never written** — only `RptEna`/`GI`/`DatSet`, and
  (for buffered RCBs) `OptFlds`/`EntryID`, are ever set. Every other RCB attribute is left exactly
  as the device's own SCL/engineering-tool configuration has it.
- **Dynamic dataset creation has no chunking against a device's `maxAttributes` cap** — an LN with
  more reportable leaves than the cap fails `createDataSet` for that LN outright and falls back to
  the pre-existing failure mode (DATSET left unset, `setRCBValues` fails, logged, RCB skipped).
- **No handling of a device's total dataset-count cap being smaller than its unique-LN count** —
  de-duplication is per-LN scope only, not per-LDevice.
- **`memberLeafWireTypes`/`IedModel_dataAttributeTypeMatchesMmsType` are populated but not
  consulted** as a Gap-4 decomposition gate — decomposition instead trusts a count match plus
  `reorderFlattenedToMatchReferences`'s type-based correction for `"q"`/`"t"` only; other leaves
  are still assigned positionally.
- **A device sending zero EntryID under its current OptFlds configuration** makes EntryID
  resumption structurally inert until this client's proactive `RPT_OPT_ENTRY_ID` OR-in (see
  `enableOneTarget`) actually changes the device's behavior.
- **A "reason that a value transiently changed and changed back within one buffer interval" is
  unrecoverable** — since `reason` is never trusted as a filtering bypass, a round-trip like that
  is invisible to the diff-based filter (only the net result at report time is ever compared).
- **The cross-RCB dedup cache only ever compares against the single most recently forwarded
  record**, not a longer history — three or more redundant RCB instances reporting the same event
  in quick succession only dedupe pairwise against whichever forwarded immediately before.

## 6. Cross-feature dependencies

- **`ied_model`** — the only source of RCB targets (`IedModel_getReportSubscriptionTargets`),
  dataset member references/leaf references/leaf wire types/semantics
  (`IedModel_getDataSetMemberReferences`/`_getDataSetMemberLeafReferences`/
  `_getDataSetMemberLeafWireTypes`/`_getDataSetMemberSemantics`/`_getDataSetMemberLeafSemantics`),
  and the reportable-attribute list for dynamic RCBs
  (`IedModel_getReportableAttributeReferencesForLogicalNode`). Never re-discovers RCBs over the
  wire, never re-parses SCL — every one of these calls is purely local, reading structure already
  resolved into the `IedModelHandle` this feature borrows.
- **`ipc_dispatcher`** — the consumer of every delivered `MmsReportRecord`. JSON stringification,
  quality-pairing (a second, independent pairing pass mirroring this feature's own group-extension
  logic), and Dbpos label rendering (from `MmsReportEntry.semantic`) all happen there, not here.
- **`orchestration`** — the sole caller of this feature's public API: creates the client, registers
  the two callbacks it wires to `IpcDispatcher_onMmsReport`, and calls `MmsReportClient_start`/
  `_stop` as part of its own fail-hard sequencing (tearing this client down first, in reverse
  order, if a later stage such as `goose_subscriber` start fails).

## 7. Tests

`tests/mms_report_client/` (unit, Unity framework):
- `test_mms_report_client_api.c` — `MmsReportClient_create` argument validation (`NULL`
  `iedModel`/`host`, non-positive `port`) and success path; `MmsReportClient_start` failing on an
  empty target list; `_stop`/`_destroy` no-op on a `NULL` client; `MmsReportClientConfig_defaults`
  matches its documented values and is `NULL`-safe; `acseAuthPassword` is taken as an owned copy,
  not aliased to the caller's buffer, and stays `NULL` when not configured.
- `test_mms_report_client_usecases.c` (the largest file, ~1490 lines) — covers, by group:
  `isDuplicateValue`'s `MMS_UTC_TIME`/`MMS_BIT_STRING` semantic-equality special cases plus
  type-mismatch/boolean-unchanged cases; `buildReportRecord`'s scalar-field copying, deep-copy
  independence, server-vs-fallback reference resolution, EntryID copying; the hybrid filter's
  GI/integrity/data-change/unknown-reason forwarding and suppression behavior including the
  first-ever-value bootstrap case; Gap-4 decomposition (successful decompose, count-mismatch
  fallback, decomposition proceeding even when wire-types are present); quality-pairing/
  group-extension (value drags quality, quality drags value, dragged-along siblings' own
  `previousValue`, both-unchanged suppression, ungroupable singleton fallback, decomposed-group
  drag, deeply-nested CMV anchor resolution several ancestor levels up, not overreaching past an
  unrelated ancestor); the never-reset cache design (first report seeds + sets `everPopulated`, a
  simulated reconnect's genuine change forwards with a real previous value, an unchanged resend is
  suppressed); cross-RCB dedup (first-content, same-RCB-repeat-still-forwards,
  different-RCB-duplicate-suppressed, different-content-forwards, suppression not disturbing the
  baseline, `NULL`-cache no-op); `buildWireMemberReferences`'s dollar-to-dot/bracket conversion,
  nested-segment joining, malformed/`NULL`-input skipping; `computeNextBackoffDelay`'s
  initial/doubling/cap/overflow-safety behavior.
- `test_mms_report_client_utils.c` — `cloneMmsValueArray`/`cloneReasonArray` `NULL`/empty-input
  handling and independent-deep-copy behavior; `safeStringDup`; `flattenStructure` on `NULL`, a
  scalar (single-element passthrough), a flat structure, and a nested structure (recursion to
  terminal leaves).

`integration_tests/mms_report_client/` (E2E, real `ied_simulator` "Reporter1" IED, plain
TCP/loopback MMS — no `sudo` needed):
- `test_dataChangeOnServer_triggersReportWithNewValue` — a real value change on the server reaches
  the report callback.
- `test_authRequired_correctPassword_connectsAndEnablesRcb` /
  `test_authRequired_wrongPassword_neverConnects` — ACSE password auth end to end, both directions.
- `test_dynamicDataset_createdOnEnable_andReportsRealChange` — a `datSet="Dyn"` RCB gets a real
  dynamically-created dataset and reports a genuine change through it.
- `test_reconnect_afterServerRestart_redeliverySuppressed_thenChangeReportsPreservedPreviousValue`
  — the never-reset cache design against a real reconnect: an unchanged redelivery is suppressed,
  and a genuine change reports with the real, pre-disconnect previous value.
- `test_crossRcbDuplicateContent_onlyOneOfTwoIdenticalRcbsReachesCallback` — two redundant RCB
  instances reporting the same event; only one reaches the callback.
- `test_secondReconnectWithNoNewChanges_doesNotRedeliverBacklog` — EntryID resumption end to end:
  accumulates a multi-entry alternating backlog while disconnected, reconnects once (backlog must
  be delivered), forces a second reconnect with zero new changes, and asserts nothing is
  redelivered.
