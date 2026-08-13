# `goose_subscriber`

## 1. Overview

`goose_subscriber` subscribes to every GOOSE Control Block (GoCB) discovered on one IED and
delivers normalized `GooseSubscriberRecord`s to a caller-registered callback. It is one of "The
Two Workers" described in the root `CLAUDE.md` — the event-driven GOOSE sniffer, paired with
`mms_report_client`'s BRCB-driven MMS worker. Reception is handled entirely by libiec61850's
`GooseReceiver`/`GooseSubscriber` (`third_party/include/goose_receiver.h`,
`goose_subscriber.h`), which opens a raw AF_PACKET-style socket via libiec61850's own
`hal_ethernet` platform-abstraction layer — this feature never touches libpcap/Npcap or hand-rolls
GOOSE parsing (Hard Rule: "libiec61850 is mandatory for all protocol handling").

Subscription targets come exclusively from `ied_model` (`IedModel_getGooseSubscriptionTargets`) —
this feature never re-parses SCL and never discovers GoCBs over the wire (Hard Rule: "No
over-the-wire tree discovery"). It works under every `IedModelHandle` `AccessMode`, including the
lowest tier `IED_MODEL_ACCESS_REPORT_ONLY`, because `IedModel_getGooseSubscriptionTargets` is
always available regardless of mode.

Public boundary: `src/features/goose_subscriber/service/goose_subscriber_api.h`. Every public
function is named `GooseSubscription_*`, **not** `GooseSubscriber_*` — libiec61850 already owns
`GooseSubscriber_*` as its own opaque-type API, and colliding with it would be both confusing and
a linker hazard. Other features (`ipc_dispatcher`, `orchestration`) must only ever include this
one header, never reach into `domain/`/`data/`/`utils/` directly.

## 2. Public API surface

All declared in `service/goose_subscriber_api.h`:

- **`GooseSubscriberConfig_defaults(GooseSubscriberConfig* config)`** — fills `config` with
  `livenessPollMs = 0` (auto-derive from observed `TimeAllowedToLive`, see §3). NULL-safe no-op.
- **`GooseSubscription_create(IedModelHandle iedModel, const char* interfaceId, const
  GooseSubscriberConfig* config, GooseSubscriberError* outError)`** — allocates the handle only;
  opens no socket. `iedModel` is borrowed (caller retains ownership, must outlive
  `GooseSubscription_destroy`). `interfaceId` (e.g. `"eth0"`) is required and copied internally —
  SCL carries no interface-name parsing, so the caller must supply which local NIC to listen on;
  it's independent from the publisher-side VLAN/APPID/dst-MAC addressing `ied_model` parses for
  per-target filtering. `config == NULL` applies `GooseSubscriberConfig_defaults`. Returns `NULL` +
  sets `*outError` only on argument/allocation failure.
- **`GooseSubscription_setRecordCallback(handle, GooseSubscriberCallback callback, void*
  userParam)`** — must be set before `_start()`. Fires on `GooseReceiver`'s internal reception
  thread for every frame that decodes successfully; must be fast, non-blocking, must not call back
  into this feature's own API. `test=true`/`needsCommission=true` frames ARE still forwarded — per
  `goose_subscriber.h`'s own IMPORTANT note a standard-compliant receiver must not treat those as
  live data, but that policy decision is left to the caller (e.g. `ipc_dispatcher`), not silently
  dropped here. The delivered `GooseSubscriberRecord*` is owned by the caller after the callback
  returns — must eventually call `GooseSubscription_destroyRecord`.
- **`GooseSubscription_setStatusCallback(handle, GooseSubscriberStatusCallback callback, void*
  userParam)`** — optional. Fires on a VALID↔non-VALID transition for one target, from the
  liveness timer thread. Purely observational — never required for record delivery, which stays
  100% event-driven regardless. Must not block or call back into this feature's own API.
- **`GooseSubscription_start(GooseSubscriberHandle handle)`** — reads
  `IedModel_getGooseSubscriptionTargets(iedModel)` once, builds one `GooseSubscriber` per target
  (applying dst-MAC/APPID filters from SCL addressing when present), attaches them all to one
  `GooseReceiver`, calls `GooseReceiver_start()`, then starts the liveness timer thread. Blocking
  only for this synchronous setup (socket bind + thread create) — reception itself is async.
  Returns `GOOSE_SUBSCRIBER_ERR_NO_TARGETS` for an empty target list,
  `GOOSE_SUBSCRIBER_ERR_RECEIVER_START_FAILED` if `GooseReceiver_start()` leaves `!isRunning()`
  (typically missing `CAP_NET_RAW` or a bad `interfaceId`), `GOOSE_SUBSCRIBER_ERR_THREAD_CREATE_FAILED`
  if the liveness thread can't start. Idempotent: calling twice on an already-started handle is a
  no-op returning `GOOSE_SUBSCRIBER_OK`.
- **`GooseSubscription_stop(GooseSubscriberHandle handle)`** — stops the liveness thread
  (bounded, prompt) and the `GooseReceiver`. Blocking. Must be called from the caller's own thread,
  never from within a registered callback (deadlock). Safe to call more than once / on a
  never-started handle.
- **`GooseSubscription_destroy(GooseSubscriberHandle handle)`** — implies `_stop()` if still
  running, then frees the handle including the `GooseReceiver` (which cascades to destroy every
  attached `GooseSubscriber`).
- **`GooseSubscription_destroyRecord(GooseSubscriberRecord* record)`** — frees a record delivered
  via the record callback, including its entries array and every entry's cloned `MmsValue`.
  NULL-safe.

`GooseSubscriberError` (in `domain/goose_subscriber_types.h`): `GOOSE_SUBSCRIBER_OK`,
`_ERR_INVALID_ARGUMENT`, `_ERR_OUT_OF_MEMORY`, `_ERR_THREAD_CREATE_FAILED`, `_ERR_NO_TARGETS`,
`_ERR_RECEIVER_START_FAILED`.

## 3. Per-file breakdown

### `service/goose_subscriber_api.h` / `.c` (public boundary + wiring)

The only public header. `goose_subscriber_api.c` owns:

- **`resolveMemberReferences`** — one-time, local resolution of a target's dataset member
  references, run once per target inside `GooseSubscription_start`. Unlike MMS (which has an
  optional server-supplied `DataRef`), GOOSE never carries a reference on the wire, so this is
  always attempted, not just a fallback. In the same pass it also resolves:
  - Gap-4 structure-decomposition metadata (`memberLeafReferences`/`memberLeafCounts`, from
    `IedModel_getDataSetMemberLeafReferences`) and per-leaf **expected** wire types
    (`memberLeafWireTypes`, from `IedModel_getDataSetMemberLeafWireTypes`, only trusted if its
    count matches `leafCount`);
  - the value-diff cache's slot layout (`leafSlotOffsets`/`totalLeafSlots`) — one slot per raw
    member if non-decomposed, `memberLeafCounts[i]` consecutive slots if decomposed — and a
    zero-initialized (all-`NULL`, i.e. never-forwarded-yet) `lastForwardedValues` array sized to
    `totalLeafSlots`;
  - the Dbpos-semantics table (`leafSemantics`, from `IedModel_getDataSetMemberSemantics`/
    `_getDataSetMemberLeafSemantics`), zero-initialized to `IED_MODEL_DA_SEMANTIC_NONE` so an
    unresolved semantic degrades safely.
  Mirrors `mms_report_client_api.c`'s `buildMemberRefCache` almost exactly (same slot-offset
  accumulation approach).
