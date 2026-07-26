# `src/scan_orchestration/`

## 1. Overview

`scan_orchestration` is a top-level sibling of `src/features/` — like
`src/orchestration/` and `src/device_manager/`, it is a *sequencing* layer,
not a feature in its own right. It turns `ied_discovery`'s one-shot,
synchronous `IedDiscovery_scanSubnet` call into a continuous, background,
reference-counted, multi-scan-capable service: sweep a subnet on a timer,
diff against what's already been announced, publish only genuinely new hosts
over `scan_dispatcher`'s shared websocket, sleep, repeat — for as many
concurrent scans (different interfaces/`mmsPort`s) as callers start.

It exists because `ied_discovery` itself is deliberately synchronous and
scope-limited to one sweep; something has to own the "keep sweeping
forever, in the background, dedupe results, fan them out to a websocket,
and let multiple scans share one transport" concerns without touching
`ied_discovery`'s own code. Per CLAUDE.md's explicit convention,
`ied_discovery` is left entirely untouched by this layer — this layer only
calls its public API. `scan_dispatcher`'s entire lifecycle (bind on
0→1 active scans, tear down on 1→0) is owned here, refcounted by active-scan
count.

Public boundary: `src/scan_orchestration/service/scan_orchestration_api.h`.
Callers (currently only `control_dispatcher`, via its `START_SCAN`/
`STOP_SCAN` commands) must only include this header — never reach into
`domain/`/`data/` directly.

## 2. Public API surface

All declared in `service/scan_orchestration_api.h`:

- **`ScanOrchestrationConfig_defaults(config)`** — fills `config` with
  `ScanDispatcherConfig_defaults` (port 8766), `defaultSweepIntervalMs =
  10000`, and `IedDiscoveryConfig_defaults` as the per-scan template (that
  template's own `.acseAuthPassword` is later ignored — see §3's
  `ScanRequest` breakdown). Caller may override individual fields before
  passing the result to `_create`.
- **`ScanOrchestration_create(config, outError)`** — allocates only: creates
  the registry and calls `ScanDispatcher_create` (ring-buffer allocation
  only, no bind — matches every sibling feature's "no I/O at create"
  contract). `config == NULL` means apply `ScanOrchestrationConfig_defaults`.
- **`ScanOrchestration_setDeviceFoundCallback(handle, callback, userParam)`**
  — must be called before any `_startScan`. One process-wide slot per
  handle (not per-scan, mirrors `orchestration`'s own single-callback-slot
  convention); a worker snapshots the pointer at creation time, so changing
  the callback after a scan has already started doesn't retroactively apply.
- **`ScanOrchestration_startScan(handle, request, outScanId)`** — starts a
  new continuous background scan: sweep → diff against this scan's own
  seen-set → publish each genuinely new host (to the shared
  `ScanDispatcher` and the optional `foundCallback`) → interruptible sleep
  for `sweepIntervalMs` → repeat, until `_stopScan(scanId)`. On the very
  first active scan (0→1) also starts the shared `ScanDispatcher`'s
  websocket (bind + service thread); a bind failure here fails the *whole*
  `_startScan` call (`SCAN_ORCHESTRATION_ERR_DISPATCHER_START_FAILED`) —
  no worker is left running. Multiple concurrent scans share the one
  `ScanDispatcher` instance. `*outScanId` is set only on
  `SCAN_ORCHESTRATION_OK`.
- **`ScanOrchestration_stopScan(handle, scanId)`** — stops the identified
  scan. **Blocking** — see §4/§5 for the exact bound (an in-flight sweep
  cannot be cancelled). If this was the last active scan (1→0), also fully
  tears down the shared `ScanDispatcher`. Returns
  `SCAN_ORCHESTRATION_ERR_SCAN_NOT_FOUND` if `scanId` isn't currently active
  (already stopped, or never existed).