- **`linkedListToStringArray` / `_WireTypeArray` / `_SemanticArray`** — generic helpers that move
  ownership out of a `LinkedList` of heap-boxed items into a flat array, discarding just the list
  shell (`LinkedList_destroyStatic`, never `_destroy`, which would double-free items already taken
  ownership of).
- **`GooseSubscription_start`** — after fetching targets, moves each `GooseSubscriptionTarget*`
  out of the returned `LinkedList` into a flat `GooseSubscriberTargetEntry[]` array (needed for
  O(1) indexed iteration by the liveness thread — different from `mms_report_client`, which keeps
  its whole cached `LinkedList` as-is since it never needs indexed access), calls
  `resolveMemberReferences` per entry, then delegates to `GooseSubscriberConnection_create`/`_start`.
  On any failure, tears down everything already built (`GooseSubscriberConnection_destroy` +
  `freeTargetEntries`).
- **`freeTargetEntries`** — the mirror-image teardown: frees every target's
  `GooseSubscriberMemberRefCache` contents (member references, leaf reference arrays, leaf wire
  types, `lastForwardedValues` clones, `leafSemantics`) and the target itself
  (`IedModel_destroyGooseSubscriptionTarget`).

### `domain/goose_subscriber_types.h` (domain vocabulary)

Deliberately uses libiec61850's own GOOSE vocabulary (`MmsValue`, `GooseSubscriber`,
`GooseParseError`) as this feature's domain types — same convention as `mms_report_client` using
`ClientReport`/`IedConnection` directly, since this data genuinely *is* the domain, not swappable
infrastructure. Key types:

- **`GooseSubscriberStatus`** — `VALID` (fresh, in-sequence), `STALE` (TAL elapsed, no
  refresh), `INVALID_STATE` (`isValid()==false` for a parse/sequence reason, not simple TAL
  expiry). Models *per-target* liveness — GOOSE has no association/connection concept, unlike
  `mms_report_client`'s single client-wide `MmsReportClientConnState`, since each GoCB is an
  independent connectionless multicast stream.
- **`GooseSubscriberEntry`** — one dataset member's decoded value: `value` (owned deep clone, NULL
  only if the source element itself was NULL), `reference` (owned, resolved locally, NULL only on
  resolution failure), `previousValue` (owned clone of whatever was cached for this exact wire
  position immediately before this frame — NULL means either genuine first-ever frame or "no cache
  slot at all"), `semantic` (`IED_MODEL_DA_SEMANTIC_DBPOS` if this leaf's real SCL bType is
  `Dbpos`, else `NONE`).
- **`GooseSubscriberRecord`** — one fully-owned GOOSE snapshot: `goCbRef`/`goId`/`dataSet` (owned,
  `goId`/`dataSet` nullable if the publisher omitted them), `stNum`/`sqNum`/`confRev`, `test`/
  `needsCommission` flags, `timeAllowedToLiveMs`, `timestampMs`, VLAN/APPID/MAC addressing
  (`hasVlan`/`vlanId`/`vlanPrio`, `appId` = -1 if unset, `srcMac`/`dstMac`), and the owned
  `entries[]`/`entryCount`.
- **`GooseSubscriberMemberRefCache`** — per-target Gap-4 decomposition metadata plus the
  value-diff cache used by the group-aware filter (see §"hybrid event filter" below). Mirrors
  `mms_report_client`'s `MmsReportClientMemberRefCacheEntry`, minus any `ReasonForInclusion`
  concept (GOOSE has none — every candidate is unconditionally diff-gated). Fields:
  `memberReferences`/`memberCount`; `memberLeafReferences`/`memberLeafCounts` (Gap-4); parallel
  `memberLeafWireTypes` (per-leaf expected `DataAttributeType`, cross-checked against the
  actual wire-decoded `MmsType`); `leafSlotOffsets`/`totalLeafSlots`/`lastForwardedValues` (the
  value-diff cache itself — **populated exactly once, on the target's first-ever valid frame, and
  PRESERVED for the subscriber's whole lifetime — never reset on a STALE/INVALID_STATE→VALID
  recovery**, superseding an earlier design with a since-deleted
  `GooseSubscriberUseCases_resetValueDiffCache`); parallel `leafSemantics` (also never reset, since
  a DA's real SCL type doesn't change); `everPopulated` (flips true once, after the first frame
  this target ever processes — gates whether a `NULL` cache slot found later is logged loudly as an
  unexpected bug, since under the never-reset design that should be structurally impossible).
- **`GooseSubscriberTargetEntry`** — one cached target: owns `target`
  (`GooseSubscriptionTarget*`, moved from the `LinkedList`), its `memberRefCache`, the live
  `rawSubscriber` handle (destruction owned by `GooseReceiver_destroy`'s cascade, not this
  struct), `lastKnownValid`/`lastValidAtMs` (guarded by `targetStateLock`, written by the frame
  adapter, read+compared by the liveness thread), and the heartbeat-dedup pair
  `hasForwardedStNum`/`lastForwardedStNum` (touched only by the frame adapter's single reception
  thread — deliberately NOT lock-guarded).
- **`GooseSubscriberDedupEntry`** / **`GooseSubscriberCrossTargetDedupCache`** — the GOOSE
  equivalent of `mms_report_client`'s cross-RCB dedup cache: a deep copy of the last record
  actually forwarded, from ANY target, used to suppress a different GoCB republishing
  byte-identical content (real networks sometimes configure multiple independent GoCBs/LNs
  publishing the same underlying event).
- **`struct sGooseSubscriberHandle`** — the internal representation, defined in this header
  (not hidden behind an extra internal header) since every file in the feature needs field
  access — opacity is enforced by which header is public (`service/goose_subscriber_api.h`), not
  by hiding the struct. Holds the borrowed `iedModel`, owned `interfaceId`, `config`, owned
  `receiver`/`targetEntries[]`/`crossTargetDedupCache`, both callbacks, and all liveness-thread
  state (`stopRequested`/`livenessExited`/`livenessThread`/`effectivePollMs`/`targetStateLock`).

### `domain/goose_subscriber_usecases.c` / `.h` (pure logic — the hybrid event filter)

No `GooseSubscriber`/`GooseReceiver` awareness at all — takes plain arguments (strings, `MmsValue*`
arrays) specifically so it's unit-testable without a real receiver (`GooseSubscriber` has no public
constructor, but `MmsValue` does).

**`GooseSubscriberUseCases_buildRecord`** is the entry point, building a fully-owned
`GooseSubscriberRecord*` from already-extracted frame fields via internal helper `buildEntries`,
which runs in three phases:

1. **`collectCandidates`** — for every raw dataset position `i`: if `memberRefCache` marks it
   decomposed (Gap-4: `memberLeafReferences[i]` non-NULL), flatten its structured value via
   `GooseSubscriberUtils_flattenStructure`. If the flattened leaf count matches
   `memberLeafCounts[i]`, run `reorderFlattenedToMatchReferences` — this reorders the flattened
   wire-order array so each output slot lines up with its actual SCL leaf reference, resolving `q`
   by type (`MMS_BIT_STRING` size 13) and `t` by type (`MMS_UTC_TIME`) first, everything else
   positionally — a defense against real hardware whose `GetVariableAccessAttributes`
   type-description order doesn't match its own GOOSE encoding order (mirrors
   `mms_report_client_usecases.c`'s identical function; the per-leaf type-compatibility *reject*
   gate that used to live here was removed at explicit user request after it rejected genuine
   decompositions on real hardware — this reorder step fixes the underlying q/stVal mislabeling
   directly instead). A count mismatch, flatten failure, or unresolvable q/t reorder falls back to
   one non-decomposed candidate for that position rather than mis-pairing labels to values. Every
   (possibly expanded) candidate is recorded as an `EntryCandidate` — undecided, forward/drop is
   not chosen here.
2. **Per-candidate diff filter + group-aware pass**, in `buildEntries` directly:
   - `shouldForwardAndUpdateCache` is the single per-candidate decision point. `slot < 0` (no
     cache slot resolvable) always survives (nothing to diff against). A candidate whose own
     `value == NULL` in this frame is **never** forwarded and **never** touches the cache — GOOSE
     legitimately can carry no value at some wire position in a given frame, and re-nulling a real
     cached slot would spuriously trip the `everPopulated`-gated error logging on a later genuine
     frame. Otherwise: `cached == NULL` means either the target's genuine first-ever frame
     (`everPopulated` still false — expected, silent) or, if `everPopulated` is already true, an
     unexpected/structurally-impossible gap logged loudly to stderr; either way the cache is
     silently seeded and this candidate is **never forwarded itself** — GOOSE's equivalent of
     MMS's GI-suppression even though GOOSE has no GI concept. Otherwise (`cached != NULL`),
     forwards only if `GooseSubscriberUseCases_isDuplicateValue` says the value genuinely differs.
   - **Group-aware "drag-along" pass**: every `q`-suffixed candidate's `$`-prefix anchors a group
     scope (`GroupAnchor`); every candidate resolves to the **longest** anchor it's nested under
     (`resolveGroupAnchor` — an ancestor walk over `$`-segments, not a single last-`$` strip,
     needed for deeply nested CONSTRUCTED-DA chains like a CMV's `q` several segments above its
     terminal `cVal$mag$f`). A candidate that didn't individually qualify still forwards if ANY
     other candidate resolving to the same anchor does — this is what keeps a value+quality pair
     traveling together in both directions. A candidate with no value of its own is excluded from
     being dragged in (same NULL-hazard guard as above). A candidate with no resolvable anchor at
     all is an ungroupable singleton, falling back to its own solo diff-check.