- **`ScanOrchestration_snapshotDiscoveredHosts(handle, scanId, outHosts,
  outCount)`** — thread-safe read-only snapshot of a scan's currently
  announced host list, for a local in-process caller that wants the live,
  growing list without connecting to the daemon's own websocket as a client
  of itself. **Unused today** — every real client gets results over
  `scan_dispatcher`'s websocket instead; this exists for a hypothetical
  local caller (e.g. an in-process discovery-prompt adapter). Caller
  contract: must not be called concurrently with `_stopScan` for the *same*
  `scanId` from a different thread — the worker object this reads from is
  destroyed by `_stopScan`.
- **`ScanOrchestration_freeDiscoveredHostsSnapshot(hosts, count)`** — frees
  a snapshot returned by the above.
- **`ScanOrchestration_destroy(handle)`** — stops every still-active scan
  (draining via the public `_stopScan` path, one at a time), destroys the
  registry, destroys the shared `ScanDispatcher` (implies `_stop` if still
  running — harmless no-op if the drain above already brought it to 0),
  frees the handle. NULL-safe.

## 3. Per-file breakdown

### `service/scan_orchestration_api.h`

Public header, documented in full in §2. Also carries the module-level
doc comment stating this layer's own convention: callers only ever include
this header, and it sequences `ied_discovery` (subnet enumeration + host
verification, untouched) and `scan_dispatcher` (transport) into the
continuous multi-scan service.

### `service/scan_orchestration_api.c`

Implements every function in §2 by delegating to the registry (`data/`)
and worker (`data/`) layers — this file has no locking or threading logic
of its own, it's pure sequencing:

- `ScanOrchestrationConfig_defaults` — thin wrapper composing
  `ScanDispatcherConfig_defaults` + `IedDiscoveryConfig_defaults` +
  the `10000`ms literal.
- `ScanOrchestration_create` — `calloc`s the handle, creates the registry,
  creates the `ScanDispatcher` (alloc-only). Rolls back cleanly (frees
  registry, then handle) if any allocation step fails, returning
  `SCAN_ORCHESTRATION_ERR_OUT_OF_MEMORY`.
  `ScanOrchestration_setDeviceFoundCallback` — just stores the pointer/
  param pair on the handle.
- `ScanOrchestration_startScan` — reserves a scanId
  (`ScanOrchestrationRegistry_nextScanId`), creates the worker
  (`ScanOrchestrationWorker_create`, no thread yet), registers it
  (`ScanOrchestrationRegistry_addAndMaybeStartDispatcher` — this is the
  0→1 dispatcher-start point), then starts its thread
  (`ScanOrchestrationWorker_start`). If the thread-start step fails *after*
  registration, it rolls back: removes the just-added registry entry,
  stops the dispatcher if that removal brought the count back to 0, and
  destroys the worker — mirroring `src/orchestration/`'s own fail-hard
  rollback convention.
- `ScanOrchestration_stopScan` — removes the worker from the registry
  under a short lock (`ScanOrchestrationRegistry_remove`), then, entirely
  *outside* that lock, calls `ScanOrchestrationWorker_destroy` (the
  potentially-slow part — implies `_stop`, which can block for an
  in-flight sweep) and, if this removal emptied the registry, stops the
  shared dispatcher. This ordering is the entire point of the two-phase
  locked registry — see §3's registry section and §4.
- `ScanOrchestration_snapshotDiscoveredHosts` — looks the worker up via
  `ScanOrchestrationRegistry_find` (borrowed pointer, not ownership) and
  delegates to `ScanOrchestrationWorker_snapshotHosts`.