3. **Emit** — every forwarded candidate's cache slot is (re-)updated (`updateValueDiffCache`,
   idempotent if already updated in phase 2) and appended via `appendGooseEntry`, which clones
   `value`/dupes `reference` and *moves* (does not re-clone) the already-cloned `previousValue`.
   Non-forwarded candidates still had `previousValue` cloned in phase 2 regardless of outcome, so
   it's freed here to avoid a leak. `memberRefCache->everPopulated` flips true only after the whole
   frame is processed (checked against its pre-frame value throughout) — this is what keeps the
   true first-ever frame's seeding silent while making every frame after that treat an unexpected
   NULL slot as a bug worth logging.

**`valuesAreSemanticallyEqual`** (private) — `MmsValue_equals` is a raw byte-exact comparison,
wrong for two types that show up constantly in real GOOSE datasets (confirmed via
`mms_report_client`'s identical, independently-duplicated bug — GOOSE carries the same DA types):
`MMS_UTC_TIME`'s last byte is a `TimeQuality` flag that can legitimately wobble independent of the
real timestamp (compared instead via `MmsValue_getUtcTimeInMs`), and `MMS_BIT_STRING`
(CODEDENUM/Dbpos/Tcmd-style points) can have inconsistently-zeroed padding bits across real
firmware's different frame-generation paths (compared instead via `MmsValue_getBitStringAsInteger`
plus a size guard). Both match `ipc_dispatcher`'s own value codec exactly. Duplicated rather than
shared per this codebase's per-feature-layer convention.

**`GooseSubscriberUseCases_isDuplicateValue(cached, newValue)`** — public wrapper: `NULL` cached or
`NULL` newValue always returns false (never a duplicate) rather than dereferencing NULL; otherwise
delegates to `valuesAreSemanticallyEqual`.

**`GooseSubscriberUseCases_detectStatusTransition(wasValid, isValid, outStatus)`** — pure
edge-detection for the liveness thread: returns true (and fills `*outStatus`) only on a genuine
`wasValid != isValid` transition. On a transition to invalid it always reports `STALE` — refining
to `INVALID_STATE` needs the live `GooseSubscriber`'s `getParseError()`, which this pure function
can't reach; that refinement happens in the caller (`data/goose_subscriber_connection.c`'s liveness
loop).

**`GooseSubscriberUseCases_computeLivenessPollIntervalMs(configuredMs, minTalMs)`** — an explicit
`configuredMs > 0` always wins verbatim. Otherwise derives from the shortest currently-observed
`TimeAllowedToLive` across targets: `minTalMs / 4`, floored at 50ms (never busy-loop). `minTalMs
<= 0` (nothing observed yet — TAL is only known from a received GOOSE message, never from SCL)
falls back to a fixed 1000ms default.

**`GooseSubscriberUseCases_isDuplicateStNum(hasForwardedStNum, lastForwardedStNum, newStNum)`** —
pure GOOSE-heartbeat dedup: a publisher retransmits at every MinTime/MaxTime interval regardless of
whether data changed (`sqNum` increments on every retransmit, `stNum` only on a genuine change).
`hasForwardedStNum == false` always returns false (nothing forwarded yet, never a duplicate).

**`GooseSubscriberUseCases_shouldForwardAcrossTarget(cache, goCbRef, entries, entryCount)`** —
cross-target duplicate-content suppression (the second, independent gate after the per-target
filter above). Suppresses (returns false, leaves cache untouched) only when the cache has prior
content, `goCbRef` differs from the one that produced it, AND every `(reference, value)` pair
matches positionally (`crossTargetEntriesEqual`, itself using `valuesAreSemanticallyEqual`).
Everything else (nothing cached yet, same `goCbRef` as before — a same-target repeat is that
target's own filter's concern, not this cache's — or genuinely different content) replaces the
cache and forwards. `NULL`-safe on `cache` (always forwards).

**`GooseSubscriberUseCases_freeRecord`** / **`_destroyCrossTargetDedupCache`** — corresponding
deep-free helpers, both NULL-safe.

### `data/goose_subscriber_connection.c` / `.h` (GooseReceiver lifecycle + liveness polling thread)

All `GooseReceiver`/`GooseSubscriber` third-party integration lives here. Deliberately not
unit-tested in depth (a live `GooseReceiver` can't be meaningfully faked hermetically) — proven
E2E instead, mirroring `mms_report_client_connection`'s convention.

- **`GooseSubscriberConnection_create`** — creates the `GooseReceiver`, sets its `interfaceId`,
  then for each target: creates a `GooseSubscriber` (`GooseSubscriber_create`), applies
  `GooseSubscriber_setDstMac`/`_setAppId` filters only if `target->hasAddress` (SCL provided a
  matching `<GSE><Address>`), installs `GooseSubscriberFrameAdapter_onGooseReceived` as its
  listener, and calls `GooseReceiver_addSubscriber` — all **before** `GooseReceiver_start()`, per
  that call's documented "must not be called while running" constraint. Also contains a
  **temporary diagnostic** `fprintf(stderr, "[GOOSE_DIAG] ...")` dumping each target's resolved
  filter (dstMac/appId/vlanId) for direct comparison against a `tshark` capture — explicitly
  flagged in-code as removable once GOOSE reception silence is root-caused; not yet cleaned up as
  of this doc.
- **`livenessLoop`** (the liveness-polling thread — the one narrow, deliberate exception to Hard
  Rule "No cyclic polling"). Reception itself stays 100% event-driven via `GooseListener`; this
  loop only detects the *absence* of frames, which is structurally unobservable any other way in a
  connectionless protocol (no "disconnect" push signal, unlike `IedConnection`'s state-changed
  handler). At each low-rate tick, per target under `targetStateLock`: computes `isStale =
  wasValid && hasEverBeenValid && tal > 0 && (now - lastValidAtMs) > tal`; if stale, flips
  `lastKnownValid = false` and (outside the lock) fires the status callback with `STALE` (if
  `GooseSubscriber_getParseError() == GOOSE_PARSE_ERROR_NO_ERROR`) or `INVALID_STATE` otherwise.
  **Deliberately does NOT re-poll `GooseSubscriber_isValid()` directly** (an earlier version did):
  on loopback, a raw `ETH_P_ALL` socket sees each transmitted frame twice (its own
  `PACKET_OUTGOING` tap plus the real `PACKET_HOST` receipt — a same-host-only artifact), and
  libiec61850's own duplicate check flips `isValid()` back to false within the same frame's
  duplicate delivery, a window this poll could trivially and consistently miss on an otherwise
  healthy feed. Instead it compares against `lastValidAtMs`, set only by the frame adapter on
  genuinely fresh, non-duplicate frames. The poll interval self-adjusts every iteration via
  `computeMinObservedTalMs` + `GooseSubscriberUseCases_computeLivenessPollIntervalMs`, and sleeps
  via `interruptibleSleep` (20ms chunks) so `_stop()`'s bounded wait doesn't have to wait out a
  full poll interval.
- **`GooseSubscriberConnection_start`** — `GooseReceiver_start()` (library-managed reception
  thread, non-blocking — no manual `tick()`), verifies `isRunning()`, then spawns the liveness
  thread.
- **`GooseSubscriberConnection_stop`** — sets `stopRequested`, waits (in 20ms increments) for the
  liveness thread to exit, then `GooseReceiver_stop()`. Blocking; must not be called from within a
  registered callback.
- **`GooseSubscriberConnection_destroy`** — `Thread_destroy` + `GooseReceiver_destroy` (cascades to
  every attached `GooseSubscriber`) + `Semaphore_destroy(targetStateLock)`.

### `data/goose_subscriber_frame_adapter.c` / `.h` (the GooseListener)

The **only** file in this feature that touches the opaque `GooseSubscriber` type inside a
`GooseListener` callback — installed as the listener on every `GooseSubscriber`. Fires on
`GooseReceiver`'s internal reception thread. `goose_subscriber.h` documents no explicit deadlock
warning for `GooseListener` (unlike `ReportCallbackFunction`), but this adapter is conservative
anyway: must not block, must not call back into this feature's own API.

`GooseSubscriberFrameAdapter_onGooseReceived`:
1. Drops the frame immediately (no liveness-state touch) if `!GooseSubscriber_isValid(subscriber)`
   — nothing safe to normalize from a frame that failed to parse/sequence.
2. Finds the matching `GooseSubscriberTargetEntry` (linear scan by `rawSubscriber` pointer
   identity). If found, under `targetStateLock`: calls
   `GooseSubscriberUseCases_detectStatusTransition(entry->lastKnownValid, true, &status)`, sets
   `lastKnownValid = true` and `lastValidAtMs = Hal_getMonotonicTimeInMs()`. This is what drives
   **VALID transitions synchronously and event-drivenly**, rather than relying solely on the
   liveness thread's periodic re-observation — the same-host loopback duplicate-tap artifact
   described above means a poll-only design could miss the VALID window entirely even on a healthy
   feed; this adapter runs exactly when a fresh frame lands, so it can't. On a genuine transition,
   `entry->hasForwardedStNum` is reset to false (so the very next real frame is delivered at least
   once even if its `stNum` numerically collides with a pre-stale value — e.g. the publisher never
   actually restarted) and the status callback fires with the resolved status. The per-position
   value-diff cache is deliberately **not** touched on this transition — see the "never reset"
   design in `GooseSubscriberMemberRefCache`.
3. If a record callback is registered: computes `stNum`, short-circuits via
   `GooseSubscriberUseCases_isDuplicateStNum` (the cheap Gap-2 heartbeat-retransmission check,
   avoiding running the whole per-position filter pipeline on every MinTime/MaxTime keep-alive),
   then extracts every other frame field (`dataSetValues`, VLAN, src/dst MAC, `goId`, `dataSet`,
   `confRev`, `test`/`needsCommission`, TAL, timestamp) and calls
   `GooseSubscriberUseCases_buildRecord`.
4. If a record was built (`buildEntries` survived at least one candidate through the filter):
   updates `hasForwardedStNum`/`lastForwardedStNum` unconditionally whenever a record was built at
   all (even a zero-entry one — so an identical-stNum heartbeat stays caught by the cheap
   `isDuplicateStNum` gate on the *next* frame rather than re-running the whole pipeline again),
   then applies the second, independent gate `GooseSubscriberUseCases_shouldForwardAcrossTarget`
   only if `record->entryCount > 0` — only survivors of this target's own per-position filter are
   even candidates for cross-target dedup. A record dropped by either gate is freed via
   `GooseSubscriberUseCases_freeRecord` rather than delivered.

### `utils/goose_subscriber_utils.c` / `.h` (small MmsValue/string helpers)

Shared by the frame adapter and the domain layer — no `GooseSubscriber`/`GooseReceiver` awareness,
just `MmsValue`/plain-C-string helpers. Same shape as `mms_report_client_utils`, minus a
`ReasonForInclusion` helper (GOOSE has no per-entry reason-for-inclusion concept — the whole
dataset is retransmitted on every message, unlike an MMS report's per-element change tracking).

- **`GooseSubscriberUtils_cloneMmsValueArray(dataSetValues, count)`** — clones the first `count`
  elements of an `MMS_ARRAY` into a freshly allocated owned array. `NULL`/`count <= 0` → `NULL`.
- **`GooseSubscriberUtils_safeStringDup(s)`** — NULL-safe strdup.
- **`GooseSubscriberUtils_flattenStructure(value, outLeafCount)`** — recursively flattens a
  (possibly nested) `MMS_STRUCTURE` into an ordered array of terminal leaf elements, depth-first
  in element order — matches the convention `IedModel_getDataSetMemberLeafReferences` uses over
  the DOType/DAType model tree (purely positional correspondence, documented not verified by
  type). If `value` isn't itself `MMS_STRUCTURE`, returns a single-element array containing it
  unchanged. Returned pointers are **borrowed** from `value` (never cloned) — caller must clone
  before `value`'s owning `GooseSubscriber` becomes invalid; caller owns only the array shell.
  `NULL` `value` or allocation failure → `NULL` with `*outLeafCount = 0`; a mid-flatten allocation
  failure drops that one leaf rather than crashing (the caller's leaf-count-mismatch guard in
  `collectCandidates` catches the resulting short count and falls back safely).

## 4. Threading & concurrency model

Two threads own all activity in a running subscription, both wholly managed by this feature (no
thread of its own beyond these two):

- **Reception thread** — entirely owned/scheduled by `GooseReceiver_start()` (library-internal,
  event-driven on frame arrival — never polled for reception). Runs
  `GooseSubscriberFrameAdapter_onGooseReceived` on every decoded frame across every attached
  `GooseSubscriber`, confirmed serial (one frame at a time, even across different targets). This
  is the only thread that writes `hasForwardedStNum`/`lastForwardedStNum` — deliberately *not*
  guarded by `targetStateLock`, since only this single thread ever touches them. It *does* write
  `lastKnownValid`/`lastValidAtMs` under `targetStateLock`, since the liveness thread reads those
  concurrently.
- **Liveness-polling thread** (`livenessLoop`, `data/goose_subscriber_connection.c`) — the one
  approved exception to "No cyclic polling." Wakes at `effectivePollMs` (self-adjusting, derived
  from observed TAL), reads/writes `lastKnownValid`/`lastValidAtMs` under `targetStateLock`, then
  fires the status callback *outside* the lock. Never touches `hasForwardedStNum`/
  `lastForwardedStNum` or the value-diff cache directly.
- **`targetStateLock`** (a `Semaphore` used as a mutex) is the only lock in this feature — it
  guards exactly `targetEntries[i].lastKnownValid`/`lastValidAtMs` and serializes the
  read-transition-write sequence around the status callback firing. The value-diff cache
  (`memberRefCache`) and Gap-4 metadata are read/written only by the reception thread (inside
  `buildEntries`), so they need no additional lock.
- **Caller's own thread** drives `GooseSubscription_create`/`_start`/`_stop`/`_destroy` and both
  callback registrations. `_stop`/`_destroy` block until the liveness thread has confirmed exit
  (`livenessExited`) and `GooseReceiver_stop()` returns — **must never** be called from inside
  either registered callback (deadlock risk, since both callbacks fire from threads `_stop` is
  waiting on).
- Record/status callbacks themselves must be fast and non-blocking and must not call back into
  this feature's own API — same contract `mms_report_client`/`ipc_dispatcher` use for their own
  producer-thread callbacks.

## 5. Known limitations / deliberate scope boundaries

- **Cross-target dedup cache is single-slot, not per-pair.** `GooseSubscriberCrossTargetDedupCache`
  remembers only the single most-recently-forwarded `(goCbRef, entries)` snapshot across the whole
  subscriber, not one baseline per distinct content-fingerprint — proven sufficient for the
  "N independent GoCBs republishing the same event" pattern by
  `test_shouldForwardAcrossTarget_suppressionDoesNotDisturbEstablishedBaseline`, but three or more
  genuinely-distinct concurrent event streams interleaving could in principle thrash this single
  slot. Not exercised by any test.
- **Liveness detection is TAL-driven and per-target**, bounded by `effectivePollMs`
  (self-derived, floored at 50ms) — a stale target is detected with up to one poll interval of
  latency, not instantly.
- **The temporary `[GOOSE_DIAG]` diagnostic**, plain `SS_LOG_DEBUG` calls like every other call
  site in this feature — `data/goose_subscriber_connection.c` and
  `data/goose_subscriber_frame_adapter.c` both declare `#define SS_LOG_FEATURE
  "goose_subscriber"`, so this feature's output (this diagnostic included) lands in its own file,
  `station-signal-goose_subscriber.log` under `STATION_SIGNAL_LOG_DIR` (see the parent repo's
  `CLAUDE.md` Logging bullet for the general per-feature-file scheme this is just one instance
  of) — explicitly marked in-code as removable once GOOSE reception silence is root-caused,
  still present as of this doc, not yet cleaned up. Two call sites: `GooseSubscriberConnection_create`/
  `_start` (`data/goose_subscriber_connection.c`) log each target's resolved dst-MAC/APPID/VLAN
  filter, the total target count, and whether `GooseReceiver_start` actually entered the running
  state; `GooseSubscriberFrameAdapter_onGooseReceived` (`data/goose_subscriber_frame_adapter.c`)
  logs **every** outcome of every callback invocation — valid/invalid, duplicate-heartbeat-dropped,
  filtered-to-zero-entries-dropped, allocation-failure-dropped, or forwarded — since every one of
  those paths was previously silent, making "frames never reach the raw socket" indistinguishable
  from "frames arrive but this codebase drops them" from a log capture alone. If a device shows
  `GooseReceiver_start ... entered the running state` but never one single subsequent
  `onGooseReceived` line, reception is failing below libiec61850's own receiver thread (wrong
  interface, VLAN/NIC-offload tag stripping, a filter mismatch against what a `tshark` capture on
  the same interface actually shows) rather than in this codebase's own filtering.
- **No `ReasonForInclusion`-equivalent signal** — unlike `mms_report_client`, which can trust an
  RCB's own `ReasonForInclusion` bitmask under some conditions, GOOSE gives this feature no such
  signal at all: every candidate is unconditionally diff-gated. A `NULL` cache slot is the *only*
  thing that unconditionally survives, and only on a target's genuine first-ever frame.
- **No dataset chunking / `maxAttributes` handling** — inherited from `ied_model`'s dynamic-dataset
  synthesis conventions elsewhere in this codebase; not this feature's own concern since it never
  creates datasets, only reads SCL-declared ones.
- **Gap-4 reorder heuristic (`reorderFlattenedToMatchReferences`) resolves only `q`/`t` by type**;
  everything else is positional. A device whose non-`q`/`t` leaves are *also* wire-order-scrambled
  relative to their SCL declaration order would still mis-pair — not observed in practice, but not
  structurally prevented either.
- **A duplicate-content suppression across targets assumes exact positional entry-order match**
  (`crossTargetEntriesEqual` compares `entries[i]` to `entries[i]` — no reordering/matching by
  reference) — two GoCBs whose otherwise-identical dataset is declared in a different member order
  in SCL would not be recognized as duplicates of each other.

## 6. Cross-feature dependencies

- **`ied_model`** (`src/features/ied_model/service/ied_model_api.h`) — the sole source of
  subscription targets (`IedModel_getGooseSubscriptionTargets`, returning
  `GooseSubscriptionTarget*`: `objectReference`, `datasetReference`, and optional
  `hasAddress`/`vlanId`/`vlanPriority`/`appId`/`dstMac` publisher addressing parsed from SCL's
  `<GSE><Address>`) and of every reference/leaf/semantic resolution
  (`IedModel_getDataSetMemberReferences`, `_getDataSetMemberLeafReferences`,
  `_getDataSetMemberLeafWireTypes`, `_getDataSetMemberSemantics`,
  `_getDataSetMemberLeafSemantics`) — all purely local, called once at `GooseSubscription_start`,
  never re-invoked over the wire. `goose_subscriber` never re-parses SCL and never discovers GoCBs
  itself (Hard Rule).
- **`src/orchestration/`** — starts and stops this feature as one stage of its per-IED pipeline
  (`Orchestration_run*`), wiring `GooseSubscription_setRecordCallback` to
  `IpcDispatcher_onGooseRecord`. Not itself a "feature" — `orchestration` is a sequencing layer
  above both workers.
- **`ipc_dispatcher`** — the sole registered consumer of delivered `GooseSubscriberRecord`s
  (via the callback `orchestration` wires up), normalizing them into the shared `GOOSE` JSON
  envelope (`schemaVersion`, `type: "GOOSE"`, `source`, `hasTimestamp` always true for GOOSE,
  `timestampMs`, `dataPoints[]`). `ipc_dispatcher`'s own quality-pairing
  (`IpcDispatcherUseCases_pairQuality`) is a second, JSON-layer application of the same
  ancestor-walk anchor logic this feature's `resolveGroupAnchor` already applies at the C-struct
  level — the two are independent implementations of the same rule, not a shared call.
- **No dependency in the other direction** — nothing in `ied_model`/`orchestration`/
  `ipc_dispatcher` reaches into this feature's `domain`/`data`/`utils` layers; only
  `service/goose_subscriber_api.h` is included elsewhere.

## 7. Tests

### `tests/goose_subscriber/` (unit — Unity, hermetic, no real socket)

Registered in `tests/Makefile` as three binaries (`tests/Makefile` does not auto-discover):

- **`test_goose_subscriber_api.c`** — wiring-only, mirrors `test_mms_report_client_api.c`'s
  "wiring only" spirit: `GooseSubscription_create` argument validation (NULL `iedModel`, NULL/empty
  `interfaceId`, success case, config-defaults-applied-on-NULL case),
  `GooseSubscription_start` returning `GOOSE_SUBSCRIBER_ERR_NO_TARGETS` against a bare model with
  no GSEControlBlocks, NULL-safety of `_stop`/`_destroy`/both callback setters, and
  `GooseSubscriberConfig_defaults`' documented value (`livenessPollMs == 0`). Deliberately does
  **not** call `GooseSubscription_start` against a model *with* GSEControlBlocks — that would open
  a real raw socket (needs `CAP_NET_RAW`, not hermetic) and is left to the E2E test.
- **`test_goose_subscriber_usecases.c`** (the largest file, ~50 test cases) — the hybrid filter's
  full behavioral proof, direct calls into `GooseSubscriberUseCases_buildRecord` and friends:
  scalar-field copying, deep-copy/non-aliasing of entries and references, reference resolution
  from `memberRefCache`/out-of-range handling, NULL-dataSetValues handling, VLAN field zeroing when
  `hasVlan == false`; Gap-4 decomposition (successful decompose, leaf-count-mismatch fallback,
  decompose still succeeding with matching wire types present); the per-position value-diff filter
  (first-ever-value suppression + cache seeding, unchanged-value drop, changed-value forward +
  cache update + `previousValue` population, `previousValue == NULL` when there's no cache slot at
  all); the group-aware drag-along pairing (value drags unchanged quality and vice versa,
  dragged-along sibling's own `previousValue == value`, both-unchanged-siblings suppressed,
  ungroupable singleton falls back to solo diff-check, a decomposed-group leaf dragging its
  sibling leaf, a deeply-nested CMV's `q` dragging several ancestor levels up, and the negative
  case proving it does *not* overreach past a genuinely unrelated ancestor); the "never reset"
  cache-persistence design simulated across multiple `buildRecord` calls with no reset call in
  between (first-frame seeding + `everPopulated` flip, a simulated recovery's genuine change
  forwarding with a real preserved `previousValue`, a simulated recovery's unchanged resend being
  suppressed); `isDuplicateValue`'s type-aware comparison (`MMS_UTC_TIME` ignoring the
  `TimeQuality` byte, `MMS_BIT_STRING` ignoring padding via a size+integer-value comparison, a
  documented sanity check that the *old* raw `MmsValue_equals` would have gotten this wrong);
  `shouldForwardAcrossTarget`'s full cross-target dedup matrix (first-ever content, same-target
  repeat still forwarded, different-target identical content suppressed, different-target
  different content forwarded, a suppression not disturbing the established baseline, NULL-cache
  no-op); `detectStatusTransition`'s edge cases; `computeLivenessPollIntervalMs`'s
  configured/derived/floored/fallback branches; `isDuplicateStNum`'s three cases.