- `ScanOrchestration_destroy` — loops
  `ScanOrchestrationRegistry_anyActiveScanId` → `ScanOrchestration_stopScan`
  until the registry reports empty (0 is never a valid scanId, doubling as
  the loop's sentinel), then destroys the registry and the shared
  dispatcher and frees the handle.

### `domain/scan_orchestration_types.h`

Domain vocabulary for the whole layer — entirely composed of its sibling
features' *public* APIs (`ied_discovery_api.h`, `scan_dispatcher_api.h`),
never their domain/data headers directly, mirroring
`src/orchestration/domain/orchestration_types.h`'s own rule. Zero direct
third-party includes: this layer sequences already-implemented features,
it doesn't talk to libiec61850/libwebsockets/cJSON itself.

Key types:

- **`ScanOrchestrationError`** — `OK`, `ERR_INVALID_ARGUMENT`,
  `ERR_OUT_OF_MEMORY`, `ERR_DISPATCHER_START_FAILED` (0→1 transition's
  `ScanDispatcher_start` failed), `ERR_THREAD_CREATE_FAILED`,
  `ERR_DISCOVERY_CREATE_FAILED` (per-scan `IedDiscovery_create` failed),
  `ERR_SCAN_NOT_FOUND` (scanId isn't currently active).
- **`ScanRequest`** — `interfaceId` (borrowed at call time, deep-copied
  internally by the worker), `mmsPort`, `sweepIntervalMs` (`0` ⇒
  `config.defaultSweepIntervalMs`), `acseAuthPassword` (borrowed,
  deep-copied internally; `NULL` = unauthenticated). This is **per-request,
  not per-handle** — concurrent scans may target devices needing different
  credentials.
- **`ScanOrchestrationConfig`** — `scanDispatcherConfig` (port default
  8766), `defaultSweepIntervalMs` (default 10000), `discoveryConfigTemplate`
  (an `IedDiscoveryConfig` whose `tcpProbeTimeoutMs`/
  `maxConcurrentTcpProbes`/`mmsConnectTimeoutMs`/`maxHosts` are reused
  per-scan verbatim, but whose own `.acseAuthPassword` field is **ignored**
  — `ScanRequest.acseAuthPassword` wins instead, per-scan).
- **`ScanOrchestrationDeviceFoundCallback`** — `void (*)(userParam, scanId,
  host, mmsPort)`. Fires once per genuinely new host, on the scan's own
  worker thread. Must not block (same contract as
  `MmsReportClientConnStateCallback`/`GooseSubscriberStatusCallback`).
- **`struct sScanOrchestrationHandle`** — `config`, `scanDispatcher`
  (owned, alloc-only at `_create`), `registry` (owned, opaque —
  `struct sScanOrchestrationRegistry` is defined only in
  `data/scan_orchestration_registry.c`), `foundCallback` +
  `foundCallbackParam`.

### `domain/scan_orchestration_usecases.h` / `.c`

Pure logic, zero third-party includes, unit-testable with hand-built plain
arrays. One function:

- **`ScanOrchestrationUseCases_isHostNew(seenHosts, seenCount, host)`** —
  linear scan, exact-string comparison, `O(seenCount)` (fine for realistic
  per-scan host counts — tens, bounded by `IedDiscoveryConfig.maxHosts`
  anyway). Canonical dotted-quad IPs need no normalization (confirmed by
  `test_isHostNew_exactStringMatch_noNormalization` — `"192.168.1.1"` and
  `"192.168.001.001"` are treated as distinct). Returns `true` (treated as
  "new") if `host` is `NULL`/empty — caller's own responsibility not to
  call this with a bad host in practice.

### `data/scan_orchestration_registry.h` / `.c`

Mutex-guarded (`hal_thread.h` `Semaphore(1)` as a binary mutex) bookkeeping
of every currently-active scan: scanId generation, the active-scan array,
and the 0↔1 `ScanDispatcher` start/stop refcounting. Opaque outside this
file — `struct sScanOrchestrationRegistry` is private to the `.c`.

**This is the layer's key design point: two-phase locking.**
`ScanOrchestrationWorker_stop` can block for the duration of an in-flight
sweep (see §4/§5), so the registry must *never* hold its lock across that
wait — otherwise one slow scan's stop would serialize every other
concurrent scan's start/stop behind it, directly violating "multiple
concurrent scans must run independently."

Storage: a `realloc`-doubling flat array of `{scanId, worker}` entries
(`ScanOrchestrationRegistryEntry`), `count`/`capacity`, plus a monotonic
`nextScanId` counter starting at 1 (0 is never a valid scanId).

Functions:

- **`ScanOrchestrationRegistry_create`/`_destroy`** — alloc/free the
  registry struct + semaphore + entries array. `_destroy` does **not**
  stop/destroy any workers still registered — the caller
  (`ScanOrchestration_destroy`) must drain every active scan via the public
  `_stopScan` path first.
- **`ScanOrchestrationRegistry_nextScanId`** — pure counter bump under the
  lock, `O(1)`, no I/O.
- **`ScanOrchestrationRegistry_addAndMaybeStartDispatcher(registry,
  scanDispatcher, worker)`** — the **short critical section**: held under
  the lock for its entire body, which is safe *because* every operation
  inside is fast and bounded (a counter check, at most one
  `ScanDispatcher_start` bind+thread-create, one array append) — unlike
  `ScanOrchestrationWorker_stop`'s unbounded wait, which is why *that* one
  is deliberately kept outside this lock (see `_remove` below). If
  `registry->count == 0` (this would be the first active entry), calls
  `ScanDispatcher_start` *before* registering anything — a bind failure
  here means nothing gets registered at all, and the worker is left for
  the caller to destroy. On successful append, returns `OK`; on append
  failure (OOM) after a dispatcher start was just performed for this
  would-be-first scan, it undoes that start (`ScanDispatcher_stop`) so a
  later successful `_startScan` can rebind cleanly, and returns
  `ERR_OUT_OF_MEMORY`.
- **`ScanOrchestrationRegistry_remove(registry, scanId, outWorker,
  outNowEmpty)`** — removes `scanId`'s entry (if present) under the lock,
  via swap-remove (order among active scans is never meaningful), and
  hands the worker back through `*outWorker` — **ownership transfers to
  the caller**, who stops/destroys it (the potentially-slow part) entirely
  *outside* any lock, afterward. Sets `*outNowEmpty = true` if this
  removal brought the active count to zero, so the caller can stop the
  dispatcher itself in a separate, later, short critical section. Returns
  `false` (leaves `*outWorker` untouched) if `scanId` isn't currently
  active.
- **`ScanOrchestrationRegistry_find(registry, scanId)`** — thread-safe
  lookup, returns a *borrowed* worker pointer (`NULL` if not active) —
  caller must not stop/destroy it via this pointer; only `_remove` hands
  out ownership.
- **`ScanOrchestrationRegistry_activeCount`** — number of currently
  registered scans; *is* the scanDispatcher's own reference count, no
  separate counter needed.
- **`ScanOrchestrationRegistry_anyActiveScanId`** — returns any one active
  scanId (or 0 if empty), used only by `ScanOrchestration_destroy` to drain
  the registry one scan at a time via the public `_stopScan` path.

### `data/scan_orchestration_worker.h` / `.c`

One continuously-running background scan: owns a **private**
`IedDiscoveryHandle` and its **own seen-set** (hosts already announced by
this scan), looping sweep → diff against seen-set → publish each
genuinely new host → interruptible sleep → repeat, until stopped. Opaque
outside this file (`struct sScanOrchestrationWorker` is private to the
`.c`).

**Struct fields** (`struct sScanOrchestrationWorker`): `scanId`;
`interfaceId` (owned copy); `mmsPort`; `sweepIntervalMs`;
`ownedAcseAuthPassword` (owned copy or `NULL`); `discoveryHandle` (owned,
private `IedDiscoveryHandle`); `scanDispatcher` (borrowed, owned by the
caller's `ScanOrchestrationHandle`); `foundCallback` + `foundCallbackParam`
(borrowed); `seenSetLock` (`Semaphore(1)`, same binary-mutex convention as
`goose_subscriber`'s `targetStateLock`/`ipc_dispatcher_ring_buffer`'s own
lock); `seenHosts`/`seenCount`/`seenCapacity` (owned array of owned
strings, `realloc`-doubling from an initial capacity of 16); `thread`;
`volatile bool stopRequested`; `volatile bool exited`.

**`ScanOrchestrationWorker_create(scanId, request, discoveryConfigTemplate,
defaultSweepIntervalMs, scanDispatcher, foundCallback, foundCallbackParam,
outError)`** — validates `request` (non-NULL, non-empty `interfaceId`,
positive `mmsPort`) and `discoveryConfigTemplate`. Deep-copies
`interfaceId` and `acseAuthPassword` (the `ScanRequest` struct itself need
not outlive this call). Builds a per-scan `IedDiscoveryConfig` by copying
`discoveryConfigTemplate` verbatim except overriding `.acseAuthPassword`
with this worker's own owned copy, then calls `IedDiscovery_create` — this
is the one place `ScanRequest.acseAuthPassword` actually takes effect over
the template's own (ignored) field. No thread started yet; the only I/O at
this point is that one `IedDiscovery_create` allocation, mirroring every
sibling feature's "no I/O at create" contract. Returns `NULL` + `*outError`
on bad arguments, allocation failure, or `IedDiscovery_create` failure
(`ERR_DISCOVERY_CREATE_FAILED`).

**`ScanOrchestrationWorker_start`** — `Thread_create(sweepLoop, worker,
false)` + `Thread_start`. Non-blocking. Resets `stopRequested`/`exited` to
`false` first.

**`sweepLoop`** (static, the actual worker thread body):

1. `interruptibleSleep(worker, SCAN_ORCHESTRATION_INITIAL_SWEEP_GRACE_MS)`
   — a fixed **300ms** grace delay before the very first sweep. Reason,
   per the file's own comment: `scan_dispatcher` only binds on this scan's
   0→1 transition, so a client can't have a live connection to it before
   `START_SCAN`'s ack arrives — it force-reconnects only after receiving
   that ack. Both dispatcher websockets use start-from-now ring-buffer
   cursors (no backlog replay), so if the first sweep publishes a found
   host before that reconnect's handshake completes, the event is lost to
   that client forever (the dedup in `isHostNew` means it won't be
   republished on a later sweep, since the host is already in the
   seen-set). This grace delay only runs before the very first sweep,
   giving a well-behaved client's reconnect (typically single-digit ms on
   loopback) time to land first.
2. Loop while `!stopRequested`:
   - `IedDiscovery_scanSubnet(discoveryHandle, interfaceId, mmsPort, &err)`.
     A `NULL` result (bad interface / subnet too large / OOM) is tolerated
     gracefully — skip straight to the sleep step, never crash or exit the
     loop. Same tolerance pattern as `goose_subscriber`'s liveness loop
     tolerating a missed poll.
   - For each discovered host (`LinkedList` iteration, also checking
     `!stopRequested` per element): under `seenSetLock`, check
     `ScanOrchestrationUseCases_isHostNew` against the seen-set; if new,
     append it (still under the lock). **Publish outside the lock** — same
     pattern `goose_subscriber_connection.c`'s `livenessLoop` already
     establishes — and re-check `stopRequested` immediately before
     publishing (mirrors the documented `enableAllTargets` bugfix: "check
     `stopRequested` once per loop iteration"), avoiding delivery of a
     found-event after a stop was already requested mid-sweep. Publishing
     means: `ScanDispatcher_publishDeviceFound(scanDispatcher, scanId,
     host, mmsPort)`, then, if set, `foundCallback(foundCallbackParam,
     scanId, host, mmsPort)`.
   - `LinkedList_destroyDeep(results, free)`.
   - `interruptibleSleep(worker, sweepIntervalMs)`.
3. On loop exit, sets `worker->exited = true`.

**`interruptibleSleep(worker, totalMs)`** — sleeps in 20ms chunks,
re-checking `stopRequested` between chunks, so `_stop`'s bounded wait for
the loop to notice a stop request doesn't have to wait out a full
`sweepIntervalMs`. Same idiom as `goose_subscriber_connection.c`'s own
`interruptibleSleep` (`hal_thread.h` has no interruptible-sleep
primitive).

**`ScanOrchestrationWorker_stop`** — sets `stopRequested = true`, then
busy-waits (`Thread_sleep(20)` per iteration) until `worker->exited`.
Idempotent (no-op if already `stopRequested` or never started). **Known,
accepted limitation**: `IedDiscovery_scanSubnet` has no cancellation hook,
so if a sweep is in flight when this is called, this call blocks until
that sweep's own call returns on its own — see §5 for the worst-case
bound. Must be called from the caller's own thread, never from within
`foundCallback` (deadlock — same rule as every `_stop()` in this
codebase).

**`ScanOrchestrationWorker_destroy`** — implies `_stop()` first (idempotent,
safe even if never started), then `Thread_destroy`, `IedDiscovery_destroy`
on the private discovery handle, `Semaphore_destroy` on `seenSetLock`,
frees every seen-set string + the array itself, frees `interfaceId` and
`ownedAcseAuthPassword`, frees the worker. NULL-safe.

**`ScanOrchestrationWorker_scanId`** — trivial accessor.

**`ScanOrchestrationWorker_snapshotHosts(worker, outCount)`** — under
`seenSetLock`, `strdup`s every entry in the seen-set into a freshly
`malloc`'d array, releases the lock, returns it. `NULL`/`0` if empty (not
an error). Backs `ScanOrchestration_snapshotDiscoveredHosts` (§2 — unused
by any current caller). Guarded by the *worker's own* lock, **not** the
registry's lock.

**`ScanOrchestrationWorker_freeSnapshot`** — frees each string then the
array.

## 4. Threading & concurrency model

Three distinct lock/thread domains coexist in this layer:

- **Per-worker sweep thread** — one OS thread per active scan
  (`Thread_create`/`Thread_start` in `ScanOrchestrationWorker_start`),
  running `sweepLoop` for the worker's entire lifetime. Fully independent
  across scans: each worker owns its own `IedDiscoveryHandle` (so two
  scans never share `ied_discovery` state) and its own seen-set +
  `seenSetLock`. Nothing about one scan's sweep thread ever blocks or
  coordinates with another's.
- **Per-worker seen-set lock (`seenSetLock`)** — a private binary mutex
  guarding only that worker's `seenHosts`/`seenCount`/`seenCapacity`.
  Held only for the dedup check + array append inside `sweepLoop`, and for
  the full body of `ScanOrchestrationWorker_snapshotHosts`. Never held
  across the publish call (`ScanDispatcher_publishDeviceFound`/
  `foundCallback`) — those run lock-free, after release, with a
  `stopRequested` re-check immediately before the call.
- **Registry lock** — one process-wide lock (`sScanOrchestrationHandle`
  has exactly one registry) guarding the active-scan bookkeeping array and
  the scanId counter. This is the layer's central concurrency guarantee,
  and it's deliberately **two-phase**:
  - *Phase 1 (short, lock held throughout)*:
    `ScanOrchestrationRegistry_addAndMaybeStartDispatcher` — bounded work
    only (a counter check, at most one dispatcher bind+thread-create, one
    array append).
  - *Phase 2 (short lock, then unlocked slow work)*:
    `ScanOrchestrationRegistry_remove` takes the lock only long enough to
    splice the entry out of the array and hand back the worker pointer;
    the caller (`ScanOrchestration_stopScan`) then calls
    `ScanOrchestrationWorker_destroy` — which can block for the duration
    of an in-flight sweep — **entirely outside the lock**. Only after that
    potentially-slow call returns does the code take a *separate*, later,
    short critical section to stop the shared dispatcher if the scan
    count reached zero.

  The entire reason for this split: if the registry lock were held across
  `Worker_destroy`'s wait, one scan's slow stop (blocked inside a
  multi-second `IedDiscovery_scanSubnet` call) would serialize every other
  concurrent scan's start/stop behind it — directly violating "multiple
  concurrent scans must run independently," which is this layer's whole
  reason for existing.
- **Shared `ScanDispatcher`** — one instance per `ScanOrchestrationHandle`,
  refcounted purely by `ScanOrchestrationRegistry_activeCount` (no separate
  counter). Started on the 0→1 transition inside the registry's short
  locked phase; stopped on the 1→0 transition outside any lock, from
  `ScanOrchestration_stopScan`/`_destroy`. `scan_dispatcher` itself has no
  knowledge of scans/refcounting — this layer is entirely responsible for
  deciding when to start/stop it.

Net effect: two concurrent scans' `startScan`/`stopScan` calls can proceed
fully in parallel except for the two short, bounded critical sections
(registration and removal-splice) — never blocked on each other's sweep
work.

## 5. Known limitations / deliberate scope boundaries

- **Stop cannot interrupt an in-flight sweep.** `IedDiscovery_scanSubnet`
  has no cancellation hook. `ScanOrchestrationWorker_stop`/`_destroy`
  block until that in-flight call returns on its own. Worst case, per the
  worker header's own doc comment: `(tcpProbeTimeoutMs *
  ceil(hostCount / maxConcurrentTcpProbes)) + (mmsConnectTimeoutMs *
  tcpSurvivorCount)`, which at this feature's own defaults (500ms /
  64 / 3000ms) against a full /24 could be several seconds, and against a
  larger allowed subnet could stretch to tens of seconds.
- **`_snapshotDiscoveredHosts` is unused today.** Every current client
  (only `control_dispatcher`) gets results exclusively over
  `scan_dispatcher`'s websocket. This function exists for a hypothetical
  local in-process caller and carries its own caller contract (must not
  race a concurrent `_stopScan` for the same scanId — see §2).
- **300ms fixed startup grace delay** before the very first sweep of any
  worker (`SCAN_ORCHESTRATION_INITIAL_SWEEP_GRACE_MS`), to give a
  reconnecting websocket client time to land its handshake before the
  first sweep can publish a result that would otherwise be lost forever
  (start-from-now ring buffers, no backlog replay — see §3's `sweepLoop`
  breakdown). Not configurable.
- **No cross-scan dedup.** Each scan's seen-set is entirely private — two
  concurrent scans covering overlapping address space will each publish
  the same discovered host independently (as different `scanId`s in the
  `SCAN_RESULT` envelope, so this is by design, not a bug).
- **Seen-set is never pruned or reset.** A host that stops responding
  remains in the seen-set forever for that scan's lifetime — this layer
  has no notion of "device disappeared," only "device newly appeared."
- **A `_startScan` failure after registration rolls back cleanly but is
  synchronous** — the caller's `_startScan` call absorbs the cost of that
  rollback (dispatcher stop if applicable, worker destroy) before
  returning an error.

## 6. Cross-feature dependencies

- **`ied_discovery`** — left entirely untouched, consumed purely through
  its public API (`IedDiscoveryHandle`, `IedDiscovery_create/_destroy`,
  `IedDiscovery_scanSubnet`, `IedDiscoveryConfig`/`IedDiscoveryError`).
  Each worker owns a private `IedDiscoveryHandle` — this layer never
  shares one across scans and never reaches into `ied_discovery`'s
  domain/data internals.
- **`scan_dispatcher`** — the transport. `scan_orchestration` owns its
  entire lifecycle: one shared `ScanDispatcherHandle` per
  `ScanOrchestrationHandle`, created (alloc-only) at `_create`, started on
  the first scan (0→1), stopped when the last scan stops (1→0), destroyed
  at `_destroy`. `scan_dispatcher` itself has zero knowledge of scans,
  interfaces, or refcounting — purely a ring-buffer + libwebsockets
  transport with one typed publish call
  (`ScanDispatcher_publishDeviceFound(handle, scanId, host, mmsPort)`),
  matching CLAUDE.md's description of it as "near-verbatim structural
  duplicate of `ipc_dispatcher`."
- **`control_dispatcher`** — the only current driver of this layer, via
  its `START_SCAN {interfaceId, mmsPort, sweepIntervalMs?}` →
  `{scanId}` and `STOP_SCAN {scanId}` → `{scanId}` commands (see the root
  CLAUDE.md's Commands section). `control_dispatcher` holds the one
  `ScanOrchestrationHandle` for the daemon's lifetime, created in
  `main.c`. `START_SCAN`/`STOP_SCAN` deliberately carry no
  `acseAuthPassword` field at the control-plane level today, even though
  `ScanRequest.acseAuthPassword` supports per-scan credentials internally.
- **`main.c`** — creates the one `ScanOrchestrationHandle` alongside
  `device_manager` and `control_dispatcher`, tears it down (via
  `ScanOrchestration_destroy`) on `SIGINT`/`SIGTERM` shutdown.

## 7. Tests

- **`tests/scan_orchestration/test_scan_orchestration_usecases.c`** — pure
  unit tests of `ScanOrchestrationUseCases_isHostNew`: true on an empty
  seen-set, false when present, true when not present, exact-string match
  with no dotted-quad normalization (`"192.168.1.1"` vs
  `"192.168.001.001"` treated as distinct hosts), true on `NULL`/empty
  host input.
- **`tests/scan_orchestration/test_scan_orchestration_api.c`** —
  argument-validation-only wiring tests, plus a full start/stop lifecycle
  driven against a deliberately nonexistent interface (`"nonexistent0"`):
  every sweep fails fast with `IED_DISCOVERY_ERR_INTERFACE_NOT_FOUND` and
  is tolerated gracefully by `sweepLoop`, which proves scanId monotonicity
  and refcounted dispatcher start/stop without needing any real reachable
  network (same reasoning `test_ied_discovery_api.c` uses). Uses a
  dedicated high port (18866) to avoid clashing with a real daemon
  instance during test runs. Covers: config defaults match documented
  values; `_create` succeeds with `NULL` config; `_destroy` is NULL-safe;
  `_startScan` rejects `NULL` handle/request/empty `interfaceId`;
  `_stopScan`/`_snapshotDiscoveredHosts` return `SCAN_NOT_FOUND` on an
  unknown scanId; start-then-stop-then-restart produces a strictly
  monotonic scanId; two concurrent scans share the dispatcher and can be
  stopped independently (stopping one doesn't affect the other's
  findability); stopping the same scanId twice returns `SCAN_NOT_FOUND`
  the second time; `_destroy` drains a still-active scan without hanging
  or crashing. **Never a real scan** — this suite proves sequencing and
  registry correctness against fake/nonexistent interfaces only, per
  CLAUDE.md's Testing section.
- **`integration_tests/scan_orchestration/e2e_test_scan_orchestration.c`**
  — real E2E, no `sudo` needed. Drives the real
  `ScanOrchestration_startScan`/`_stopScan` against the real `"lo"`
  interface with two different `mmsPort`s (102, 103), on a dedicated test
  port (18867). Sweeps against `"lo"` are expected to fail
  (`IedDiscovery`'s `/8` exceeds `maxHosts`) and are tolerated gracefully
  — this test proves sequencing/refcounting/threading, not sweep success.
  Liveness of the shared dispatcher's websocket is probed with a
  hand-rolled minimal RFC6455 handshake (connect + HTTP Upgrade, expecting
  a `101` response) — same helper shape as `integration_tests/
  scan_dispatcher/`'s own test client. Two test cases:
  - `test_fullRefcountedLifecycle_overLoopback` — the full
    0→1→2→1→0→1-rebind refcounting cycle: dispatcher unbound before any
    scan; bound after scan #1 starts (0→1); still serving and unrebound
    after scan #2 starts (1→2, different `mmsPort`, same interface);
    still serving after stopping scan #1 (2→1, scan #2 still active);
    torn down after stopping scan #2, the last one (1→0); and a third
    scan cleanly rebinds the exact same port afterward, with a
    fresh scanId distinct from both prior ones.
  - `test_stopScan_genuinelyBlocksUntilWorkerThreadExits` — starts a scan,
    lets at least one sweep pass, then asserts that `_stopScan` returning
    means the worker thread has *genuinely* exited (not just marked
    stopped) by checking the dispatcher is no longer serving immediately
    after `_stopScan` returns, with no extra wait.