- **`test_goose_subscriber_utils.c`** — `cloneMmsValueArray` (NULL/zero-count/negative-count →
  NULL, independent deep copies proven via post-clone mutation), `safeStringDup` (NULL-safety,
  independent copy proven via post-dup mutation), `flattenStructure` (NULL → NULL, scalar → single
  borrowed-pointer array, flat 3-element structure preserves order, nested structure recurses to
  terminal leaves only — intermediate structure nodes like `cVal` never appear in the output).

### `integration_tests/goose_subscriber/` (E2E — needs `sudo`, real raw socket)

`e2e_test_goose_subscriber.c` runs a real `ied_simulator` "Reporter1" IED
(`integration_tests/ied_simulator/`, fully decoupled from `src/`) in-process, loads
`fixtures/reporter1.cid` through the real `ied_model` service at
`IED_MODEL_ACCESS_REPORT_ONLY` (proving this feature needs nothing more than the lowest access
tier), and connects the real `goose_subscriber` feature to it over `"lo"`. Three cases:

- **`test_dataChangeOnServer_triggersGooseRecordWithNewValue`** — waits (via the status callback,
  no arbitrary sleep) for the liveness thread to observe a VALID feed, flips
  `GGIO1.Ind1.stVal` on the simulator, and asserts a real GOOSE-carried record arrives with the
  expected `goCbRef` (`Reporter1LD1/LLN0$GO$gcbInd`), `entryCount == 2`, and both entries' locally
  resolved references (`...GGIO1$ST$Ind1$stVal`, `...GGIO1$ST$Ind1$q`) — the primary end-to-end
  proof of the always-on, purely-local reference-labeling path.
- **`test_firstFrameEverPerTarget_neverReachesCallback`** — proves bootstrap-suppression
  end-to-end: the very first frame a subscriber receives for `gcbInd` (sent immediately and
  repeatedly by the publisher from `SimServer_start`, well before any value flip) must never reach
  the record callback at all, using a bounded *negative* wait (`waitBriefly`, ~2s) shorter than the
  normal positive-wait bound.
- **`test_crossTargetDuplicateContent_onlyOneOfTwoIdenticalGoCbsReachesCallback`** — proves
  `GooseSubscriberUseCases_shouldForwardAcrossTarget` end-to-end: `gcbInd` and `gcbDup` in the
  fixture are two independently-subscribed GoCBs publishing the identical `ds1` dataset
  (reproducing a real network's redundant-publisher pattern). Flipping `GGIO1.Ind1.stVal` changes
  both publishers' state at nearly the same moment; asserts exactly one of the two resulting
  identical-content records reaches the callback, with a generous 500ms settle window to catch a
  hypothetical duplicate.

Requires `CAP_NET_RAW` (raw AF_PACKET socket over `"lo"`) — run via `sudo make run` per
`integration_tests/goose_subscriber/Makefile`; fails fast with a clear message
(`GOOSE_SUBSCRIBER_ERR_RECEIVER_START_FAILED`) rather than hanging if not run as root. Whether
GOOSE frames actually round-trip over `"lo"` through this libiec61850 build was validated
separately, beforehand, via `tools/smoke_tests/goose_loopback_smoke_test.c`. Registered in
`run_all_tests.sh` as `"e2e: goose_subscriber"`.
